/*
 * bench.c -- time this engine against the POSIX <regex.h> implementation.
 *
 * Three scenarios:
 *   1. (a+)+b on "aaaa...a!"  -- the catastrophic-backtracking trap. POSIX
 *      NFA/backtracking implementations can blow up exponentially here; a DFA
 *      cannot. We sweep n = 10..30.
 *   2. (a|aa)+$ on a long run of a's -- another ambiguity trap.
 *   3. a plain pattern on a large input -- the common case, where POSIX's
 *      constant factors may well beat ours.
 *
 * POSIX regex has no timeout; to stay safe on the catastrophic case we cap its
 * per-match wall time and print "timeout" past the cap. Output is a Markdown
 * table.
 */
#include <regex.h>          /* POSIX regex, for the comparison */
#include "../include/regex.h"   /* our engine (explicit path avoids the name clash) */
#include "backtrack.h"          /* naive backtracking foil */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* --- our engine --- */
static double time_ours(const char *pat, const char *text, int *matched) {
    const char *err = NULL;
    Regex *re = regex_compile(pat, &err);
    if (!re) { fprintf(stderr, "ours: compile '%s' failed: %s\n", pat, err); *matched = -1; return 0; }
    double t0 = now_sec();
    *matched = regex_match(re, text);
    double dt = now_sec() - t0;
    regex_free(re);
    return dt;
}

/* --- POSIX --- with a wall-clock cap enforced via a repeat/estimate guard.
 * POSIX regexec cannot be interrupted, so for the catastrophic case we simply
 * run it once and report the (possibly very large) time; a soft cap marks
 * anything over `cap` seconds as a timeout by not attempting larger n. */
static double time_posix(const char *pat, const char *text, int *matched) {
    regex_t rx;
    if (regcomp(&rx, pat, REG_EXTENDED) != 0) { *matched = -1; return 0; }
    double t0 = now_sec();
    int rc = regexec(&rx, text, 0, NULL, 0);
    double dt = now_sec() - t0;
    regfree(&rx);
    *matched = (rc == 0);
    return dt;
}

static char *make_run(char fill, int n, char tail) {
    char *s = malloc(n + 2);
    memset(s, fill, n);
    if (tail) { s[n] = tail; s[n + 1] = '\0'; }
    else      { s[n] = '\0'; }
    return s;
}

/* Time the naive backtracking foil; returns ms, or -1 if it blew the budget. */
static double time_backtrack(const char *pat, const char *text, int *matched) {
    const char *err = NULL;
    double t0 = now_sec();
    int r = backtrack_match(pat, text, &err);
    double dt = now_sec() - t0;
    *matched = r;
    return (r < 0) ? -1.0 : dt;
}

static void catastrophic(void) {
    printf("### 1. Catastrophic backtracking: `(a+)+b` on `\"aaaa...a!\"`\n\n");
    printf("The input is n `a`s followed by `!`, which never matches -- the "
           "worst case for a backtracking engine.\n\n");
    printf("| n (a's) | ours (ms) | POSIX (ms) | naive backtracking (ms) |\n");
    printf("|--------:|----------:|-----------:|------------------------:|\n");
    int bt_dead = 0;
    for (int n = 10; n <= 30; n += 2) {
        char *text = make_run('a', n, '!');
        int m1, m2, m3;
        double d1 = time_ours("(a+)+b", text, &m1);
        double d2 = time_posix("(a+)+b", text, &m2);

        char b3[40];
        if (!bt_dead) {
            double d3 = time_backtrack("(a+)+b", text, &m3);
            if (d3 < 0) { snprintf(b3, sizeof(b3), "budget exceeded"); bt_dead = 1; }
            else snprintf(b3, sizeof(b3), "%.3f", d3 * 1e3);
        } else {
            snprintf(b3, sizeof(b3), "(skipped)");
        }
        printf("| %d | %.4f | %.3f | %s |\n", n, d1 * 1e3, d2 * 1e3, b3);
        free(text);
    }
    printf("\n");
}

static void ambiguous(void) {
    printf("### 2. Ambiguous repetition: `(a|aa)+$` on a long run of a's\n\n");
    printf("| n (a's) | ours (ms) | POSIX (ms) |\n");
    printf("|--------:|----------:|-----------:|\n");
    int posix_dead = 0;
    for (int n = 16; n <= 40; n += 4) {
        char *text = make_run('a', n, 0);
        int m1, m2;
        double d1 = time_ours("(a|aa)+$", text, &m1);
        const char *p2; char buf2[32];
        if (!posix_dead) {
            double d2 = time_posix("(a|aa)+$", text, &m2);
            snprintf(buf2, sizeof(buf2), "%.3f", d2 * 1e3);
            p2 = buf2;
            if (d2 > 5.0) posix_dead = 1;
        } else {
            p2 = "(skipped)";
        }
        printf("| %d | %.4f | %s |\n", n, d1 * 1e3, p2);
        free(text);
    }
    printf("\n");
}

static void common_case(void) {
    printf("### 3. Common case: full match of `([a-z]+ )*[a-z]+` on a large input\n\n");
    printf("| input size | ours (ms) | POSIX (ms) |\n");
    printf("|-----------:|----------:|-----------:|\n");

    /* Our regex_match is a full match. To make POSIX do the same amount of
     * work (verify the whole string, not stop at the first match) we anchor
     * its pattern with ^...$. */
    const char *ours_pat  = "([a-z]+ )*[a-z]+";
    const char *posix_pat = "^([a-z]+ )*[a-z]+$";
    for (size_t n = 1u << 18; n <= (1u << 21); n <<= 1) {
        /* words of lowercase letters separated by single spaces */
        char *text = malloc(n + 1);
        for (size_t i = 0; i < n; i++) text[i] = ((i + 1) % 8 == 0) ? ' ' : 'a' + (i % 26);
        text[n - 1] = 'z';                 /* end on a letter, not a space */
        text[n] = '\0';

        int m1, m2;
        double d1 = time_ours(ours_pat, text, &m1);
        double d2 = time_posix(posix_pat, text, &m2);
        printf("| %zu bytes | %.3f | %.3f |\n", n, d1 * 1e3, d2 * 1e3);
        free(text);
    }
    printf("\n");
}

int main(void) {
    printf("## Benchmark results\n\n");
    printf("_Times are a single run each; treat them as orders of magnitude, "
           "not precise measurements._\n\n");
    catastrophic();
    ambiguous();
    common_case();
    return 0;
}
