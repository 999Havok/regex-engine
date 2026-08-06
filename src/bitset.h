/*
 * bitset.h -- fixed-size heap-allocated bit set of position ids.
 *
 * The classic lab implementation of the ASU algorithm packs a position set
 * into a single unsigned long long, which caps a pattern at 64 positions.
 * We instead store a heap array of 64-bit words so the only limit on pattern
 * size is available memory.
 */
#ifndef REGEX_BITSET_H
#define REGEX_BITSET_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t   nwords;   /* number of 64-bit words */
    uint64_t *w;       /* nwords words, or NULL when nbits == 0 */
} BitSet;

/* Allocate a set able to hold ids 0..nbits-1, all initially clear.
 * Returns 0 on success, -1 on allocation failure. */
int  bitset_init(BitSet *b, size_t nbits);
void bitset_free(BitSet *b);

void bitset_clear(BitSet *b);
void bitset_add(BitSet *b, size_t i);
int  bitset_test(const BitSet *b, size_t i);

/* dst |= src ; sets must have the same nwords. Returns 1 if dst changed. */
int  bitset_union(BitSet *dst, const BitSet *src);
void bitset_copy(BitSet *dst, const BitSet *src);
int  bitset_equal(const BitSet *a, const BitSet *b);
int  bitset_empty(const BitSet *b);
size_t bitset_count(const BitSet *b);

/*
 * Iterate set members in increasing order. Start with *it = 0; each call
 * returns the next member id and advances *it, or returns -1 when exhausted.
 *
 *   size_t it = 0; long p;
 *   while ((p = bitset_next(&b, &it)) >= 0) { ... use p ... }
 */
long bitset_next(const BitSet *b, size_t *it);

#endif /* REGEX_BITSET_H */
