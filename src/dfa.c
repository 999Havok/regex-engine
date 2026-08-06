/*
 * dfa.c -- subset construction directly from the followpos table.
 *
 * A DFA state is a set of leaf positions. The start state is firstpos of the
 * augmented root. From a state S, the move on a symbol a is
 *
 *     move(S, a) = union of followpos(p) for every position p in S that a
 *                  can "consume".
 *
 * Bytes 0..255 are consumed by charset leaves. The two anchor symbols get a
 * "pass-through" transition instead: on SYM_START the anchor-^ leaves advance
 * to their followpos while every other position stays put (a zero-width
 * assertion consumes nothing else), and likewise SYM_END with anchor-$ leaves.
 * When no anchor leaf is present this is a self-loop, so feeding a boundary
 * symbol to an anchorless pattern is a no-op -- exactly the semantics we want.
 * See match.c for when the boundary symbols are actually fed.
 *
 * State 0 is always the empty set (the dead state); any byte move with no
 * consuming position lands there, keeping the transition table total, which
 * Hopcroft minimisation relies on.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    BitSet *sets;     /* [cap] position set per state */
    int    *trans;    /* [cap * NSYM] */
    int     count;
    int     cap;
    int     numpos;
} Builder;

static int builder_grow(Builder *b) {
    int ncap = b->cap ? b->cap * 2 : 16;
    BitSet *ns = realloc(b->sets, ncap * sizeof(BitSet));
    if (!ns) return -1;
    b->sets = ns;
    int *nt = realloc(b->trans, (size_t)ncap * NSYM * sizeof(int));
    if (!nt) return -1;
    b->trans = nt;
    b->cap = ncap;
    return 0;
}

/* Return the id of the state equal to `set`, adding a fresh state if none
 * exists yet. Returns -1 on allocation failure. */
static int intern(Builder *b, const BitSet *set) {
    for (int i = 0; i < b->count; i++)
        if (bitset_equal(&b->sets[i], set)) return i;

    if (b->count == b->cap && builder_grow(b) != 0) return -1;
    int id = b->count++;
    if (bitset_init(&b->sets[id], b->numpos) != 0) return -1;
    bitset_copy(&b->sets[id], set);
    for (int s = 0; s < NSYM; s++) b->trans[id * NSYM + s] = -1;
    return id;
}

static void compute_move(const Positions *P, const BitSet *S, int sym, BitSet *out) {
    bitset_clear(out);
    size_t it = 0; long p;
    if (sym < 256) {
        while ((p = bitset_next(S, &it)) >= 0)
            if (P->kind[p] == LEAF_CHARSET && ((P->set[p][sym >> 3] >> (sym & 7)) & 1))
                bitset_union(out, &P->followpos[p]);
    } else if (sym == SYM_START) {
        while ((p = bitset_next(S, &it)) >= 0) {
            if (P->kind[p] == LEAF_ANCHOR_START) bitset_union(out, &P->followpos[p]);
            else bitset_add(out, p);          /* pass through */
        }
    } else { /* SYM_END */
        while ((p = bitset_next(S, &it)) >= 0) {
            if (P->kind[p] == LEAF_ANCHOR_END) bitset_union(out, &P->followpos[p]);
            else bitset_add(out, p);
        }
    }
}

int dfa_build(const Positions *P, DFA *out) {
    memset(out, 0, sizeof(*out));

    Builder b = {0};
    b.numpos = P->numpos;

    BitSet empty, move;
    if (bitset_init(&empty, P->numpos) != 0) return -1;
    if (bitset_init(&move, P->numpos) != 0) { bitset_free(&empty); return -1; }

    int rc = -1;
    int dead = intern(&b, &empty);          /* state 0 = dead */
    int start = intern(&b, &P->start);
    if (dead < 0 || start < 0) goto done;

    /* Process states in order; intern() appends newly discovered ones. */
    for (int i = 0; i < b.count; i++) {
        for (int sym = 0; sym < NSYM; sym++) {
            compute_move(P, &b.sets[i], sym, &move);
            int t = intern(&b, &move);
            if (t < 0) goto done;
            b.trans[i * NSYM + sym] = t;      /* b.sets may have moved */
        }
    }

    /* Hand the built table to the caller. */
    out->nstates = b.count;
    out->start   = start;
    out->dead    = dead;
    out->trans   = malloc((size_t)b.count * NSYM * sizeof(int));
    out->accept  = malloc((size_t)b.count);
    if (!out->trans || !out->accept) { dfa_free(out); goto done; }

    memcpy(out->trans, b.trans, (size_t)b.count * NSYM * sizeof(int));
    for (int i = 0; i < b.count; i++)
        out->accept[i] = (unsigned char)bitset_test(&b.sets[i], P->endpos);

    /* Transfer the position-set labels to the caller (for regexdump). */
    out->sets = b.sets;
    b.sets = NULL;
    rc = 0;

done:
    if (b.sets) {
        for (int i = 0; i < b.count; i++) bitset_free(&b.sets[i]);
        free(b.sets);
    }
    free(b.trans);
    bitset_free(&empty);
    bitset_free(&move);
    return rc;
}

void dfa_free(DFA *d) {
    if (!d) return;
    if (d->sets) {
        for (int i = 0; i < d->nstates; i++) bitset_free(&d->sets[i]);
        free(d->sets);
    }
    free(d->trans);
    free(d->accept);
    d->trans = NULL;
    d->accept = NULL;
    d->sets = NULL;
    d->nstates = 0;
}
