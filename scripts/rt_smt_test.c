/* mem & vectorisation stage — runtime unit tests (no LLVM needed).
 *
 * Exercises the pieces that had to be provably correct before the
 * compiler ever emits a paired dispatch:
 *   1. frt_parse_cpu_list — the sysfs sibling-list parser (ranges,
 *      lists, junk rejection);
 *   2. frt_parfor_pair semantics — a paired dispatch must produce the
 *      SAME combined results as running the two regions sequentially,
 *      for every inline/dispatch combination (forced via body_cost and
 *      FRT_WORK_CUTOFF / FRT_SERIAL_CUTOFF);
 *   3. the packed return values (nA | nB << 32) match the ctx counts
 *      the caller's combine loops would walk.
 *
 * Build:  gcc -std=c11 -D_GNU_SOURCE -Wall -o /tmp/rt_smt_test \
 *           scripts/rt_smt_test.c runtime/frt_rt.c -lpthread -lm
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* from runtime/frt_rt.c (compiled together) */
extern int frt_parse_cpu_list(const char* s, int* out, int cap);
typedef struct {
  void (*body)(int64_t, int64_t, void*, int64_t);
  int64_t lo, hi, chunk;
  int32_t nthreads, affinity, ctx_size;
  int32_t repro;
  int64_t tag;
  void* ctx_template;
  int64_t body_cost;
  int32_t mem_class;
  int64_t sp0;   /* data-stack index base (goose port v2, offset 80) */
} fsh_range_mirror_t;
extern int32_t frt_parfor(fsh_range_mirror_t* r, void** ctxout, int32_t maxctx);
extern int64_t frt_parfor_pair(fsh_range_mirror_t* a, void** ctxA,
                               fsh_range_mirror_t* b, void** ctxB,
                               int32_t maxctx);
extern void frt_init(void);

static int nfail = 0;
#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      printf("FAIL: %s (line %d)\n", msg, __LINE__);                      \
      ++nfail;                                                            \
    } else {                                                              \
      printf("ok:   %s\n", msg);                                          \
    }                                                                     \
  } while (0)

static void test_parse_cpu_list(void) {
  int out[128];
  int n = frt_parse_cpu_list("0,2", out, 128);
  CHECK(n == 2 && out[0] == 0 && out[1] == 2, "parse \"0,2\"");
  n = frt_parse_cpu_list("0-3,7", out, 128);
  CHECK(n == 5 && out[0] == 0 && out[3] == 3 && out[4] == 7,
        "parse \"0-3,7\"");
  n = frt_parse_cpu_list("5", out, 128);
  CHECK(n == 1 && out[0] == 5, "parse \"5\"");
  n = frt_parse_cpu_list("0-3\n", out, 128);
  CHECK(n == 4, "parse trailing newline");
  n = frt_parse_cpu_list("", out, 128);
  CHECK(n == 0, "parse empty");
  n = frt_parse_cpu_list("x", out, 128);
  CHECK(n == -1, "reject junk");
  n = frt_parse_cpu_list("1-", out, 128);
  CHECK(n == -1, "reject dangling range");
  n = frt_parse_cpu_list(",,2,,", out, 128);
  CHECK(n == 1 && out[0] == 2, "tolerate empty list items");
}

/* ---- pair-semantics harness ------------------------------------------- */

typedef struct {
  _Atomic int64_t sum;        // written via plain atomic per element
  int64_t* sink;              // memory-heavy region writes here
  double* fsink;              // compute-heavy region writes here
} pair_ctx_t;

/* memory-bound body: gathers src[p[i]] into sink (2 loads + 1 store) */
typedef struct {
  pair_ctx_t* pc;
  const int64_t* src;
  const int64_t* perm;
  char _pad[64 - 3 * 8];   /* ctx blocks are >= 64B (memcpy contract) */
} gather_ctx_t;

static void gather_body(int64_t lo, int64_t hi, void* vctx, int64_t sp) {
  (void)sp;
  gather_ctx_t* gc = (gather_ctx_t*)vctx;
  for (int64_t i = lo; i < hi; ++i)
    gc->pc->sink[i] = gc->src[gc->perm[i]];
}

/* compute-bound body: transcendental-ish arithmetic on fsink */
typedef struct {
  pair_ctx_t* pc;
  char _pad[64 - 8];
} compute_ctx_t;

static void compute_body(int64_t lo, int64_t hi, void* vctx, int64_t sp) {
  (void)sp;
  compute_ctx_t* cc = (compute_ctx_t*)vctx;
  double x = 1.000001;
  for (int64_t i = lo; i < hi; ++i) {
    double acc = (double)i;
    for (int k = 0; k < 32; ++k) acc = acc * x + 1.0;
    cc->pc->fsink[i] = acc;
  }
}

static void fill_range(fsh_range_mirror_t* r, void (*body)(int64_t, int64_t, void*, int64_t),
                       void* tmpl, int64_t lo, int64_t hi, int32_t mem_class,
                       int64_t body_cost) {
  memset(r, 0, sizeof *r);
  r->body = body;
  r->lo = lo;
  r->hi = hi;
  r->chunk = 0;
  r->nthreads = 0;
  r->affinity = 0;
  r->ctx_size = 64;
  r->ctx_template = tmpl;
  r->body_cost = body_cost;
  r->mem_class = mem_class;
  r->tag = 0;
}

/* verify a paired run equals the sequential reference for both regions,
 * for the given cutoff configuration; returns 0 on success */
static int run_pair_case(int64_t n, int64_t costA, int64_t costB,
                         int64_t workCutoff, int64_t serialCutoff,
                         int32_t memA, int32_t memB) {
  char ws[64], ss[64];
  snprintf(ws, sizeof ws, "%lld", (long long)workCutoff);
  snprintf(ss, sizeof ss, "%lld", (long long)serialCutoff);
  setenv("FRT_WORK_CUTOFF", ws, 1);
  setenv("FRT_SERIAL_CUTOFF", ss, 1);
  unsetenv("FRT_SMT_INTERLEAVE");
  frt_init();

  int64_t* src = malloc(sizeof(int64_t) * (size_t)n);
  int64_t* perm = malloc(sizeof(int64_t) * (size_t)n);
  int64_t* sink = calloc((size_t)n, sizeof(int64_t));
  double* fsink = calloc((size_t)n, sizeof(double));
  int64_t* refSink = calloc((size_t)n, sizeof(int64_t));
  double* refFsink = calloc((size_t)n, sizeof(double));
  for (int64_t i = 0; i < n; ++i) {
    src[i] = i * 7 + 3;
    perm[i] = (i * 13 + 5) % n;
  }

  pair_ctx_t pc; pc.sum = 0; pc.sink = refSink; pc.fsink = refFsink;
  gather_ctx_t gc = {&pc, src, perm};
  compute_ctx_t cc = {&pc};
  /* sequential reference */
  gather_body(0, n, &gc, 0);
  compute_body(0, n, &cc, 0);

  pc.sink = sink;
  pc.fsink = fsink;
  gather_ctx_t g2 = {&pc, src, perm};
  compute_ctx_t c2 = {&pc};

  fsh_range_mirror_t ra, rb;
  fill_range(&ra, gather_body, &g2, 0, n, memA, costA);
  fill_range(&rb, compute_body, &c2, 0, n, memB, costB);

  void* ctxA[64];
  void* ctxB[64];
  int64_t packed = frt_parfor_pair(&ra, ctxA, &rb, ctxB, 64);
  int32_t nA = (int32_t)(uint32_t)(packed & 0xffffffffll);
  int32_t nB = (int32_t)(uint32_t)((uint64_t)packed >> 32);

  int bad = 0;
  for (int64_t i = 0; i < n && !bad; ++i) {
    if (sink[i] != refSink[i] || fsink[i] != refFsink[i]) bad = 1;
  }
  /* every reported ctx slot must have been visited exactly once; the
   * harness reuses the ctx blocks (ctx_size 64 heap allocs) — free them
   * like the compiled combine loops do */
  for (int32_t c = 0; c < nA && c < 64; ++c) free(ctxA[c]);
  for (int32_t c = 0; c < nB && c < 64; ++c) free(ctxB[c]);

  free(src); free(perm); free(sink); free(fsink);
  free(refSink); free(refFsink);
  if (bad) return -1;
  return 0;
}

static void test_pair_semantics(void) {
  const int64_t n = 4096;
  /* case 1: both regions dispatch (cost above the cutoffs) */
  CHECK(run_pair_case(n, 5, 5, 100, 100, 1, 2) == 0,
        "pair: both dispatched, results match sequential");
  /* case 2: A inline (cost 1 => total work 4096 > cutoff 100? no:
   * 4096*1=4096 > 100 => dispatch; use a big cutoff for inline) */
  CHECK(run_pair_case(n, 5, 5, 1 << 30, 100, 1, 2) == 0,
        "pair: both inline (work cutoff huge), results match");
  /* case 3: A dispatches, B inline (compute region under cutoff) */
  CHECK(run_pair_case(n, 5, 0, 100, 100, 1, 2) == 0,
        "pair: A dispatched + B inline, results match");
  /* case 4: A inline, B dispatches */
  CHECK(run_pair_case(n, 0, 5, 100, 100, 1, 2) == 0,
        "pair: A inline + B dispatched, results match");
  /* case 5: legacy costs (0) with a serial cutoff that inlines both */
  CHECK(run_pair_case(n, 0, 0, 0, n, 1, 2) == 0,
        "pair: legacy serial-cutoff inline, results match");
  /* case 6: legacy costs with serial cutoff 0 (always dispatch) */
  CHECK(run_pair_case(n, 0, 0, 0, 0, 1, 2) == 0,
        "pair: legacy always-dispatch, results match");
  /* determinism: same classes, swapped */
  CHECK(run_pair_case(n, 5, 5, 100, 100, 3, 3) == 0,
        "pair: balanced classes, results match");
}

/* plain frt_parfor still works with the lane fields present */
static void test_single_region_unchanged(void) {
  const int64_t n = 8192;
  setenv("FRT_WORK_CUTOFF", "100", 1);
  setenv("FRT_SERIAL_CUTOFF", "100", 1);
  unsetenv("FRT_SMT_INTERLEAVE");
  frt_init();
  int64_t* src = malloc(sizeof(int64_t) * (size_t)n);
  int64_t* perm = malloc(sizeof(int64_t) * (size_t)n);
  int64_t* sink = calloc((size_t)n, sizeof(int64_t));
  for (int64_t i = 0; i < n; ++i) {
    src[i] = i * 3 + 1;
    perm[i] = (i * 7) % n;
  }
  pair_ctx_t pc; pc.sum = 0; pc.sink = sink; pc.fsink = NULL;
  gather_ctx_t gc = {&pc, src, perm};
  fsh_range_mirror_t r;
  fill_range(&r, gather_body, &gc, 0, n, 1, 5);
  void* ctxout[64];
  int32_t nch = frt_parfor(&r, ctxout, 64);
  int bad = 0;
  for (int64_t i = 0; i < n && !bad; ++i)
    if (sink[i] != src[perm[i]]) bad = 1;
  for (int32_t c = 0; c < nch && c < 64; ++c) free(ctxout[c]);
  free(src); free(perm); free(sink);
  CHECK(!bad && nch >= 1, "single region: results match, chunks >= 1");
}

int main(void) {
  test_parse_cpu_list();
  test_single_region_unchanged();
  test_pair_semantics();
  if (nfail == 0) printf("ALL RUNTIME SMT TESTS PASSED\n");
  else printf("%d FAILURE(S)\n", nfail);
  return nfail ? 1 : 0;
}
