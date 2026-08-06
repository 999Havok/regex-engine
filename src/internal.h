/*
 * internal.h -- shared definitions across the compilation pipeline.
 *
 *   parser.c    pattern  -> syntax tree (Node)
 *   positions.c syntax tree -> nullable/firstpos/lastpos/followpos (Positions)
 *   dfa.c       Positions -> DFA by subset construction
 *   minimize.c  DFA -> minimised DFA (Hopcroft)
 *   match.c     minimised DFA -> matching, plus regex_compile/free glue
 *
 * Alphabet: symbols 0..255 are input bytes. Two extra symbols model the
 * zero-width anchors so they can be handled by the same table-driven DFA:
 *   SYM_START (256) is fed at the very start of the text  (matches ^)
 *   SYM_END   (257) is fed at the very end of the text     (matches $)
 * See dfa.c for how anchor columns get their "pass-through" transition, and
 * match.c for when the boundary symbols are fed.
 */
#ifndef REGEX_INTERNAL_H
#define REGEX_INTERNAL_H

#include <stdio.h>

#include "bitset.h"
#include "regex.h"

#define SYM_START 256
#define SYM_END   257
#define NSYM      258

/* ------------------------------------------------------------------ AST */

typedef enum {
    N_CHARSET,       /* leaf: matches any byte in `set` */
    N_ANCHOR_START,  /* leaf: ^  (matches SYM_START) */
    N_ANCHOR_END,    /* leaf: $  (matches SYM_END) */
    N_END,           /* leaf: the augmentation endmarker # */
    N_EPSILON,       /* matches the empty string */
    N_CONCAT,        /* left . right */
    N_ALT,           /* left | right */
    N_STAR           /* left * */
} NodeKind;

typedef struct Node {
    NodeKind kind;
    unsigned char set[32];   /* N_CHARSET: 256-bit byte membership map */
    struct Node *left, *right;

    /* Filled in by positions.c: */
    int    pos;              /* leaf position id, or -1 for interior nodes */
    int    nullable;
    BitSet first, last;      /* firstpos / lastpos */
} Node;

/* ast.c */
Node *ast_leaf(NodeKind kind);
Node *ast_charset(const unsigned char set[32]);
Node *ast_epsilon(void);
Node *ast_binary(NodeKind kind, Node *l, Node *r);
Node *ast_star(Node *child);
Node *ast_clone(const Node *n);       /* deep copy, for desugaring + */
void  ast_free(Node *n);
void  ast_dump(const Node *n, FILE *out);   /* human-readable tree, for tools */

/* parser.c: returns the syntax tree for `pattern` (without the endmarker),
 * or NULL on error with *err set to a static description. */
Node *parse_regex(const char *pattern, const char **err);

/* ---------------------------------------------------------- Positions */

typedef enum {
    LEAF_CHARSET,
    LEAF_ANCHOR_START,
    LEAF_ANCHOR_END,
    LEAF_END
} LeafKind;

typedef struct {
    int            numpos;        /* positions are 0..numpos-1 */
    LeafKind      *kind;          /* [numpos] */
    unsigned char (*set)[32];     /* [numpos] byte maps (CHARSET leaves) */
    BitSet        *followpos;     /* [numpos] */
    BitSet         start;         /* firstpos of the augmented root */
    int            endpos;        /* position id of the endmarker */
} Positions;

/* positions.c: augments `tree` with an endmarker, numbers the leaves, and
 * computes the follow tables. Consumes ownership of nothing; the caller still
 * frees `tree`. Returns 0 on success, -1 on allocation failure. */
int  positions_build(Node *tree, Positions *out);
void positions_free(Positions *p);

/* --------------------------------------------------------------- DFA */

typedef struct {
    int   nstates;
    int   start;
    int   dead;
    int  *trans;      /* nstates * NSYM, each entry a state id */
    unsigned char *accept;  /* [nstates], 1 if accepting */
    BitSet *sets;     /* [nstates] position-set label per state, or NULL.
                         Populated by dfa_build for debugging/regexdump;
                         dfa_minimize leaves it NULL. */
} DFA;

/* dfa.c: subset construction from the follow tables. Returns 0 / -1. */
int  dfa_build(const Positions *p, DFA *out);
void dfa_free(DFA *d);

/* minimize.c: Hopcroft minimisation. Produces a fresh DFA. Returns 0 / -1. */
int  dfa_minimize(const DFA *in, DFA *out);

/* --------------------------------------------------------------- Regex */

struct Regex {
    DFA dfa;                 /* the minimised, matching automaton */
    int raw_states;          /* state count before minimisation (for tests) */
    int min_states;          /* state count after minimisation */
};

/* Debug accessors used by tests and tools; not part of the public API. */
int regex_debug_raw_states(const Regex *re);
int regex_debug_min_states(const Regex *re);

#endif /* REGEX_INTERNAL_H */
