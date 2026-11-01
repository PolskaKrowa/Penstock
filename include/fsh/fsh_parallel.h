/* fsh_parallel.h — parallel primitives (report §5.2 + §6.1).
 *
 * The historical runtime exposes:
 *   int32_t frt_parfor(frt_range_t* r, void** ctxout, int32_t maxctx);
 *   int32_t __frt_env_threads;
 *   void frt_init(void);
 *
 * The fsh_parallel layer:
 *   - exposes a typed `fsh_parallel_for` that takes a body fn + range
 *     instead of the low-level frt_range_t struct,
 *   - adds explicit `fsh_reduce_*` for sum / product / min / max with
 *     a deterministic mode (sequential tree) and a fast mode (per-worker
 *     accumulators, reassociated),
 *   - exposes thread id / count / barrier / yield,
 *   - adds atomic operations with a documented memory model.
 *
 * The memory model (report §6.1) is:
 *   - Within one parfor region: writes by iteration i are NOT visible to
 *     iteration j (i != j) — even if they target the same address.
 *     Cross-iteration communication requires a reduction clause or an
 *     atomic operation.
 *   - After the region completes: all writes by all iterations are
 *     visible to the calling thread (release-acquire pairing on the
 *     region's `remaining` counter).
 *   - Nested parfor: outer iteration's writes are visible to inner
 *     iterations of the same outer chunk; sibling outer iterations are
 *     isolated as above.
 */
#ifndef FSH_PARALLEL_H
#define FSH_PARALLEL_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- parallel for ----------------------------------------- */
typedef enum {
  FSH_SCHED_STATIC = 0,   /* T contiguous chunks, one per worker */
  FSH_SCHED_DYNAMIC = 1,  /* many small chunks, work-stealing */
  FSH_SCHED_GUIDED = 2,   /* decreasing chunk size as work drains */
  FSH_SCHED_SPREAD = 3,   /* close affinity: 8T tiny interleaved chunks */
} fsh_sched;

typedef struct {
  int64_t lo, hi;          /* half-open iteration range [lo, hi) */
  int32_t threads;         /* 0 = use default worker count */
  int32_t chunk;           /* 0 = heuristic */
  fsh_sched sched;         /* default: STATIC for uniform bodies */
  int32_t affinity;        /* 0 = no pinning; 1 = pin (best effort) */
} fsh_range;

typedef void (*fsh_body_fn)(int64_t lo, int64_t hi, void* ctx);

/* Runs `body(lo, hi, ctx)` over every chunk of [r->lo, r->hi), in
 * parallel across the worker pool.  Returns the number of chunks
 * dispatched (>= 1 for non-empty ranges, 0 for empty ranges).  The
 * caller's thread participates as worker 0.  Nested calls inside a body
 * are safe (the pool is re-entrant). */
int32_t fsh_parallel_for(const fsh_range* r, fsh_body_fn body, void* ctx);

/* ---------------- reductions ------------------------------------------- */
typedef enum {
  FSH_RED_SUM = 0,
  FSH_RED_PRODUCT = 1,
  FSH_RED_MIN = 2,
  FSH_RED_MAX = 3,
} fsh_red_op;

typedef enum {
  FSH_RED_DETERMINISTIC = 0,  /* same iteration order every run; bit-reproducible */
  FSH_RED_FAST = 1,           /* per-worker accumulators, reassociated */
} fsh_red_mode;

/* fsh_reduce: combines `body(i, ctx)` for i in [lo, hi) using `op`.
 * The accumulator is internal to the call.  For non-numeric reductions
 * (custom associative op) the caller should manage their own accumulator
 * via atomic ops inside the body. */
double fsh_reduce(int64_t lo, int64_t hi, fsh_red_op op, fsh_red_mode mode,
                  double (*body)(int64_t i, void* ctx), void* ctx);

/* ---------------- worker introspection --------------------------------- */
/* Returns the worker count the pool was initialised with (>= 1).  The
 * caller thread is one of them. */
int32_t fsh_thread_count(void);

/* Returns the index of the calling worker in [0, fsh_thread_count()).
 * Returns 0 if called from outside the pool (e.g. before fsh_init). */
int32_t fsh_thread_id(void);

/* ---------------- barriers --------------------------------------------- */
typedef struct {
  int32_t n;                /* number of threads expected */
  _Atomic int32_t count;   /* arrived so far */
  _Atomic int32_t generation;
} fsh_barrier;

void fsh_barrier_init(fsh_barrier* b, int32_t n);
void fsh_barrier_wait(fsh_barrier* b);

/* ---------------- atomics --------------------------------------------- */
/* fsh wraps stdatomic.h with explicit memory orders so the Freshet
 * memory model is documented at the API surface.  Callers that don't
 * care about ordering should use FSH_MO_SEQ_CST; performance-sensitive
 * code can opt down. */
typedef enum {
  FSH_MO_RELAXED = 0,
  FSH_MO_ACQUIRE = 1,
  FSH_MO_RELEASE = 2,
  FSH_MO_ACQ_REL = 3,
  FSH_MO_SEQ_CST = 4,
} fsh_memory_order;

int64_t fsh_atomic_load_i64(const _Atomic int64_t* p, fsh_memory_order mo);
void    fsh_atomic_store_i64(_Atomic int64_t* p, int64_t v, fsh_memory_order mo);
int64_t fsh_atomic_fetch_add_i64(_Atomic int64_t* p, int64_t v, fsh_memory_order mo);

/* ---------------- yield / sleep hint ----------------------------------- */
/* Cheap CPU-spin hint (PAUSE on x86, yield on other targets). */
void fsh_yield_hint(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_PARALLEL_H
