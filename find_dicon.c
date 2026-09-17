/*
 * find_dicon.c
 *
 * C implementation of Algorithm 1 (FIND_DICON structural discovery)
 * from "CFI Checking by Collaboration of Processor-Compiler and its
 * RTL Implementation".
 *
 * Pipeline:
 *   1. remove_back_edges()   : turn the CFG into a DAG (loops removed)
 *   2. compute_end_points()  : fill u.end = nearest convergence point
 *                              (immediate post-dominator)
 *   3. find_dicon()          : the recursive algorithm itself
 *
 * Differences from the pseudocode (marked "FIX" in the code):
 *   - Case 1 also stops when u == f (as described in the paper text).
 *   - Line 39 uses {m, m.local_end} because m.local_start is never defined.
 *   - The top-level region is recorded as the Top-DICON if it is still open.
 *
 * Build:  gcc -std=c99 -Wall -Wextra -O2 find_dicon.c -o find_dicon
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAX_NODES        128
#define NONE             (-1)
#define MIN_DICON_DEPTH  3      /* pseudocode: 3 <= depth            */
#define MAX_DICON_DEPTH  6      /* pseudocode: depth <= 6 (DICON-6)  */

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    int succ[2];      /* successors (a RISC-V branch has at most 2)   */
    int nsucc;        /* 0 = leaf, 1 = linear, 2 = divergence         */
    int end;          /* nearest convergence point (u.end)            */
    int local_depth;  /* u.local_depth                                */
    int local_end;    /* u.local_end                                  */
} Node;

typedef struct {
    Node nodes[MAX_NODES];
    int  n;
} CFG;

typedef struct {
    int begin;
    int end;
} Dicon;

/* global "blocks" set of the pseudocode */
static Dicon blocks[MAX_NODES * 2];
static int   nblocks = 0;

static void add_block(int begin, int end)
{
    for (int i = 0; i < nblocks; i++)          /* it is a set: no duplicates */
        if (blocks[i].begin == begin && blocks[i].end == end)
            return;
    blocks[nblocks].begin = begin;
    blocks[nblocks].end   = end;
    nblocks++;
}

/* ------------------------------------------------------------------ */
/* Graph construction helpers                                          */
/* ------------------------------------------------------------------ */

static void cfg_init(CFG *g, int n)
{
    g->n = n;
    for (int i = 0; i < n; i++) {
        g->nodes[i].nsucc       = 0;
        g->nodes[i].succ[0]     = NONE;
        g->nodes[i].succ[1]     = NONE;
        g->nodes[i].end         = NONE;
        g->nodes[i].local_depth = 0;
        g->nodes[i].local_end   = NONE;
    }
}

static void add_edge(CFG *g, int from, int to)
{
    Node *u = &g->nodes[from];
    if (u->nsucc >= 2) {
        fprintf(stderr, "node %d already has 2 successors\n", from);
        return;
    }
    u->succ[u->nsucc++] = to;
}

static void remove_edge(Node *u, int k)
{
    if (k == 0) u->succ[0] = u->succ[1];
    u->succ[1] = NONE;
    u->nsucc--;
}

/* ------------------------------------------------------------------ */
/* Step 1: remove back edges (DFS, an edge to a "gray" node is a loop) */
/* ------------------------------------------------------------------ */

enum { WHITE, GRAY, BLACK };

static void dfs_back_edges(CFG *g, int u, int color[])
{
    color[u] = GRAY;
    Node *U = &g->nodes[u];
    for (int k = U->nsucc - 1; k >= 0; k--) {
        int v = U->succ[k];
        if (color[v] == GRAY)
            remove_edge(U, k);                 /* back edge -> drop it */
        else if (color[v] == WHITE)
            dfs_back_edges(g, v, color);
    }
    color[u] = BLACK;
}

static void remove_back_edges(CFG *g, int entry)
{
    int color[MAX_NODES] = { 0 };
    dfs_back_edges(g, entry, color);
}

/* ------------------------------------------------------------------ */
/* Step 2: u.end = immediate post-dominator (nearest convergence)      */
/* ------------------------------------------------------------------ */

#define WORDS ((MAX_NODES + 1 + 63) / 64)
typedef struct { uint64_t w[WORDS]; } BitSet;

static void bs_fill(BitSet *s)            { memset(s->w, 0xFF, sizeof s->w); }
static void bs_clear(BitSet *s)           { memset(s->w, 0, sizeof s->w); }
static void bs_set(BitSet *s, int i)      { s->w[i / 64] |= (uint64_t)1 << (i % 64); }
static int  bs_has(const BitSet *s, int i){ return (s->w[i / 64] >> (i % 64)) & 1; }
static int  bs_eq(const BitSet *a, const BitSet *b)
{ return memcmp(a->w, b->w, sizeof a->w) == 0; }
static void bs_and(BitSet *a, const BitSet *b)
{ for (int i = 0; i < WORDS; i++) a->w[i] &= b->w[i]; }
static int  bs_count(const BitSet *s)
{
    int c = 0;
    for (int i = 0; i < WORDS; i++) c += __builtin_popcountll(s->w[i]);
    return c;
}

static void compute_end_points(CFG *g)
{
    int    n    = g->n;
    int    exit = n;                 /* virtual exit node after all leaves */
    BitSet pdom[MAX_NODES + 1];

    for (int i = 0; i < n; i++) bs_fill(&pdom[i]);
    bs_clear(&pdom[exit]);
    bs_set(&pdom[exit], exit);

    /* iterative data flow: pdom(u) = {u} U  AND pdom(s) over successors */
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int u = n - 1; u >= 0; u--) {
            Node  *U = &g->nodes[u];
            BitSet t;
            if (U->nsucc == 0) {
                t = pdom[exit];
            } else {
                t = pdom[U->succ[0]];
                for (int k = 1; k < U->nsucc; k++) bs_and(&t, &pdom[U->succ[k]]);
            }
            bs_set(&t, u);
            if (!bs_eq(&t, &pdom[u])) { pdom[u] = t; changed = 1; }
        }
    }

    /* immediate post-dominator = strict post-dominator with the largest set */
    for (int u = 0; u < n; u++) {
        int best = NONE, best_cnt = -1;
        for (int d = 0; d <= n; d++) {
            if (d == u || !bs_has(&pdom[u], d)) continue;
            int c = bs_count(&pdom[d]);
            if (c > best_cnt) { best_cnt = c; best = d; }
        }
        g->nodes[u].end = (best == exit) ? NONE : best;
    }
}

/* ------------------------------------------------------------------ */
/* Step 3: FIND_DICON                                                  */
/* ------------------------------------------------------------------ */

static int max_int(int a, int b) { return a > b ? a : b; }

static void find_dicon(CFG *g, int u, int f)
{
    Node *U = &g->nodes[u];

    /* ---- Case 1: leaf node (FIX: also stop at the finish node f) ---- */
    if (u == f || U->nsucc == 0) {
        U->local_depth = 0;
        U->local_end   = u;
        return;
    }

    /* ---- Case 2: linear path ---- */
    if (U->nsucc == 1) {
        int v = U->succ[0];
        find_dicon(g, v, f);
        U->local_depth = g->nodes[v].local_depth;
        U->local_end   = g->nodes[v].local_end;
        return;
    }

    /* ---- Case 3: divergence point ---- */
    int v1 = U->succ[0];
    int v2 = U->succ[1];
    int e  = U->end;

    find_dicon(g, v1, e);
    find_dicon(g, v2, e);
    U->local_depth = max_int(g->nodes[v1].local_depth,
                             g->nodes[v2].local_depth) + 1;
    U->local_end   = e;

    /* Initial nullifying */
    if (U->local_depth >= MIN_DICON_DEPTH && U->local_depth <= MAX_DICON_DEPTH) {
        add_block(u, U->local_end);
        U->local_depth = 0;
    }

    if (e == NONE)          /* branches never converge: nothing to merge */
        return;

    /* Merge logic with the lower DICON starting at m = u.end */
    int m = e;
    find_dicon(g, m, f);
    Node *M = &g->nodes[m];

    U->local_depth += M->local_depth;
    U->local_end    = M->local_end;

    /* Handle merged DICON case */
    if (U->local_depth >= MIN_DICON_DEPTH && U->local_depth <= MAX_DICON_DEPTH) {
        add_block(u, U->local_end);
        U->local_depth = 0;
    }
    /* Isolate subsequent DICON (pseudocode: depth == 7) */
    else if (U->local_depth == MAX_DICON_DEPTH + 1) {
        U->local_depth -= M->local_depth;
        U->local_end    = e;
        add_block(m, M->local_end);           /* FIX: m instead of m.local_start */
    }
}

/* Runs the whole pipeline on a CFG. */
static void detect_dicons(CFG *g, int entry)
{
    nblocks = 0;
    remove_back_edges(g, entry);
    compute_end_points(g);
    find_dicon(g, entry, NONE);

    /* FIX: whatever is still open at the top becomes the Top-DICON */
    Node *E = &g->nodes[entry];
    if (E->local_depth > 0)
        add_block(entry, E->local_end);
}

/* ------------------------------------------------------------------ */
/* Printing helper                                                     */
/* ------------------------------------------------------------------ */

static void print_result(const CFG *g, const char *const names[])
{
    printf("%-6s %-6s %-12s %-9s\n", "node", "end", "local_depth", "local_end");
    for (int i = 0; i < g->n; i++) {
        const Node *u = &g->nodes[i];
        printf("%-6s %-6s %-12d %-9s\n", names[i],
               u->end == NONE ? "-" : names[u->end],
               u->local_depth,
               u->local_end == NONE ? "-" : names[u->local_end]);
    }
    printf("\nDetected DICONs:\n");
    for (int i = 0; i < nblocks; i++)
        printf("  DICON%d: begin = %s, end = %s\n",
               i, names[blocks[i].begin], names[blocks[i].end]);
}

/* ------------------------------------------------------------------ */
/* Test: the graph of Fig. 3(b) in the paper                           */
/* ------------------------------------------------------------------ */
/*
 * Node names (see the figure):
 *   T0            top divergence node (E0)
 *   D1            begin of DICON1 (E1)
 *   A             begin of DICON0
 *   L, L1, L2, L3 left if/else inside DICON0
 *   M             node after E2 (E4)
 *   N, N1, N2, N3 inner if/else (E5)
 *   P             right child of M
 *   Q             convergence of M
 *   Z             end of DICON0
 *   B             node after E1 (E6)
 *   C, C1, C2, C3 inner if/else (E7)
 *   K             convergence of B
 *   W             end of DICON1
 *   R0..R3        first if/else on the right side (E0 branch)
 *   S, S1..S3     second if/else on the right side
 *   Y             final node (end of Top DICON)
 */
enum {
    T0, D1, A, L, L1, L2, L3, M, N, N1, N2, N3, P, Q, Z,
    B, C, C1, C2, C3, K, W,
    R0, R1, R2, R3, S, S1, S2, S3, Y,
    FIG3_N
};

static const char *const fig3_names[FIG3_N] = {
    "T0", "D1", "A", "L", "L1", "L2", "L3", "M", "N", "N1", "N2", "N3", "P", "Q", "Z",
    "B", "C", "C1", "C2", "C3", "K", "W",
    "R0", "R1", "R2", "R3", "S", "S1", "S2", "S3", "Y"
};

static void test_fig3(void)
{
    CFG g;
    cfg_init(&g, FIG3_N);

    /* Top DICON */
    add_edge(&g, T0, D1);  add_edge(&g, T0, R0);      /* ~E0 / E0 */

    /* DICON1 */
    add_edge(&g, D1, A);   add_edge(&g, D1, B);       /* ~E1 / E1 */

    /* DICON0 */
    add_edge(&g, A, L);    add_edge(&g, A, M);        /* ~E2 / E2 */
    add_edge(&g, L, L1);   add_edge(&g, L, L2);
    add_edge(&g, L1, L3);  add_edge(&g, L2, L3);
    add_edge(&g, L3, Z);
    add_edge(&g, M, N);    add_edge(&g, M, P);        /* ~E4 / E4 */
    add_edge(&g, N, N1);   add_edge(&g, N, N2);       /* ~E5 / E5 */
    add_edge(&g, N1, N3);  add_edge(&g, N2, N3);
    add_edge(&g, N3, Q);
    add_edge(&g, P, Q);
    add_edge(&g, Q, Z);
    add_edge(&g, Z, W);

    /* right part of DICON1 */
    add_edge(&g, B, C);    add_edge(&g, B, K);        /* ~E6 / E6 */
    add_edge(&g, C, C1);   add_edge(&g, C, C2);       /* ~E7 / E7 */
    add_edge(&g, C1, C3);  add_edge(&g, C2, C3);
    add_edge(&g, C3, K);
    add_edge(&g, K, W);
    add_edge(&g, W, Y);

    /* right side of Top DICON */
    add_edge(&g, R0, R1);  add_edge(&g, R0, R2);
    add_edge(&g, R1, R3);  add_edge(&g, R2, R3);
    add_edge(&g, R3, S);
    add_edge(&g, S, S1);   add_edge(&g, S, S2);
    add_edge(&g, S1, S3);  add_edge(&g, S2, S3);
    add_edge(&g, S3, Y);

    detect_dicons(&g, T0);

    printf("=== Fig. 3(b) ===\n");
    print_result(&g, fig3_names);
}

int main(void)
{
    test_fig3();
    return 0;
}
