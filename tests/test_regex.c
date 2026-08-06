/*
 * test_regex.c -- assertion-based test runner (no framework).
 *
 * Every check funnels through CHECK/CHECK_MATCH/... which count passes and
 * failures and print only failures plus a final summary. Exit status is
 * non-zero if anything failed, so `make test` fails the build on regression.
 */
#include "regex.h"
#include "internal.h"      /* debug state-count accessors */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
static int failures = 0;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);            \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                          \
    }                                                          \
} while (0)

/* Compile `pat`, assert regex_match(text) == expect, then free. */
static void check_match(const char *pat, const char *text, int expect) {
    const char *err = NULL;
    Regex *re = regex_compile(pat, &err);
    CHECK(re != NULL, "compile('%s') failed: %s", pat, err ? err : "?");
    if (!re) return;
    int got = regex_match(re, text);
    CHECK(got == expect, "match('%s', \"%s\") = %d, want %d", pat, text, got, expect);
    regex_free(re);
}

/* Compile `pat`, assert it fails to compile (malformed pattern). */
static void check_compile_error(const char *pat) {
    const char *err = NULL;
    Regex *re = regex_compile(pat, &err);
    CHECK(re == NULL, "compile('%s') should have failed but succeeded", pat);
    CHECK(re != NULL || err != NULL, "compile('%s') failed but set no error string", pat);
    regex_free(re);
}

/* Compile `pat`, assert regex_search finds [ws,we); if expect_found is 0 the
 * search must return no match. */
static void check_search(const char *pat, const char *text, int expect_found,
                         size_t ws, size_t we) {
    const char *err = NULL;
    Regex *re = regex_compile(pat, &err);
    CHECK(re != NULL, "compile('%s') failed: %s", pat, err ? err : "?");
    if (!re) return;
    size_t s = 0, e = 0;
    int found = regex_search(re, text, &s, &e);
    CHECK(found == expect_found, "search('%s', \"%s\") found=%d, want %d",
          pat, text, found, expect_found);
    if (found && expect_found)
        CHECK(s == ws && e == we, "search('%s', \"%s\") = [%zu,%zu), want [%zu,%zu)",
              pat, text, s, e, ws, we);
    regex_free(re);
}

static void test_basics(void) {
    check_match("a", "a", 1);
    check_match("a", "b", 0);
    check_match("a", "", 0);
    check_match("abc", "abc", 1);
    check_match("abc", "abd", 0);
    check_match("abc", "ab", 0);
    check_match("abc", "abcd", 0);

    /* alternation */
    check_match("a|b", "a", 1);
    check_match("a|b", "b", 1);
    check_match("a|b", "c", 0);
    check_match("cat|dog", "dog", 1);
    check_match("cat|dog", "cog", 0);

    /* star */
    check_match("a*", "", 1);
    check_match("a*", "a", 1);
    check_match("a*", "aaaaa", 1);
    check_match("a*", "aab", 0);
    check_match("ab*c", "ac", 1);
    check_match("ab*c", "abbbc", 1);

    /* plus */
    check_match("a+", "", 0);
    check_match("a+", "a", 1);
    check_match("a+", "aaa", 1);
    check_match("(ab)+", "abab", 1);
    check_match("(ab)+", "aba", 0);

    /* optional */
    check_match("a?", "", 1);
    check_match("a?", "a", 1);
    check_match("a?", "aa", 0);
    check_match("colou?r", "color", 1);
    check_match("colou?r", "colour", 1);
    check_match("colou?r", "colouur", 0);
}

static void test_classic(void) {
    /* The textbook example. */
    check_match("(a|b)*abb", "abb", 1);
    check_match("(a|b)*abb", "aabb", 1);
    check_match("(a|b)*abb", "babbabb", 1);
    check_match("(a|b)*abb", "ababababb", 1);
    check_match("(a|b)*abb", "ab", 0);
    check_match("(a|b)*abb", "abba", 0);
    check_match("(a|b)*abb", "abbb", 0);
    check_match("(a|b)*abb", "", 0);
}

static void test_nesting(void) {
    check_match("((a|b)*c)+", "c", 1);
    check_match("((a|b)*c)+", "abc", 1);
    check_match("((a|b)*c)+", "abcbac", 1);
    check_match("((a|b)*c)+", "cccc", 1);
    check_match("((a|b)*c)+", "", 0);
    check_match("((a|b)*c)+", "ab", 0);
    check_match("((a|b)*c)+", "abca", 0);
    check_match("(a(b|c)*)+", "abccbc", 1);
    check_match("((x|y)+z?)+", "xyzxz", 1);
}

static void test_char_classes(void) {
    check_match("[abc]", "a", 1);
    check_match("[abc]", "d", 0);
    check_match("[a-z]+", "hello", 1);
    check_match("[a-z]+", "Hello", 0);
    check_match("[a-zA-Z0-9]+", "Abc123", 1);
    check_match("[0-9]+", "2024", 1);

    /* negation */
    check_match("[^0-9]+", "abc", 1);
    check_match("[^0-9]+", "ab1", 0);
    check_match("[^a]", "b", 1);
    check_match("[^a]", "a", 0);

    /* escaped metacharacters inside brackets */
    check_match("[.*+]", ".", 1);
    check_match("[.*+]", "*", 1);
    check_match("[.*+]", "a", 0);
    check_match("[\\]]", "]", 1);
    check_match("[a\\-c]", "-", 1);
    check_match("[a\\-c]", "b", 0);
    check_match("[\\d]+", "123", 1);
    check_match("[\\dx]+", "1x2", 1);

    /* a ']' right after '[' is a literal, not a close */
    check_match("[]a]", "]", 1);
    check_match("[]a]", "a", 1);

    /* shorthand classes */
    check_match("\\d\\d", "42", 1);
    check_match("\\w+", "hello_World9", 1);
    check_match("\\w+", "hi there", 0);
    check_match("\\s", " ", 1);
    check_match("\\s", "x", 0);
    check_match("\\D+", "abc", 1);
    check_match("\\D+", "a1", 0);
    check_match("\\S+", "abc", 1);
    check_match("\\W+", "!@#", 1);
}

static void test_wildcard_and_escapes(void) {
    check_match("a.c", "abc", 1);
    check_match("a.c", "axc", 1);
    check_match(".", "\n", 0);          /* '.' excludes newline */
    check_match("a.c", "a\nc", 0);
    check_match("\\.", ".", 1);
    check_match("\\.", "x", 0);
    check_match("a\\*b", "a*b", 1);
    check_match("a\\*b", "aab", 0);
    check_match("\\(\\)", "()", 1);
    check_match("\\n", "\n", 1);
    check_match("\\t", "\t", 1);
}

static void test_empty(void) {
    check_match("", "", 1);
    check_match("", "a", 0);
    check_match("a*", "", 1);
    check_match("a?", "", 1);
    check_match("()", "", 1);            /* empty group matches only "" */
    check_match("()", "a", 0);
    check_match("(a|)", "", 1);          /* explicit empty alternative */
    check_match("(a|)", "a", 1);
}

static void test_anchors(void) {
    check_match("^abc", "abc", 1);
    check_match("abc$", "abc", 1);
    check_match("^abc$", "abc", 1);
    check_match("^abc$", "abcd", 0);
    check_match("^abc$", "xabc", 0);
    check_match("^$", "", 1);            /* empty line */
    check_match("^$", "x", 0);
    check_match("^a*$", "aaa", 1);

    /* anchors bind to their own alternative */
    check_match("^a|b", "a", 1);
    check_match("^a|b", "b", 1);

    /* in search, ^ only matches at position 0 and $ only at the end */
    check_search("^abc", "abcxx", 1, 0, 3);
    check_search("^abc", "xabc", 0, 0, 0);
    check_search("abc$", "xxabc", 1, 2, 5);
    check_search("abc$", "abcxx", 0, 0, 0);
    check_search("^abc$", "abc", 1, 0, 3);
}

static void test_search(void) {
    /* leftmost */
    check_search("a", "bbaba", 1, 2, 3);
    check_search("abc", "xxabcyy", 1, 2, 5);
    check_search("z", "abc", 0, 0, 0);

    /* longest at the leftmost start */
    check_search("a+", "xaaax", 1, 1, 4);
    check_search("a*", "aaab", 1, 0, 3);
    check_search("(a|b)+", "xxabbaxx", 1, 2, 6);

    /* leftmost wins over a later longer match */
    check_search("a+", "axaaa", 1, 0, 1);

    /* empty match is leftmost-longest at position 0 for nullable patterns */
    check_search("a*", "bbb", 1, 0, 0);
    check_search("x*", "", 1, 0, 0);

    /* a realistic extraction */
    check_search("\\d+", "abc123def", 1, 3, 6);
    check_search("[a-z]+", "  hello  ", 1, 2, 7);
}

static void test_malformed(void) {
    check_compile_error("(");
    check_compile_error(")");
    check_compile_error("(a");
    check_compile_error("a)");
    check_compile_error("a(b|c");
    check_compile_error("\\");           /* trailing backslash */
    check_compile_error("abc\\");
    check_compile_error("[abc");         /* unclosed bracket */
    check_compile_error("[a-z");
    check_compile_error("*");            /* nothing to repeat */
    check_compile_error("*abc");
    check_compile_error("+a");
    check_compile_error("?a");
    check_compile_error("[z-a]");        /* reversed range */
    check_compile_error("[9-0]");
    check_compile_error("\\q");          /* unknown escape */

    /* well-formed edge cases that must NOT error */
    const char *err = NULL;
    Regex *re;
    re = regex_compile("a**", &err);  CHECK(re != NULL, "a** should compile"); regex_free(re);
    re = regex_compile("[a-]", &err); CHECK(re != NULL, "[a-] should compile"); regex_free(re);
    re = regex_compile("[-a]", &err); CHECK(re != NULL, "[-a] should compile"); regex_free(re);
}

static void test_minimization(void) {
    /* (a*b*)+ produces a subset-construction DFA with redundant states that
     * Hopcroft collapses. Assert the state count strictly drops, and that the
     * minimised machine still matches correctly. */
    const char *err = NULL;
    Regex *re = regex_compile("(a*b*)+", &err);
    CHECK(re != NULL, "compile('(a*b*)+') failed: %s", err ? err : "?");
    if (re) {
        int raw = regex_debug_raw_states(re);
        int min = regex_debug_min_states(re);
        CHECK(min < raw, "(a*b*)+ minimisation did not reduce states: raw=%d min=%d", raw, min);
        CHECK(regex_match(re, "aabbab"), "(a*b*)+ should match aabbab");
        CHECK(regex_match(re, ""), "(a*b*)+ should match empty");
        regex_free(re);
    }

    /* A second, more intuitive case. */
    re = regex_compile("(ab|cd)+", &err);
    CHECK(re != NULL, "compile('(ab|cd)+') failed");
    if (re) {
        CHECK(regex_debug_min_states(re) < regex_debug_raw_states(re),
              "(ab|cd)+ minimisation did not reduce states");
        CHECK(regex_match(re, "abcdab"), "(ab|cd)+ should match abcdab");
        CHECK(!regex_match(re, "abc"), "(ab|cd)+ should not match abc");
        regex_free(re);
    }
}

static void test_no_catastrophic_backtracking(void) {
    /* The whole point: (a+)+b on a long run of a's followed by '!' resolves
     * immediately instead of blowing up. If the engine backtracked this would
     * hang; here it is a linear table walk. */
    const char *err = NULL;
    Regex *re = regex_compile("(a+)+b", &err);
    CHECK(re != NULL, "compile('(a+)+b') failed: %s", err ? err : "?");
    if (re) {
        char buf[64];
        memset(buf, 'a', 40);
        buf[40] = '!';
        buf[41] = '\0';
        CHECK(!regex_match(re, buf), "(a+)+b must reject 40 a's + '!'");
        memset(buf, 'a', 40);
        buf[40] = 'b';
        buf[41] = '\0';
        CHECK(regex_match(re, buf), "(a+)+b must accept 40 a's + 'b'");
        regex_free(re);
    }
}

int main(void) {
    test_basics();
    test_classic();
    test_nesting();
    test_char_classes();
    test_wildcard_and_escapes();
    test_empty();
    test_anchors();
    test_search();
    test_malformed();
    test_minimization();
    test_no_catastrophic_backtracking();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
