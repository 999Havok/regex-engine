/*
 * parser.c -- recursive-descent parser: pattern text -> syntax tree.
 *
 * Grammar (lowest precedence first):
 *
 *   alt     := concat ('|' concat)*
 *   concat  := repeat*                      (empty concat = epsilon)
 *   repeat  := atom ('*' | '+' | '?')*
 *   atom    := '(' alt ')' | class | '.' | '^' | '$' | escape | literal
 *
 * The postfix operators + and ? are desugared here, exactly as the ASU text
 * suggests:  r+  ->  r r*   and   r?  ->  (r | epsilon).
 * Desugaring r+ clones r so the two copies get distinct leaf positions.
 *
 * On any error the parser sets *err to a static string that includes the byte
 * offset, and every partially built subtree is freed before returning NULL.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char  *s;
    int          pos;
    const char **err;
    int          failed;
} P;

static char err_buf[160];

static void set_error(P *p, int offset, const char *msg) {
    if (p->failed) return;            /* keep the first error */
    snprintf(err_buf, sizeof(err_buf), "%s at offset %d", msg, offset);
    if (p->err) *p->err = err_buf;
    p->failed = 1;
}

static int  peek(P *p)   { return (unsigned char)p->s[p->pos]; }
static int  at_end(P *p) { return p->s[p->pos] == '\0'; }
static int  advance(P *p){ return (unsigned char)p->s[p->pos++]; }

/* ------------------------------------------------------ charset helpers */

static void cs_set(unsigned char set[32], int c)  { set[c >> 3] |= 1u << (c & 7); }
static void cs_range(unsigned char set[32], int lo, int hi) {
    for (int c = lo; c <= hi; c++) cs_set(set, c);
}
static void cs_complement(unsigned char set[32]) {
    for (int i = 0; i < 32; i++) set[i] = (unsigned char)~set[i];
}

/* Build the byte map for a shorthand class letter (d/w/s). Uppercase letters
 * (D/W/S) produce the complement. Returns 0 on success, -1 if `which` is not
 * a recognised shorthand. */
static int shorthand(int which, unsigned char out[32]) {
    memset(out, 0, 32);
    int lower = which;
    int negate = 0;
    if (which == 'D' || which == 'W' || which == 'S') { negate = 1; lower = which + 32; }
    switch (lower) {
    case 'd': cs_range(out, '0', '9'); break;
    case 'w': cs_range(out, 'a', 'z'); cs_range(out, 'A', 'Z');
              cs_range(out, '0', '9'); cs_set(out, '_'); break;
    case 's': cs_set(out, ' '); cs_set(out, '\t'); cs_set(out, '\n');
              cs_set(out, '\r'); cs_set(out, '\f'); cs_set(out, '\v'); break;
    default: return -1;
    }
    if (negate) cs_complement(out);
    return 0;
}

/* Is `c` one of the metacharacters that may follow a backslash to mean the
 * literal character itself? */
static int is_literal_escape(int c) {
    return strchr("ntr\\.*+?()[]|-^$", c) != NULL;
}

/* Decode the value of a literal escape byte (its second char already known). */
static int literal_escape_value(int c) {
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    default:  return c;   /* \\ \. \* \+ \? \( \) \[ \] \| \- \^ \$ */
    }
}

/* -------------------------------------------------------- class parsing */

/*
 * Parse one member inside [...]. On return:
 *   *is_single == 1  -> a single character whose value is *value
 *   *is_single == 0  -> a shorthand set that has already been OR'd into `set`
 * Returns 0 on success, -1 on error (with the parser error set).
 */
static int class_member(P *p, unsigned char set[32], int *is_single, int *value) {
    if (peek(p) == '\\') {
        int esc_off = p->pos;
        advance(p);                       /* consume '\\' */
        if (at_end(p)) { set_error(p, esc_off, "trailing backslash"); return -1; }
        int c = advance(p);
        unsigned char sh[32];
        if (shorthand(c, sh) == 0) {
            for (int i = 0; i < 32; i++) set[i] |= sh[i];
            *is_single = 0;
            return 0;
        }
        if (is_literal_escape(c)) {
            *is_single = 1;
            *value = literal_escape_value(c);
            return 0;
        }
        set_error(p, esc_off, "unknown escape sequence");
        return -1;
    }
    *is_single = 1;
    *value = advance(p);
    return 0;
}

static Node *parse_class(P *p) {
    int open_off = p->pos;
    advance(p);                           /* consume '[' */

    int negate = 0;
    if (peek(p) == '^') { advance(p); negate = 1; }

    unsigned char set[32] = {0};
    int first = 1;
    for (;;) {
        if (at_end(p)) { set_error(p, open_off, "unclosed character class"); return NULL; }
        if (peek(p) == ']' && !first) { advance(p); break; }
        first = 0;

        int lo_single, lo;
        if (class_member(p, set, &lo_single, &lo) != 0) return NULL;

        /* A range lo-hi only forms when lo is a single char, a '-' follows,
         * and the '-' is not the closing member. */
        if (lo_single && peek(p) == '-' &&
            p->s[p->pos + 1] != ']' && p->s[p->pos + 1] != '\0') {
            int dash_off = p->pos;
            advance(p);                   /* consume '-' */
            int hi_single, hi;
            if (class_member(p, set, &hi_single, &hi) != 0) return NULL;
            if (!hi_single) { set_error(p, dash_off, "shorthand class as range bound"); return NULL; }
            if (lo > hi) { set_error(p, dash_off, "reversed character range"); return NULL; }
            cs_range(set, lo, hi);
        } else if (lo_single) {
            cs_set(set, lo);
        }
    }

    if (negate) cs_complement(set);
    return ast_charset(set);
}

/* ------------------------------------------------------- expression parse */

static Node *parse_alt(P *p);

static Node *parse_atom(P *p) {
    int c = peek(p);
    switch (c) {
    case '(': {
        int open_off = p->pos;
        advance(p);
        Node *inner = parse_alt(p);
        if (p->failed) { ast_free(inner); return NULL; }
        if (peek(p) != ')') {
            set_error(p, open_off, "unbalanced '('");
            ast_free(inner);
            return NULL;
        }
        advance(p);                       /* consume ')' */
        return inner;
    }
    case '[':
        return parse_class(p);
    case '.': {
        advance(p);
        unsigned char set[32];
        memset(set, 0xff, 32);
        set['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));   /* '.' excludes newline */
        return ast_charset(set);
    }
    case '^':
        advance(p);
        return ast_leaf(N_ANCHOR_START);
    case '$':
        advance(p);
        return ast_leaf(N_ANCHOR_END);
    case '*': case '+': case '?':
        set_error(p, p->pos, "nothing to repeat");
        return NULL;
    case ')':
        set_error(p, p->pos, "unbalanced ')'");
        return NULL;
    case '\\': {
        int esc_off = p->pos;
        advance(p);
        if (at_end(p)) { set_error(p, esc_off, "trailing backslash"); return NULL; }
        int e = advance(p);
        unsigned char sh[32];
        if (shorthand(e, sh) == 0) return ast_charset(sh);
        if (is_literal_escape(e)) {
            unsigned char set[32] = {0};
            cs_set(set, literal_escape_value(e));
            return ast_charset(set);
        }
        set_error(p, esc_off, "unknown escape sequence");
        return NULL;
    }
    default: {
        advance(p);
        unsigned char set[32] = {0};
        cs_set(set, c);
        return ast_charset(set);
    }
    }
}

static Node *parse_repeat(P *p) {
    Node *atom = parse_atom(p);
    if (p->failed) { ast_free(atom); return NULL; }

    for (;;) {
        int c = peek(p);
        if (c == '*') {
            advance(p);
            atom = ast_star(atom);
        } else if (c == '+') {
            advance(p);
            Node *copy = ast_clone(atom);      /* r+ -> r r* */
            atom = ast_binary(N_CONCAT, atom, ast_star(copy));
        } else if (c == '?') {
            advance(p);
            atom = ast_binary(N_ALT, atom, ast_epsilon());  /* r? -> (r|eps) */
        } else {
            break;
        }
        if (!atom) { set_error(p, p->pos, "out of memory"); return NULL; }
    }
    return atom;
}

static int concat_end(P *p) {
    int c = peek(p);
    return at_end(p) || c == '|' || c == ')';
}

static Node *parse_concat(P *p) {
    Node *result = NULL;
    while (!concat_end(p)) {
        Node *r = parse_repeat(p);
        if (p->failed) { ast_free(r); ast_free(result); return NULL; }
        result = result ? ast_binary(N_CONCAT, result, r) : r;
    }
    if (!result) result = ast_epsilon();  /* empty alternative matches "" */
    return result;
}

static Node *parse_alt(P *p) {
    Node *left = parse_concat(p);
    if (p->failed) { ast_free(left); return NULL; }
    while (peek(p) == '|') {
        advance(p);
        Node *right = parse_concat(p);
        if (p->failed) { ast_free(left); ast_free(right); return NULL; }
        left = ast_binary(N_ALT, left, right);
    }
    return left;
}

Node *parse_regex(const char *pattern, const char **err) {
    P p = { pattern, 0, err, 0 };
    if (err) *err = NULL;

    Node *tree = parse_alt(&p);
    if (p.failed) { ast_free(tree); return NULL; }

    if (!at_end(&p)) {                     /* e.g. a stray ')' */
        set_error(&p, p.pos, "unexpected character");
        ast_free(tree);
        return NULL;
    }
    return tree;
}
