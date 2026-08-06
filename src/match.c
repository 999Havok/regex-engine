/*
 * match.c -- compile-pipeline glue and the matching loops.
 *
 * regex_compile runs the whole pipeline: parse -> positions -> subset
 * construction -> Hopcroft minimisation, keeping only the minimised transition
 * table for matching. Matching is then a plain table walk: one array lookup per
 * input byte, so it is worst-case linear in the length of the text with no
 * backtracking.
 *
 * The anchor symbols are fed at the boundaries: SYM_START before the first byte
 * (only when we are truly at text position 0) and SYM_END after the last byte
 * (only when we are truly at the end). For anchorless patterns those columns
 * are self-loops (see dfa.c), so feeding them changes nothing.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static char compile_err[160];

Regex *regex_compile(const char *pattern, const char **err) {
    if (err) *err = NULL;
    if (!pattern) {
        if (err) *err = "null pattern";
        return NULL;
    }

    Node *tree = parse_regex(pattern, err);
    if (!tree) return NULL;                 /* *err already set by the parser */

    Regex *re = NULL;
    Positions P;
    DFA raw;
    int have_pos = 0, have_raw = 0;

    if (positions_build(tree, &P) != 0) goto oom;
    have_pos = 1;
    if (dfa_build(&P, &raw) != 0) goto oom;
    have_raw = 1;

    re = calloc(1, sizeof(Regex));
    if (!re) goto oom;
    if (dfa_minimize(&raw, &re->dfa) != 0) { free(re); re = NULL; goto oom; }

    re->raw_states = raw.nstates;
    re->min_states = re->dfa.nstates;

    dfa_free(&raw);
    positions_free(&P);
    ast_free(tree);
    return re;

oom:
    if (have_raw) dfa_free(&raw);
    if (have_pos) positions_free(&P);
    ast_free(tree);
    snprintf(compile_err, sizeof(compile_err), "out of memory");
    if (err) *err = compile_err;
    return NULL;
}

void regex_free(Regex *re) {
    if (!re) return;
    dfa_free(&re->dfa);
    free(re);
}

/* One table step. `sym` is a byte 0..255 or SYM_START / SYM_END. */
static int step(const DFA *d, int state, int sym) {
    return d->trans[state * NSYM + sym];
}

int regex_match(const Regex *re, const char *text) {
    if (!re || !text) return 0;
    const DFA *d = &re->dfa;

    int s = step(d, d->start, SYM_START);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        s = step(d, s, *p);
        if (s == d->dead) return 0;         /* nothing can revive a dead state */
    }
    s = step(d, s, SYM_END);
    return d->accept[s] ? 1 : 0;
}

int regex_search(const Regex *re, const char *text, size_t *start, size_t *end) {
    if (!re || !text) return 0;
    const DFA *d = &re->dfa;
    size_t n = strlen(text);
    const unsigned char *t = (const unsigned char *)text;

    /* Leftmost: try each start position in increasing order and return the
     * first that yields any match. Longest: at that start, keep scanning and
     * remember the furthest accepting end reached. */
    for (size_t i = 0; i <= n; i++) {
        int s = d->start;
        if (i == 0) s = step(d, s, SYM_START);

        long best = -1;                     /* furthest accepting end, or none */
        size_t j = i;
        for (;;) {
            int at_end = (j == n);
            int cs = at_end ? step(d, s, SYM_END) : s;
            if (d->accept[cs]) best = (long)j;
            if (at_end) break;
            int ns = step(d, s, t[j]);
            if (ns == d->dead) break;
            s = ns;
            j++;
        }
        if (best >= 0) {
            if (start) *start = i;
            if (end)   *end = (size_t)best;
            return 1;
        }
    }
    return 0;
}

int regex_debug_raw_states(const Regex *re) { return re ? re->raw_states : 0; }
int regex_debug_min_states(const Regex *re) { return re ? re->min_states : 0; }
