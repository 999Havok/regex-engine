/*
 * ast.c -- syntax-tree node construction, cloning, freeing, and a debug dump.
 *
 * Leaf nodes carry a `pos` id and firstpos/lastpos bit sets, but those are
 * only populated later, in positions.c. Here we just manage the tree shape.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static Node *node_alloc(NodeKind kind) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) return NULL;
    n->kind = kind;
    n->pos = -1;
    /* first/last remain zeroed (nwords 0, w NULL) until positions_build. */
    return n;
}

Node *ast_leaf(NodeKind kind) {
    return node_alloc(kind);
}

Node *ast_charset(const unsigned char set[32]) {
    Node *n = node_alloc(N_CHARSET);
    if (n) memcpy(n->set, set, 32);
    return n;
}

Node *ast_epsilon(void) {
    return node_alloc(N_EPSILON);
}

Node *ast_binary(NodeKind kind, Node *l, Node *r) {
    if (!l || !r) {          /* propagate a failed sub-allocation cleanly */
        ast_free(l);
        ast_free(r);
        return NULL;
    }
    Node *n = node_alloc(kind);
    if (!n) {
        ast_free(l);
        ast_free(r);
        return NULL;
    }
    n->left = l;
    n->right = r;
    return n;
}

Node *ast_star(Node *child) {
    if (!child) return NULL;
    Node *n = node_alloc(N_STAR);
    if (!n) {
        ast_free(child);
        return NULL;
    }
    n->left = child;
    return n;
}

Node *ast_clone(const Node *n) {
    if (!n) return NULL;
    Node *c = node_alloc(n->kind);
    if (!c) return NULL;
    memcpy(c->set, n->set, 32);
    if (n->left) {
        c->left = ast_clone(n->left);
        if (!c->left) { ast_free(c); return NULL; }
    }
    if (n->right) {
        c->right = ast_clone(n->right);
        if (!c->right) { ast_free(c); return NULL; }
    }
    return c;
}

void ast_free(Node *n) {
    if (!n) return;
    ast_free(n->left);
    ast_free(n->right);
    bitset_free(&n->first);
    bitset_free(&n->last);
    free(n);
}

/* -------------------------------------------------------------- debug dump */

static void dump_charset(const unsigned char set[32], FILE *out) {
    fputc('[', out);
    for (int c = 0; c < 256; c++) {
        if (!((set[c >> 3] >> (c & 7)) & 1)) continue;
        /* collapse contiguous runs into ranges */
        int start = c;
        while (c + 1 < 256 && ((set[(c + 1) >> 3] >> ((c + 1) & 7)) & 1)) c++;
        if (start >= 0x20 && start < 0x7f) fputc(start, out);
        else fprintf(out, "\\x%02x", start);
        if (c != start) {
            fputc('-', out);
            if (c >= 0x20 && c < 0x7f) fputc(c, out);
            else fprintf(out, "\\x%02x", c);
        }
    }
    fputc(']', out);
}

static void dump(const Node *n, FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
    switch (n->kind) {
    case N_CHARSET:
        fprintf(out, "CHARSET pos=%d ", n->pos);
        dump_charset(n->set, out);
        fputc('\n', out);
        break;
    case N_ANCHOR_START: fprintf(out, "ANCHOR ^ pos=%d\n", n->pos); break;
    case N_ANCHOR_END:   fprintf(out, "ANCHOR $ pos=%d\n", n->pos); break;
    case N_END:          fprintf(out, "END # pos=%d\n", n->pos); break;
    case N_EPSILON:      fputs("EPSILON\n", out); break;
    case N_CONCAT:       fputs("CONCAT\n", out); break;
    case N_ALT:          fputs("ALT\n", out); break;
    case N_STAR:         fputs("STAR\n", out); break;
    }
    if (n->left)  dump(n->left, out, depth + 1);
    if (n->right) dump(n->right, out, depth + 1);
}

void ast_dump(const Node *n, FILE *out) {
    if (n) dump(n, out, 0);
}
