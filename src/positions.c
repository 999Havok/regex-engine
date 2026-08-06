/*
 * positions.c -- the heart of the ASU direct-DFA method.
 *
 * We conceptually augment the parsed tree r into  (r . #)  where # is a unique
 * endmarker leaf. Then, for every node, we compute three things bottom-up:
 *
 *   nullable(n)  -- can the subexpression match the empty string?
 *   firstpos(n)  -- leaf positions that can begin a match of the subexpression
 *   lastpos(n)   -- leaf positions that can end a match of the subexpression
 *
 * and from those we fill the followpos table over leaf positions. followpos(i)
 * is the set of positions that can immediately follow position i in some string
 * of the whole language. Two rules generate it (ASU 3.9):
 *
 *   (1) For a concat node c1.c2: every position in lastpos(c1) can be followed
 *       by every position in firstpos(c2).
 *   (2) For a star node (c)*: every position in lastpos(c) can be followed by
 *       every position in firstpos(c) (the loop-back edge).
 *
 * firstpos of the augmented root becomes the DFA start state, and any state
 * containing the endmarker position is accepting.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static int count_leaves(const Node *n) {
    if (!n) return 0;
    switch (n->kind) {
    case N_CHARSET:
    case N_ANCHOR_START:
    case N_ANCHOR_END:
        return 1;
    case N_EPSILON:
        return 0;                 /* epsilon is not a position */
    default:
        return count_leaves(n->left) + count_leaves(n->right);
    }
}

/* Assign position ids left-to-right and record each leaf's kind/byte-set. */
static void assign_positions(Node *n, Positions *P, int *next) {
    if (!n) return;
    switch (n->kind) {
    case N_CHARSET:
        n->pos = (*next)++;
        P->kind[n->pos] = LEAF_CHARSET;
        memcpy(P->set[n->pos], n->set, 32);
        break;
    case N_ANCHOR_START:
        n->pos = (*next)++;
        P->kind[n->pos] = LEAF_ANCHOR_START;
        break;
    case N_ANCHOR_END:
        n->pos = (*next)++;
        P->kind[n->pos] = LEAF_ANCHOR_END;
        break;
    case N_EPSILON:
        break;
    default:
        assign_positions(n->left, P, next);
        assign_positions(n->right, P, next);
    }
}

/* followpos[i] |= src, for every i in `lasts`. */
static void follow_from(Positions *P, const BitSet *lasts, const BitSet *src) {
    size_t it = 0; long i;
    while ((i = bitset_next(lasts, &it)) >= 0)
        bitset_union(&P->followpos[i], src);
}

/* Post-order pass: allocate and fill first/last, set nullable, grow followpos.
 * Returns 0 on success, -1 on allocation failure. */
static int compute(Node *n, Positions *P) {
    if (bitset_init(&n->first, P->numpos) != 0) return -1;
    if (bitset_init(&n->last, P->numpos) != 0) return -1;

    switch (n->kind) {
    case N_CHARSET:
    case N_ANCHOR_START:
    case N_ANCHOR_END:
        n->nullable = 0;
        bitset_add(&n->first, n->pos);
        bitset_add(&n->last, n->pos);
        break;

    case N_EPSILON:
        n->nullable = 1;
        break;

    case N_CONCAT:
        if (compute(n->left, P) != 0 || compute(n->right, P) != 0) return -1;
        n->nullable = n->left->nullable && n->right->nullable;

        bitset_union(&n->first, &n->left->first);
        if (n->left->nullable) bitset_union(&n->first, &n->right->first);

        bitset_union(&n->last, &n->right->last);
        if (n->right->nullable) bitset_union(&n->last, &n->left->last);

        /* rule (1): lastpos(left) --> firstpos(right) */
        follow_from(P, &n->left->last, &n->right->first);
        break;

    case N_ALT:
        if (compute(n->left, P) != 0 || compute(n->right, P) != 0) return -1;
        n->nullable = n->left->nullable || n->right->nullable;
        bitset_union(&n->first, &n->left->first);
        bitset_union(&n->first, &n->right->first);
        bitset_union(&n->last, &n->left->last);
        bitset_union(&n->last, &n->right->last);
        break;

    case N_STAR:
        if (compute(n->left, P) != 0) return -1;
        n->nullable = 1;
        bitset_union(&n->first, &n->left->first);
        bitset_union(&n->last, &n->left->last);
        /* rule (2): lastpos(child) --> firstpos(child) */
        follow_from(P, &n->left->last, &n->left->first);
        break;

    case N_END:
        /* endmarker never appears inside the parsed tree; handled separately */
        break;
    }
    return 0;
}

int positions_build(Node *tree, Positions *P) {
    memset(P, 0, sizeof(*P));

    int nleaves = count_leaves(tree);
    P->numpos = nleaves + 1;          /* +1 for the endmarker */
    P->endpos = nleaves;              /* endmarker is the last position */

    P->kind      = calloc(P->numpos, sizeof(LeafKind));
    P->set       = calloc(P->numpos, sizeof(*P->set));
    P->followpos = calloc(P->numpos, sizeof(BitSet));
    if (!P->kind || !P->set || !P->followpos) goto fail;

    for (int i = 0; i < P->numpos; i++)
        if (bitset_init(&P->followpos[i], P->numpos) != 0) goto fail;
    if (bitset_init(&P->start, P->numpos) != 0) goto fail;

    int next = 0;
    assign_positions(tree, P, &next);
    P->kind[P->endpos] = LEAF_END;

    if (compute(tree, P) != 0) goto fail;

    /* Augmented root is (tree . #): apply the concat rule with the endmarker,
     * and build the start state = firstpos of that root. */
    BitSet endset;
    if (bitset_init(&endset, P->numpos) != 0) goto fail;
    bitset_add(&endset, P->endpos);
    follow_from(P, &tree->last, &endset);
    bitset_free(&endset);

    bitset_union(&P->start, &tree->first);
    if (tree->nullable) bitset_add(&P->start, P->endpos);

    return 0;

fail:
    positions_free(P);
    return -1;
}

void positions_free(Positions *P) {
    if (P->followpos) {
        for (int i = 0; i < P->numpos; i++) bitset_free(&P->followpos[i]);
        free(P->followpos);
    }
    free(P->kind);
    free(P->set);
    bitset_free(&P->start);
    memset(P, 0, sizeof(*P));
}
