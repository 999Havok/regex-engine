/*
 * minimize.c -- Hopcroft DFA minimisation.
 *
 * We start from the coarsest partition that respects acceptance -- accepting
 * states in one block, non-accepting in another -- and repeatedly refine it.
 * Two states may share a block only while no observed transition distinguishes
 * them; the refinement splits a block the moment a symbol sends part of it into
 * a distinguishing block.
 *
 * The refinement loop is the classic Hopcroft worklist:
 *
 *   take a splitter block A off the worklist
 *   for each symbol c:
 *     X = { states whose c-transition lands in A }        (the preimage)
 *     for every block Y that X cuts (some in X, some not):
 *       split Y into Y∩X and Y\X
 *       if Y was on the worklist, put both halves on it;
 *       otherwise put only the smaller half on it
 *
 * Enqueuing only the smaller half is what gives Hopcroft its O(n log n |Σ|)
 * bound: any state is in the smaller half at most log n times.
 *
 * The transition table is total (dfa.c guarantees a dead state), so every
 * c-transition lands in exactly one block and the preimage is well defined.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct { int *data; int len, cap; } IntVec;

static int vec_push(IntVec *v, int x) {
    if (v->len == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        int *nd = realloc(v->data, nc * sizeof(int));
        if (!nd) return -1;
        v->data = nd;
        v->cap = nc;
    }
    v->data[v->len++] = x;
    return 0;
}

int dfa_minimize(const DFA *in, DFA *out) {
    memset(out, 0, sizeof(*out));
    int n = in->nstates;

    /* Partition state. A block id in [0,nblocks); at most n blocks. */
    IntVec *blocks = calloc(n, sizeof(IntVec));
    int    *where  = malloc(n * sizeof(int));       /* state -> block id */
    int    *in_w   = calloc(n, sizeof(int));        /* block on worklist? */
    IntVec  work   = {0};                           /* worklist of block ids */
    IntVec *bucket = calloc(n, sizeof(IntVec));     /* per-block preimage members */
    int    *touched = malloc(n * sizeof(int));      /* blocks cut this round */
    int    *xlist   = malloc(n * sizeof(int));      /* preimage members */
    int    *inA     = malloc(n * sizeof(int));      /* membership of splitter A */
    int     rc = -1;
    int     nblocks = 0;

    if (!blocks || !where || !in_w || !bucket || !touched || !xlist || !inA) goto done;

    /* Initial partition: accepting vs non-accepting (drop empty blocks). */
    int acc = -1, nonacc = -1;
    for (int q = 0; q < n; q++) {
        int want = in->accept[q] ? 1 : 0;
        int *slot = want ? &acc : &nonacc;
        if (*slot < 0) *slot = nblocks++;
        if (vec_push(&blocks[*slot], q) != 0) goto done;
        where[q] = *slot;
    }
    for (int b = 0; b < nblocks; b++) { in_w[b] = 1; if (vec_push(&work, b) != 0) goto done; }

    while (work.len > 0) {
        int A = work.data[--work.len];
        in_w[A] = 0;

        /* Snapshot A's membership -- A itself may be split later this round. */
        memset(inA, 0, n * sizeof(int));
        for (int i = 0; i < blocks[A].len; i++) inA[blocks[A].data[i]] = 1;

        for (int c = 0; c < NSYM; c++) {
            /* Preimage X = states whose c-move lands inside A. */
            int xn = 0;
            for (int q = 0; q < n; q++)
                if (inA[in->trans[q * NSYM + c]]) xlist[xn++] = q;
            if (xn == 0) continue;

            /* Bucket the preimage by block; remember which blocks were cut. */
            int tn = 0;
            for (int i = 0; i < xn; i++) {
                int q = xlist[i], b = where[q];
                if (bucket[b].len == 0) touched[tn++] = b;
                if (vec_push(&bucket[b], q) != 0) goto done;
            }

            for (int i = 0; i < tn; i++) {
                int b = touched[i];
                int inCount = bucket[b].len, total = blocks[b].len;
                if (inCount == total) { bucket[b].len = 0; continue; }  /* not cut */

                /* Split: move the X-members of b into a new block Z. */
                int Z = nblocks++;
                for (int k = 0; k < bucket[b].len; k++) {
                    int q = bucket[b].data[k];
                    if (vec_push(&blocks[Z], q) != 0) goto done;
                    where[q] = Z;
                }
                /* Rebuild b keeping only the states not moved to Z. */
                IntVec kept = {0};
                for (int k = 0; k < blocks[b].len; k++) {
                    int q = blocks[b].data[k];
                    if (where[q] != Z) { if (vec_push(&kept, q) != 0) { free(kept.data); goto done; } }
                }
                free(blocks[b].data);
                blocks[b] = kept;

                if (in_w[b]) {
                    in_w[Z] = 1;
                    if (vec_push(&work, Z) != 0) goto done;
                } else {
                    int small = (blocks[b].len <= blocks[Z].len) ? b : Z;
                    in_w[small] = 1;
                    if (vec_push(&work, small) != 0) goto done;
                }
                bucket[b].len = 0;
            }
        }
    }

    /* Build the minimised DFA: one state per block. */
    out->nstates = nblocks;
    out->start   = where[in->start];
    out->dead    = where[in->dead];
    out->trans   = malloc((size_t)nblocks * NSYM * sizeof(int));
    out->accept  = malloc((size_t)nblocks);
    if (!out->trans || !out->accept) { dfa_free(out); goto done; }

    for (int b = 0; b < nblocks; b++) {
        int rep = blocks[b].data[0];                     /* any representative */
        out->accept[b] = in->accept[rep];
        for (int c = 0; c < NSYM; c++)
            out->trans[b * NSYM + c] = where[in->trans[rep * NSYM + c]];
    }
    rc = 0;

done:
    for (int b = 0; b < n; b++) { free(blocks[b].data); free(bucket[b].data); }
    free(blocks); free(where); free(in_w); free(work.data);
    free(bucket); free(touched); free(xlist); free(inA);
    return rc;
}
