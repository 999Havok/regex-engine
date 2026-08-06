/*
 * regexdump.c -- inspect every stage of the compilation pipeline for a pattern.
 *
 *   regexdump 'PATTERN'
 *
 * Prints, in order:
 *   1. the syntax tree with numbered leaf positions
 *   2. the nullable / firstpos / lastpos table
 *   3. the followpos table
 *   4. the DFA transition table from subset construction (states are position
 *      sets, exactly as in Aho/Sethi/Ullman), plus the minimised state count
 *
 * All output is generated from the real engine data structures, so the tables
 * in the README (produced by this tool) are guaranteed to match the code.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A readable name for a position: its symbol, class, anchor, or #. */
static void position_label(const Positions *P, int pos, char *buf, size_t n) {
    switch (P->kind[pos]) {
    case LEAF_END:          snprintf(buf, n, "#"); return;
    case LEAF_ANCHOR_START: snprintf(buf, n, "^"); return;
    case LEAF_ANCHOR_END:   snprintf(buf, n, "$"); return;
    case LEAF_CHARSET: break;
    }
    /* Count members to decide between a single char and a class. */
    int count = 0, only = -1;
    for (int c = 0; c < 256; c++)
        if ((P->set[pos][c >> 3] >> (c & 7)) & 1) { count++; if (only < 0) only = c; }
    if (count == 1) {
        if (only >= 0x20 && only < 0x7f) snprintf(buf, n, "%c", only);
        else                             snprintf(buf, n, "\\x%02x", only);
    } else {
        snprintf(buf, n, "<class:%d chars>", count);
    }
}

static void print_bitset(const BitSet *b) {
    putchar('{');
    size_t it = 0; long p; int first = 1;
    while ((p = bitset_next(b, &it)) >= 0) {
        if (!first) printf(", ");
        printf("%ld", p + 1);            /* 1-based to match ASU convention */
        first = 0;
    }
    putchar('}');
}

/* Recursively print nullable/firstpos/lastpos for every node. */
static int node_counter;
static void print_node_sets(const Node *n) {
    if (!n) return;
    print_node_sets(n->left);
    print_node_sets(n->right);
    const char *kind = "?";
    switch (n->kind) {
    case N_CHARSET: kind = "char"; break;
    case N_ANCHOR_START: kind = "^"; break;
    case N_ANCHOR_END: kind = "$"; break;
    case N_EPSILON: kind = "eps"; break;
    case N_CONCAT: kind = "concat"; break;
    case N_ALT: kind = "alt"; break;
    case N_STAR: kind = "star"; break;
    case N_END: kind = "#"; break;
    }
    printf("  n%-3d %-7s nullable=%d  first=", node_counter++, kind, n->nullable);
    print_bitset(&n->first);
    printf("  last=");
    print_bitset(&n->last);
    putchar('\n');
}

/* Build the sorted list of byte symbols that appear in the pattern, so the
 * transition table has a narrow, readable set of columns. */
static int collect_alphabet(const Positions *P, int *bytes, int *use_start, int *use_end) {
    char seen[256] = {0};
    *use_start = *use_end = 0;
    for (int p = 0; p < P->numpos; p++) {
        if (P->kind[p] == LEAF_ANCHOR_START) *use_start = 1;
        else if (P->kind[p] == LEAF_ANCHOR_END) *use_end = 1;
        else if (P->kind[p] == LEAF_CHARSET)
            for (int c = 0; c < 256; c++)
                if ((P->set[p][c >> 3] >> (c & 7)) & 1) seen[c] = 1;
    }
    int n = 0;
    for (int c = 0; c < 256; c++) if (seen[c]) bytes[n++] = c;
    return n;
}

static void sym_header(int sym) {
    if (sym == SYM_START)      printf(" %4s", "^");
    else if (sym == SYM_END)   printf(" %4s", "$");
    else if (sym >= 0x20 && sym < 0x7f) printf(" %4c", sym);
    else                       printf(" \\x%02x", sym);
}

static void print_dfa(const DFA *d, const Positions *P) {
    int bytes[256];
    int use_start, use_end;
    int nb = collect_alphabet(P, bytes, &use_start, &use_end);
    if (nb > 40) nb = 40;   /* keep the table sane for huge classes */

    printf("state          ");
    for (int i = 0; i < nb; i++) sym_header(bytes[i]);
    if (use_start) sym_header(SYM_START);
    if (use_end)   sym_header(SYM_END);
    if (d->sets) printf("   set");
    putchar('\n');

    for (int s = 0; s < d->nstates; s++) {
        char flag = ' ';
        if (s == d->start) flag = '>';
        printf("%c%-3d%-2s", flag, s, d->accept[s] ? " *" : "  ");
        for (int i = 0; i < nb; i++) printf(" %4d", d->trans[s * NSYM + bytes[i]]);
        if (use_start) printf(" %4d", d->trans[s * NSYM + SYM_START]);
        if (use_end)   printf(" %4d", d->trans[s * NSYM + SYM_END]);
        if (d->sets) { printf("   "); print_bitset(&d->sets[s]); }
        putchar('\n');
    }
    printf("(> = start, * = accepting; entries are target state ids; "
           "state %d is dead)\n", d->dead);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s 'PATTERN'\n", argv[0]);
        return 2;
    }
    const char *pattern = argv[1];
    const char *err = NULL;

    Node *tree = parse_regex(pattern, &err);
    if (!tree) {
        fprintf(stderr, "parse error: %s\n", err);
        return 1;
    }

    Positions P;
    if (positions_build(tree, &P) != 0) {
        fprintf(stderr, "out of memory building positions\n");
        ast_free(tree);
        return 1;
    }

    printf("Pattern: %s\n\n", pattern);

    printf("== Syntax tree (augmented with endmarker #) ==\n");
    ast_dump(tree, stdout);
    printf("  END # pos=%d\n\n", P.endpos);

    printf("== Positions ==\n");
    for (int p = 0; p < P.numpos; p++) {
        char buf[32];
        position_label(&P, p, buf, sizeof(buf));
        printf("  %d: %s\n", p + 1, buf);
    }
    putchar('\n');

    printf("== nullable / firstpos / lastpos (per node, post-order) ==\n");
    node_counter = 0;
    print_node_sets(tree);
    printf("  (root firstpos = DFA start state)\n\n");

    printf("== followpos ==\n");
    for (int p = 0; p < P.numpos; p++) {
        char buf[32];
        position_label(&P, p, buf, sizeof(buf));
        printf("  %2d (%s): ", p + 1, buf);
        print_bitset(&P.followpos[p]);
        putchar('\n');
    }
    putchar('\n');

    DFA raw, min;
    if (dfa_build(&P, &raw) != 0) {
        fprintf(stderr, "out of memory building DFA\n");
        positions_free(&P);
        ast_free(tree);
        return 1;
    }

    printf("== DFA (subset construction) ==\n");
    print_dfa(&raw, &P);
    putchar('\n');

    if (dfa_minimize(&raw, &min) == 0) {
        printf("== DFA (after Hopcroft minimisation) ==\n");
        print_dfa(&min, &P);   /* min.sets is NULL, so no set column */
        printf("\nStates: %d before minimisation, %d after.\n",
               raw.nstates, min.nstates);
        dfa_free(&min);
    }

    dfa_free(&raw);
    positions_free(&P);
    ast_free(tree);
    return 0;
}
