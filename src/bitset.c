#include "bitset.h"

#include <stdlib.h>
#include <string.h>

int bitset_init(BitSet *b, size_t nbits) {
    b->nwords = (nbits + 63) / 64;
    if (b->nwords == 0) {
        b->w = NULL;
        return 0;
    }
    b->w = calloc(b->nwords, sizeof(uint64_t));
    return b->w ? 0 : -1;
}

void bitset_free(BitSet *b) {
    if (!b) return;
    free(b->w);
    b->w = NULL;
    b->nwords = 0;
}

void bitset_clear(BitSet *b) {
    if (b->w) memset(b->w, 0, b->nwords * sizeof(uint64_t));
}

void bitset_add(BitSet *b, size_t i) {
    b->w[i >> 6] |= (uint64_t)1 << (i & 63);
}

int bitset_test(const BitSet *b, size_t i) {
    size_t word = i >> 6;
    if (word >= b->nwords) return 0;
    return (b->w[word] >> (i & 63)) & 1u;
}

int bitset_union(BitSet *dst, const BitSet *src) {
    int changed = 0;
    for (size_t i = 0; i < dst->nwords; i++) {
        uint64_t before = dst->w[i];
        uint64_t after = before | src->w[i];
        if (after != before) {
            dst->w[i] = after;
            changed = 1;
        }
    }
    return changed;
}

void bitset_copy(BitSet *dst, const BitSet *src) {
    memcpy(dst->w, src->w, dst->nwords * sizeof(uint64_t));
}

int bitset_equal(const BitSet *a, const BitSet *b) {
    if (a->nwords != b->nwords) return 0;
    return memcmp(a->w, b->w, a->nwords * sizeof(uint64_t)) == 0;
}

int bitset_empty(const BitSet *b) {
    for (size_t i = 0; i < b->nwords; i++)
        if (b->w[i]) return 0;
    return 1;
}

size_t bitset_count(const BitSet *b) {
    size_t n = 0;
    for (size_t i = 0; i < b->nwords; i++) {
        uint64_t x = b->w[i];
        while (x) { x &= x - 1; n++; }
    }
    return n;
}

long bitset_next(const BitSet *b, size_t *it) {
    size_t i = *it;
    while (i < b->nwords * 64) {
        size_t word = i >> 6;
        uint64_t bits = b->w[word] >> (i & 63);
        if (bits == 0) {
            i = (word + 1) * 64;      /* skip to next word */
            continue;
        }
        while ((bits & 1u) == 0) { bits >>= 1; i++; }
        *it = i + 1;
        return (long)i;
    }
    *it = i;
    return -1;
}
