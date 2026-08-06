/*
 * regex.h -- public API for a backtracking-free regular expression engine.
 *
 * The engine compiles a pattern directly into a minimised DFA (Aho, Sethi,
 * Ullman "Compilers", section 3.9) and matches in worst-case time linear in
 * the length of the input. There is no backtracking, so pathological patterns
 * such as (a+)+b cannot cause exponential blowup.
 *
 * The API is intentionally tiny. Library code never calls abort() or exit();
 * every failure path returns an error to the caller.
 */
#ifndef REGEX_H
#define REGEX_H

#include <stddef.h>

typedef struct Regex Regex;

/*
 * Compile `pattern` into a Regex. On success returns a non-NULL handle that
 * must be released with regex_free(). On failure returns NULL and, if `err`
 * is non-NULL, sets *err to a human-readable static string describing the
 * problem and the character offset at which it occurred.
 *
 * The string pointed to by *err is owned by the library (a static buffer) and
 * must not be freed. It is valid until the next failing regex_compile() call.
 */
Regex *regex_compile(const char *pattern, const char **err);

/* Full match: returns 1 iff the entire text is in the language, else 0. */
int regex_match(const Regex *re, const char *text);

/*
 * Leftmost-longest search: scans for the leftmost position at which a match
 * begins and, at that position, the longest match. On success returns 1 and,
 * if `start`/`end` are non-NULL, writes the half-open byte range [*start,*end)
 * of the match. Returns 0 if no match exists anywhere in the text.
 */
int regex_search(const Regex *re, const char *text, size_t *start, size_t *end);

/* Release a compiled regex. Safe to call on NULL. */
void regex_free(Regex *re);

#endif /* REGEX_H */
