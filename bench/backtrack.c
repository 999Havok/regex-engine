/*
 * backtrack.c -- naive recursive-backtracking matcher (benchmark foil).
 *
 * It reuses this project's parser to get the same syntax tree, then matches it
 * with continuation-passing recursion: to match a concatenation it matches the
 * first part and, for each way that succeeds, recursively tries the rest. A
 * star tries "one more repetition" before "stop", and backtracks on failure.
 *
 * For an unambiguous pattern that is fine. For (a+)+b there are exponentially
 * many ways to split a run of a's between the inner and outer repetition, and
 * the matcher tries them all before discovering there is no trailing b -- the
 * classic catastrophic blowup. A step budget stops it so the benchmark itself
 * cannot hang; exceeding the budget is reported as -1 ("blew up").
 */
#include "internal.h"      /* Node, parse_regex, ast_free */
#include "backtrack.h"

#include <stddef.h>

/* Global step budget so a blowup cannot hang the benchmark. */
static unsigned long bt_steps;
static int           bt_overflow;
#define BT_BUDGET 300000000UL

typedef struct {
    const unsigned char *s;
    size_t n;
} Ctx;

typedef int (*Cont)(void *kc, size_t i);

static int bt(const Node *node, Ctx *c, size_t i, Cont k, void *kc);

/* Continuation that matches `right` (then the outer continuation). */
typedef struct { const Node *right; Ctx *c; Cont k; void *kc; } ConcatK;
static int concat_k(void *p, size_t i) {
    ConcatK *ck = p;
    return bt(ck->right, ck->c, i, ck->k, ck->kc);
}

/* Continuation that, after one star iteration, re-enters the star. */
typedef struct { const Node *star; Ctx *c; Cont k; void *kc; size_t from; } StarK;
static int star_k(void *p, size_t i) {
    StarK *sk = p;
    if (i == sk->from) return 0;          /* empty match: stop, avoid infinite loop */
    return bt(sk->star, sk->c, i, sk->k, sk->kc);
}

static int in_set(const Node *n, int ch) {
    return (n->set[(ch & 0xff) >> 3] >> (ch & 7)) & 1;
}

static int bt(const Node *node, Ctx *c, size_t i, Cont k, void *kc) {
    if (bt_overflow) return 0;
    if (++bt_steps > BT_BUDGET) { bt_overflow = 1; return 0; }

    switch (node->kind) {
    case N_CHARSET:
        if (i < c->n && in_set(node, c->s[i])) return k(kc, i + 1);
        return 0;
    case N_ANCHOR_START:
        return (i == 0) ? k(kc, i) : 0;
    case N_ANCHOR_END:
        return (i == c->n) ? k(kc, i) : 0;
    case N_EPSILON:
        return k(kc, i);
    case N_CONCAT: {
        ConcatK ck = { node->right, c, k, kc };
        return bt(node->left, c, i, concat_k, &ck);
    }
    case N_ALT:
        if (bt(node->left, c, i, k, kc)) return 1;
        return bt(node->right, c, i, k, kc);
    case N_STAR: {
        StarK sk = { node, c, k, kc, i };
        if (bt(node->left, c, i, star_k, &sk)) return 1;  /* greedy: one more */
        return k(kc, i);                                   /* or zero times */
    }
    case N_END:
        return k(kc, i);
    }
    return 0;
}

static int accept_end(void *p, size_t i) {
    Ctx *c = p;
    return i == c->n;
}

int backtrack_match(const char *pattern, const char *text, const char **err) {
    Node *tree = parse_regex(pattern, err);
    if (!tree) return -1;

    Ctx c = { (const unsigned char *)text, 0 };
    while (text[c.n]) c.n++;

    bt_steps = 0;
    bt_overflow = 0;
    int r = bt(tree, &c, 0, accept_end, &c);
    ast_free(tree);
    if (bt_overflow) return -1;
    return r;
}
