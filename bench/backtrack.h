/*
 * backtrack.h -- a deliberately naive backtracking matcher, used only by the
 * benchmark as a foil. This is the kind of engine (PCRE, Perl, Python's re,
 * Java) whose running time explodes on patterns like (a+)+b; it exists here so
 * the benchmark can show the blowup that the DFA engine is built to avoid.
 *
 * Returns 1 on full match, 0 on no match, and -1 if the step budget was
 * exceeded (i.e. it "blew up"). On a compile error returns -1 and sets *err.
 */
#ifndef BENCH_BACKTRACK_H
#define BENCH_BACKTRACK_H

int backtrack_match(const char *pattern, const char *text, const char **err);

#endif
