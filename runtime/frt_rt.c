/* Freshet runtime (libfreshet).
 *
 * Contract with the compiler (irgen):
 *   - %frt.range layout == frt_range_t below (checked by tests)
 *   - soa descriptor block: { i32 nf @0, i64 n @8, ptr planes[] @16 }
 *   - parfor contexts are heap blocks >= 64B-aligned, ctx_size bytes,
 *     initialized by memcpy from the caller's template
 *
 * Parallelism:
 *   - persistent worker pool (FRT_THREADS or nproc)
 *   - frt_parfor is re-entrant: nested calls from worker threads push tasks
 *     to the same global queue and the caller *helps* (executes tasks)
 *     until its region drains — no thread ever blocks on a region, so
 *     nested parallelism cannot deadlock
 *   - affinity close: T contiguous ranges; spread: 8T small ranges
 *     (interleaved execution order); FRT_AFFINITY=1 additionally pins
 *     threads to cores (best effort — cgroups may refuse)
 *
 * Stage R1 (report §7 Phase 1): the task queue is a fixed-size ring
 *   buffer (head/tail counters).  Previously push was O(1) but pop was
 *   O(n) due to memmove of the remaining queue under a contended mutex.
 *   The ring buffer makes both O(1).
 *
 * Stage R2 (report §7 Phase 1): the timing table is now protected by
 *   its own mutex (g_timing_mu) — concurrent parfor regions can record
 *   timing without racing on g_ntiming or the g_timing[] slots.  The
 *   bogus `static int spin` counter in frt_parfor (which was shared
 *   mutable state across all callers) is replaced by a thread-local
 *   spin counter so each worker's back-off is independent.
 *
 * Stage R3 (report §6.4): x86-specific __builtin_ia32_pause and
 *   __builtin_ia32_rdtsc are now behind FRT_ARCH_* macros so a non-x86
 *   target compiles with portable fallbacks (sched_yield / clock_gettime).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

#define FRT_MAX_THREADS 64
#define FRT_MAXQ 2048

/* Compile-time tuned defaults, baked by `penstock -O4` after the evolution
 * search picks a winner (0 = off, runtime heuristic applies). Env vars
 * FRT_THREADS / FRT_AFFINITY / FRT_CHUNK still win over these. */
#ifndef FRT_TUNED_THREADS
#define FRT_TUNED_THREADS 0
#endif
#ifndef FRT_TUNED_AFFINITY
#define FRT_TUNED_AFFINITY 0
#endif
#ifndef FRT_TUNED_CHUNK
#define FRT_TUNED_CHUNK 0
#endif
/* §10.1 serial cutoff: parfor regions whose iteration count is at most
 * this execute INLINE in the calling thread (one context, no queue, no
 * wake-ups, no atomics) — tiny regions otherwise spend more time in
 * dispatch than in useful work.  -1 = no baked value (heuristic default
 * applies); 0 = always dispatch (historical behaviour). */
#ifndef FRT_TUNED_SERIAL_CUTOFF
#define FRT_TUNED_SERIAL_CUTOFF (-1)
#endif
#define FRT_SERIAL_CUTOFF_DEFAULT 256

/* scheduler observability (§10.1/§13.1): inline vs dispatched region
 * counts, reported in the FRT_TIMING file (ST line) and the perf report */
static _Atomic int64_t g_statSerialRegions = 0;
static _Atomic int64_t g_statDispatchedRegions = 0;

/* ---------------- Stage R3: portable CPU intrinsics --------------------- */
/* The historical runtime called __builtin_ia32_pause() and
 * __builtin_ia32_rdtsc() unconditionally, which made it x86-only.  Behind
 * these macros a non-x86 build falls back to sched_yield() and a
 * monotonic clock, so the runtime compiles on ARM/PPPC/RISC-V targets
 * that have a working pthreads + clock_gettime. */
#if defined(__x86_64__) || defined(__i386__)
  #define FRT_ARCH_PAUSE()  __builtin_ia32_pause()
  #define FRT_ARCH_RDTSC()  ((int64_t)__builtin_ia32_rdtsc())
#else
  /* Portable spin hint: yield the timeslice to the scheduler.  Less
   * efficient than PAUSE on x86 but correct on every SMP target. */
  #define FRT_ARCH_PAUSE()  sched_yield()
  /* No portable rdtsc — fall back to nanoseconds from CLOCK_MONOTONIC.
   * The unit differs (ns vs cycles) so callers must compare in the
   * same build; FRT_TIMING output labels the field "cycles" but the
   * value is ns on non-x86. */
  static int64_t frt_arch_rdtsc_fallback_(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
  }
  #define FRT_ARCH_RDTSC()  frt_arch_rdtsc_fallback_()
#endif

int64_t frt_rdtsc(void);   /* forward: used before definition */

int32_t __frt_env_threads = 0;   /* external global referenced by IR */
static int64_t g_chunkDefault = FRT_TUNED_CHUNK;   /* 0 = heuristic */
static int64_t g_serialCutoff = FRT_TUNED_SERIAL_CUTOFF; /* <0 = default */

typedef void (*frt_body_fn)(int64_t lo, int64_t hi, void* ctx);

typedef struct {
  frt_body_fn body;
  int64_t lo, hi;
  void* ctx;
  _Atomic int64_t* remaining;
} frt_task_t;

typedef struct {
  void (*body)(int64_t, int64_t, void*);
  int64_t lo, hi, chunk;
  int32_t nthreads, affinity, ctx_size;
  /* Phase 5 (report §12): @reproducible flag — the runtime partitions
   * [lo,hi) into FIXED blocks (chunk, else 8192) instead of
   * thread-count-derived chunks, so reduction partials and their
   * combine order are identical for every thread count.  Offset 44
   * matches %frt.range field 8 (checked by tests). */
  int32_t repro;
  int64_t tag;
  void* ctx_template;
  /* optimisation-audit §6.12: per-iteration body-cost class computed by
   * the compiler (ops / memory ops / calls census of one iteration).
   * 0 = legacy IR without the field: the iteration-count cutoff alone
   * decides.  Positive => the runtime compares the ESTIMATED TOTAL WORK
   * (trip_count * body_cost) against the work cutoff instead, so 100
   * cheap and 100 expensive iterations no longer make the same
   * dispatch decision. */
  int64_t body_cost;
} frt_range_t;

/* ---------------- memory ------------------------------------------------ */
void* frt_malloc(int64_t size, int64_t align) {
  if (size <= 0) size = 1;
  if (align < 16) align = 16;
  void* p = NULL;
  if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) {
    fprintf(stderr, "freshet: out of memory (%lld bytes)\n", (long long)size);
    abort();
  }
  return p;
}

void frt_free(void* p) { free(p); }

void frt_memset(void* p, int64_t n, int64_t unused) {
  (void)unused;
  memset(p, 0, (size_t)n);
}
void frt_memcpy(void* d, void* s, int64_t n) {
  memcpy(d, s, (size_t)n);
}

/* ---------------- soa --------------------------------------------------- */
/* Stage B5 / §2.7: overflow-safe allocation sizing.  sizes[i] * n could
 * overflow when both are large; frt_soa_new now checks for overflow
 * before calling frt_malloc and refuses the request instead of allocating
 * a tiny buffer that the caller will then overrun. */
void* frt_soa_new(int64_t n, int32_t nf, int64_t* sizes) {
  if (n < 0 || nf <= 0) return NULL;
  size_t hdr = 16 + 8 * (size_t)nf;
  char* blk = (char*)frt_malloc((int64_t)hdr, 64);
  *(int32_t*)blk = nf;
  *(int64_t*)(blk + 8) = n;
  for (int32_t i = 0; i < nf; i++) {
    void* plane = NULL;
    if (n > 0 && sizes[i] > 0) {
      /* overflow check: sizes[i] * n must not wrap as size_t */
      if (sizes[i] > (int64_t)(SIZE_MAX / (size_t)n)) {
        fprintf(stderr, "freshet: soa_new overflow: plane %d size %lld * "
                        "n %lld exceeds SIZE_MAX\n", i,
                (long long)sizes[i], (long long)n);
        /* free already-allocated planes + the header */
        for (int32_t j = 0; j < i; j++)
          free(*(void**)((char*)blk + 16 + 8 * (size_t)j));
        free(blk);
        return NULL;
      }
      plane = frt_malloc(sizes[i] * n, 64);
    }
    *(void**)(blk + 16 + 8 * (size_t)i) = plane;
  }
  return blk;
}

void frt_soa_free(void* blk) {
  if (!blk) return;
  int32_t nf = *(int32_t*)blk;
  for (int32_t i = 0; i < nf; i++) {
    void* p = *(void**)((char*)blk + 16 + 8 * (size_t)i);
    free(p);
  }
  free(blk);
}

static int nproc_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) n = 1;
  if (n > FRT_MAX_THREADS) n = FRT_MAX_THREADS;
  return (int)n;
}

/* ---------------- Stage R1: ring-buffer task queue ----------------------- */
/* The historical queue was a flat array g_queue[FRT_MAXQ] with a single
 * integer g_qcount.  push appended at the end (O(1)); pop removed from
 * the front by memmove-ing the rest one slot left — O(g_qcount) under
 * the global mutex.  With many short tasks per parfor region this became
 * a serial bottleneck.
 *
 * The new queue is a ring buffer: g_qhead advances on pop, g_qtail
 * advances on push, both modulo FRT_MAXQ.  Both operations are O(1) and
 * touch only their own counter (push touches g_qtail; pop touches
 * g_qhead); the shared slot array is read/written under the same mutex
 * as before but with no memmove.
 *
 * Capacity is FRT_MAXQ-1 because a ring buffer with one extra slot is
 * the standard way to distinguish "full" (tail+1 == head) from
 * "empty" (tail == head). */
static pthread_t g_workers[FRT_MAX_THREADS];
static int g_nworkers = 0;
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qcv = PTHREAD_COND_INITIALIZER;
static frt_task_t g_queue[FRT_MAXQ];
static int g_qhead = 0;       /* ring: next pop slot */
static int g_qtail = 0;      /* ring: next push slot */
static _Atomic int64_t g_activeRegions = 0;
static int g_pinFails = 0;

static inline int ringCount(void) {
  int d = g_qtail - g_qhead;
  return d < 0 ? d + FRT_MAXQ : d;
}

static void push_task(frt_task_t t) {
  pthread_mutex_lock(&g_qmu);
  if (ringCount() >= FRT_MAXQ - 1) {
    /* overflow: run inline (same as historical behaviour) */
    pthread_mutex_unlock(&g_qmu);
    t.body(t.lo, t.hi, t.ctx);
    atomic_fetch_sub_explicit(t.remaining, 1, memory_order_release);
    return;
  }
  g_queue[g_qtail] = t;
  g_qtail = (g_qtail + 1) % FRT_MAXQ;
  pthread_cond_signal(&g_qcv);
  pthread_mutex_unlock(&g_qmu);
}

static int pop_task(frt_task_t* out) {
  pthread_mutex_lock(&g_qmu);
  if (g_qhead == g_qtail) {
    pthread_mutex_unlock(&g_qmu);
    return 0;
  }
  *out = g_queue[g_qhead];
  g_qhead = (g_qhead + 1) % FRT_MAXQ;
  pthread_mutex_unlock(&g_qmu);
  return 1;
}

static void run_task(frt_task_t* t) {
  t->body(t->lo, t->hi, t->ctx);
  atomic_fetch_sub_explicit(t->remaining, 1, memory_order_release);
}

static void* worker_main(void* arg) {
  (void)arg;
  for (;;) {
    frt_task_t t;
    pthread_mutex_lock(&g_qmu);
    while (g_qhead == g_qtail && atomic_load(&g_activeRegions) == 0)
      pthread_cond_wait(&g_qcv, &g_qmu);
    if (g_qhead != g_qtail) {
      t = g_queue[g_qhead];
      g_qhead = (g_qhead + 1) % FRT_MAXQ;
      pthread_mutex_unlock(&g_qmu);
      run_task(&t);
    } else {
      pthread_mutex_unlock(&g_qmu);
      /* active region but queue empty: its tasks are in flight; brief
       * back-off then poll again.  The historical 200us nanosleep is
       * kept because it is a reasonable compromise for nested regions;
       * a future work-stealing scheduler (Phase 2) can replace it. */
      struct timespec ts = {0, 200000};   /* 200us */
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

static void pin_thread(pthread_t th, int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(th, sizeof(set), &set) != 0) g_pinFails++;
}

static void init_pool_body(void) {
  int T = __frt_env_threads > 0 ? __frt_env_threads : nproc_count();
  if (T < 1) T = 1;
  if (T > FRT_MAX_THREADS) T = FRT_MAX_THREADS;
  g_nworkers = T - 1;   /* caller participates */
  const char* aff = getenv("FRT_AFFINITY");
  int pin = (aff && aff[0] == '1') || FRT_TUNED_AFFINITY;
  for (int i = 0; i < g_nworkers; i++) {
    pthread_create(&g_workers[i], NULL, worker_main, NULL);
    if (pin) {
      /* spread workers over cores 1..T-1, main stays near 0 */
      pin_thread(g_workers[i], (i + 1) % T);
    }
  }
}

static void ensure_pool(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, init_pool_body);
}

/* ---------------- parfor dispatch --------------------------------------- */
/* Stage R2: the timing table is protected by its own mutex so concurrent
 * parfor regions (e.g. nested) cannot race on g_ntiming or on a
 * g_timing[] slot.  The bogus `static int spin` in frt_parfor was shared
 * mutable state across all callers; it is now thread_local so each
 * caller's spin counter is independent. */
typedef struct {
  uint64_t tag, cycles;
  int64_t count, nthreads, nchunks;
} frt_timing_ent;
static frt_timing_ent g_timing[4096];
static int g_ntiming = 0;
static int g_timingOn = 0;
static pthread_mutex_t g_timing_mu = PTHREAD_MUTEX_INITIALIZER;

/* §6.12 body-cost-aware work cutoff: run inline when the ESTIMATED TOTAL
 * WORK fits — trip_count * body_cost <= FRT_WORK_CUTOFF (default 2048
 * cost units ≈ 256 iterations of a moderately heavy body).  Env
 * override FRT_WORK_CUTOFF (>0 = threshold, 0 = disable the cost path
 * and use the iteration cutoff only).  Chosen by division so no
 * overflow is possible. */
#ifndef FRT_WORK_CUTOFF_DEFAULT
#define FRT_WORK_CUTOFF_DEFAULT 2048
#endif
static int64_t g_workCutoff = FRT_WORK_CUTOFF_DEFAULT;

int32_t frt_parfor(frt_range_t* r, void** ctxout, int32_t maxctx) {
  ensure_pool();
  int64_t total = r->hi - r->lo;
  if (total <= 0) return 0;

  /* §10.1 serial cutoff: below the threshold the whole range runs inline
   * in the caller — one context, no queue, no wake-up signalling, no
   * active-region accounting.  The combine loop in the compiled caller
   * sees nchunks == 1, so reductions keep their exact serial semantics.
   * Nested regions inside worker threads also take this path, which
   * removes the historical queue contention for small inner regions.
   *
   * §6.12 upgrade: when the compiler filled in body_cost (>0), the
   * decision uses the estimated TOTAL WORK, not the bare iteration
   * count — the limitation documented above is resolved.  The bare
   * iteration cutoff remains as the fallback for legacy ranges. */
  if (maxctx >= 1 &&
      ((r->body_cost > 0 && g_workCutoff > 0 &&
        total <= g_workCutoff / r->body_cost) ||
       (r->body_cost <= 0 && g_serialCutoff > 0 && total <= g_serialCutoff))) {
    void* ctx = frt_malloc(r->ctx_size > 0 ? r->ctx_size : 64, 64);
    memcpy(ctx, r->ctx_template, (size_t)r->ctx_size);
    ctxout[0] = ctx;
    uint64_t t0 = 0;
    if (g_timingOn) t0 = frt_rdtsc();
    r->body(r->lo, r->hi, ctx);
    if (g_timingOn) {
      uint64_t dt = frt_rdtsc() - t0;
      pthread_mutex_lock(&g_timing_mu);
      int found = 0;
      for (int i = 0; i < g_ntiming; i++) {
        if (g_timing[i].tag == r->tag) {
          g_timing[i].cycles += dt;
          g_timing[i].count++;
          g_timing[i].nchunks += 1;
          found = 1;
          break;
        }
      }
      if (!found && g_ntiming < 4096) {
        g_timing[g_ntiming].tag = r->tag;
        g_timing[g_ntiming].cycles = dt;
        g_timing[g_ntiming].count = 1;
        g_timing[g_ntiming].nchunks = 1;
        g_ntiming++;
      }
      pthread_mutex_unlock(&g_timing_mu);
    }
    atomic_fetch_add(&g_statSerialRegions, 1);
    return 1;
  }

  atomic_fetch_add(&g_statDispatchedRegions, 1);

  int T = (r->nthreads > 0 && r->nthreads <= g_nworkers + 1)
            ? r->nthreads : g_nworkers + 1;
  if (T < 1) T = 1;

  int nchunks;
  if (r->repro) {
    /* Phase 5 (report §12): deterministic partition — blocks depend ONLY
     * on (total, block size, maxctx), never on the worker count or the
     * scheduler, so every run combines the same partials in the same
     * order.  maxctx is a compile-time constant, so even the clamp keeps
     * the partition a pure function of the range. */
    int64_t block = r->chunk > 0 ? r->chunk : 8192;
    int64_t nc = (total + block - 1) / block;
    if (nc < 1) nc = 1;
    if (nc > maxctx) nc = maxctx;
    nchunks = (int)nc;
  } else if (r->affinity == 1) {
    nchunks = (int)(total < 8 * T ? total : 8 * T);   /* spread: many small */
  } else {
    nchunks = (int)(total < T ? total : T);           /* close: T big ones */
  }
  /* tuned chunk default (FRT_CHUNK env > -O4 baked default > heuristic) */
  if (r->chunk <= 0) r->chunk = g_chunkDefault;
  /* user chunk is a hint; bound the region count */
  if (r->chunk > 0) {
    int64_t nc = (total + r->chunk - 1) / r->chunk;
    if (nc < 1) nc = 1;
    if (nc < nchunks) nchunks = (int)nc;
  }
  if (nchunks > maxctx) nchunks = maxctx;
  if (nchunks < 1) nchunks = 1;

  int64_t bs = (total + nchunks - 1) / nchunks;
  if (bs < 1) bs = 1;
  nchunks = (int)((total + bs - 1) / bs);

  _Atomic int64_t remaining = nchunks;
  atomic_fetch_add(&g_activeRegions, 1);

  uint64_t t0 = 0;
  if (g_timingOn) t0 = frt_rdtsc();

  for (int c = 0; c < nchunks; c++) {
    int64_t clo = r->lo + (int64_t)c * bs;
    int64_t chi = clo + bs;
    if (chi > r->hi) chi = r->hi;
    void* ctx = frt_malloc(r->ctx_size > 0 ? r->ctx_size : 64, 64);
    memcpy(ctx, r->ctx_template, (size_t)r->ctx_size);
    ctxout[c] = ctx;
    frt_task_t t = {r->body, clo, chi, ctx, &remaining};
    push_task(t);
  }

  /* help until this region drains (never block) */
  /* Stage R2 fix: was `static int spin = 0` — shared across all callers,
   * race-prone.  Now thread_local: each worker's back-off is independent. */
  static __thread int spin = 0;
  while (atomic_load_explicit(&remaining, memory_order_acquire) > 0) {
    frt_task_t t;
    if (pop_task(&t)) {
      run_task(&t);
      spin = 0;   /* got work: reset back-off */
    } else {
      FRT_ARCH_PAUSE();
      /* yield so in-flight workers on other cores make progress */
      if (++spin % 8192 == 0) sched_yield();
    }
  }

  atomic_fetch_sub(&g_activeRegions, 1);

  /* Stage R2: timing-table update is now under g_timing_mu so concurrent
   * parfor regions (nested or sibling) cannot race on the slot array or
   * on g_ntiming. */
  if (g_timingOn) {
    uint64_t dt = frt_rdtsc() - t0;
    pthread_mutex_lock(&g_timing_mu);
    for (int i = 0; i < g_ntiming; i++) {
      if (g_timing[i].tag == r->tag) {
        g_timing[i].cycles += dt;
        g_timing[i].count++;
        g_timing[i].nchunks += nchunks;
        pthread_mutex_unlock(&g_timing_mu);
        return nchunks;
      }
    }
    if (g_ntiming < 4096) {
      g_timing[g_ntiming].tag = r->tag;
      g_timing[g_ntiming].cycles = dt;
      g_timing[g_ntiming].count = 1;
      g_timing[g_ntiming].nchunks = nchunks;
      g_ntiming++;
    }
    pthread_mutex_unlock(&g_timing_mu);
  }
  return nchunks;
}

/* ---------------- misc runtime ------------------------------------------ */
int64_t frt_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
int64_t frt_time_ms(void) { return frt_time_ns() / 1000000; }

void frt_print_str(const char* s, int64_t len) {
  fwrite(s, 1, (size_t)len, stdout);
}
void frt_print_f64(double v, int64_t digits) {
  printf("%.*f", (int)digits, v);
}
void frt_print_i64(int64_t v) { printf("%lld", (long long)v); }
void frt_print_u64(uint64_t v) { printf("%llu", (unsigned long long)v); }
void frt_println(void) { fputc('\n', stdout); }

void frt_assert(int c) {
  if (!c) {
    fprintf(stderr, "freshet: assertion failed\n");
    abort();
  }
}
void frt_pause(void) { FRT_ARCH_PAUSE(); }

/* Cache-line flush used by the language intrinsic and libfsh wrapper.
 * x86 exposes CLFLUSH directly through Clang/GCC's builtin. Other targets
 * have no portable equivalent in C, so treat the hint as a no-op there. */
void frt_clflush(void* p) {
#if defined(__x86_64__) || defined(__SSE2__)
  __builtin_ia32_clflush(p);
#else
  (void)p;
#endif
}

/* ---------------- PGO ---------------------------------------------------- */
static int32_t g_profNf = 0, g_profNb = 0;
static uint64_t* g_profFnHash;
static void* const* g_profFnPtrs;
static uint64_t* g_profBrHash;
static void* const* g_profBrPtrs;

static char* exe_path(void) {
  static char buf[512];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 16);
  if (n <= 0) strcpy(buf, "./a.out");
  else buf[n] = 0;
  return buf;
}

static void frt_prof_write(void) {
  const char* out = getenv("FRT_PROF_OUT");
  char def[512];
  if (!out || !*out) {
    snprintf(def, sizeof def, "%s.fprof", exe_path());
    out = def;
  }
  FILE* f = fopen(out, "w");
  if (!f) return;
  fprintf(f, "FRESHET-PROF 1\n");
  for (int32_t i = 0; i < g_profNf; i++) {
    uint64_t cnt = *(uint64_t*)((void* const*)g_profFnPtrs)[i];
    if (cnt) fprintf(f, "F %llx %llx\n",
                     (unsigned long long)g_profFnHash[i],
                     (unsigned long long)cnt);
  }
  for (int32_t i = 0; i < g_profNb; i++) {
    uint64_t cnt = *(uint64_t*)((void* const*)g_profBrPtrs)[i];
    if (cnt) fprintf(f, "B %llx %llx\n",
                     (unsigned long long)g_profBrHash[i],
                     (unsigned long long)cnt);
  }
  fclose(f);
}

void frt_prof_register(int32_t nf, uint64_t* fnhash, void* const* fnptrs,
                       int32_t nb, uint64_t* brhash, void* const* brptrs) {
  g_profNf = nf;
  g_profFnHash = fnhash;
  g_profFnPtrs = fnptrs;
  g_profNb = nb;
  g_profBrHash = brhash;
  g_profBrPtrs = brptrs;
  static int registered = 0;
  if (!registered) {
    registered = 1;
    atexit(frt_prof_write);
  }
}

/* ---------------- timing file ------------------------------------------- */
/* Stage R2: the atexit handler reads g_timing[] / g_ntiming under
 * g_timing_mu so any in-flight worker (extremely unlikely at exit, but
 * possible during an abnormal shutdown) cannot tear the read. */
static void frt_timing_write(void) {
  const char* out = getenv("FRT_TIMING_OUT");
  char def[512];
  if (!out || !*out) {
    snprintf(def, sizeof def, "%s.frtiming", exe_path());
    out = def;
  }
  FILE* f = fopen(out, "w");
  if (!f) return;
  fprintf(f, "FRESHET-TIMING 1\n");
  pthread_mutex_lock(&g_timing_mu);
  for (int i = 0; i < g_ntiming; i++) {
    fprintf(f, "PF %llx %llx %lld %lld %d\n",
            (unsigned long long)g_timing[i].tag,
            (unsigned long long)g_timing[i].cycles,
            (long long)g_timing[i].count,
            (long long)g_timing[i].nchunks,
            g_pinFails);
  }
  pthread_mutex_unlock(&g_timing_mu);
  /* §10.1 scheduler stats: how many regions ran inline (under the serial
   * cutoff) vs were dispatched to the pool; the perf report surfaces
   * these so the crossover threshold can be tuned from measurements. */
  fprintf(f, "ST %lld %lld\n",
          (long long)atomic_load(&g_statSerialRegions),
          (long long)atomic_load(&g_statDispatchedRegions));
  fclose(f);
}

/* ---------------- init --------------------------------------------------- */
void frt_init(void) {
  const char* t = getenv("FRT_THREADS");
  if (t && *t) {
    int v = atoi(t);
    if (v > 0) __frt_env_threads = v;
  }
#if FRT_TUNED_THREADS > 0
  if (__frt_env_threads == 0) __frt_env_threads = FRT_TUNED_THREADS;
#endif
  if (__frt_env_threads == 0) __frt_env_threads = nproc_count();
  const char* ck = getenv("FRT_CHUNK");
  if (ck && *ck) {
    long long v = atoll(ck);
    if (v > 0) g_chunkDefault = v;
  }
  /* §10.1 serial cutoff: env FRT_SERIAL_CUTOFF (>0 = threshold, 0 =
   * always dispatch) > -O4 baked FRT_TUNED_SERIAL_CUTOFF > default 256.
   * §6.12: env FRT_WORK_CUTOFF overrides the body-cost work threshold
   * (0 disables the cost path). */
  const char* sc = getenv("FRT_SERIAL_CUTOFF");
  if (sc && *sc) {
    long long v = atoll(sc);
    if (v >= 0) g_serialCutoff = v;
  }
  if (g_serialCutoff < 0) g_serialCutoff = FRT_SERIAL_CUTOFF_DEFAULT;
  /* §6.12: body-cost work cutoff override (0 = disable the cost path) */
  const char* wc = getenv("FRT_WORK_CUTOFF");
  if (wc && *wc) {
    long long v = atoll(wc);
    if (v >= 0) g_workCutoff = v;
  }
  const char* tim = getenv("FRT_TIMING");
  if (tim && tim[0] == '1') {
    g_timingOn = 1;
    atexit(frt_timing_write);
  }
}

int64_t frt_rdtsc(void) { return FRT_ARCH_RDTSC(); }

/* ------------------------------------------------------------------ *
 * Phase 1c/2b additions (language gap report §4.1/§4.5): Span bounds
 * traps and the UTF-8 string runtime (str views + owned String).
 * ------------------------------------------------------------------ */

void frt_bound_trap(int64_t idx, int64_t len) {
  fprintf(stderr,
          "freshet: bounds check failed: index %lld is outside 0..%lld\n",
          (long long)idx, (long long)(len - 1));
  abort();
}

int32_t frt_str_eq(const char* a, int64_t la, const char* b, int64_t lb) {
  if (la != lb) return 0;
  return memcmp(a, b, (size_t)la) == 0 ? 1 : 0;
}

/* frt_string layout must match the compiler's prelude String struct:
 *   { char* data; int64_t len; int64_t cap; }   (24 bytes) */
typedef struct {
  char* data;
  int64_t len;
  int64_t cap;
} frt_string;

static void frt_string_reserve(frt_string* s, int64_t need) {
  if (need <= s->cap) return;
  int64_t nc = s->cap ? s->cap : 16;
  while (nc < need) nc *= 2;
  char* nd = (char*)realloc(s->data, (size_t)nc);
  if (!nd) {
    fprintf(stderr, "freshet: out of memory growing a String\n");
    abort();
  }
  s->data = nd;
  s->cap = nc;
}

void frt_string_init(frt_string* out) {
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
}

void frt_string_from(frt_string* out, const char* p, int64_t len) {
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
  frt_string_reserve(out, len ? len : 1);
  if (len > 0) memcpy(out->data, p, (size_t)len);
  out->len = len;
}

void frt_str_cat(frt_string* out, const char* pa, int64_t la,
                 const char* pb, int64_t lb) {
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
  frt_string_reserve(out, la + lb ? la + lb : 1);
  if (la > 0) memcpy(out->data, pa, (size_t)la);
  if (lb > 0) memcpy(out->data + la, pb, (size_t)lb);
  out->len = la + lb;
}

void frt_string_append(frt_string* s, const char* p, int64_t len) {
  if (len <= 0) return;
  frt_string_reserve(s, s->len + len);
  memcpy(s->data + s->len, p, (size_t)len);
  s->len += len;
}

void frt_string_free(frt_string* s) {
  free(s->data);
  s->data = NULL;
  s->len = 0;
  s->cap = 0;
}

void frt_fmt_f64(frt_string* out, double v, int64_t digits) {
  char buf[64];
  int n = snprintf(buf, sizeof buf, "%.*f", (int)digits, v);
  if (n < 0) n = 0;
  frt_string_from(out, buf, n);
}

void frt_fmt_i64(frt_string* out, int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
  if (n < 0) n = 0;
  frt_string_from(out, buf, n);
}

char* frt_str_to_cstr(const char* p, int64_t len) {
  char* out = (char*)malloc((size_t)len + 1);
  if (!out) {
    fprintf(stderr, "freshet: out of memory in str_to_cstr\n");
    abort();
  }
  if (len > 0) memcpy(out, p, (size_t)len);
  out[len] = '\0';
  return out;
}

/* ---------------- test runner (report §10.3) ---------------------------- */
/* frt_test_* backs `penstock test`.  The generated harness main calls
 * frt_test_setup() once, then per @test fn:
 *     frt_test_run("name", name);
 * and finally `return frt_test_summary();`.
 *
 * The sigsetjmp lives in THIS function's frame and the test body is
 * called FROM here — the frame stays live across the test, so the
 * SIGABRT long-jump is well-defined (jumping back into a frame that
 * already returned would be undefined behaviour).  The test function
 * arrives as a C function pointer: the harness passes the Freshet @test
 * fn by name, exercising the fn-pointer feature end-to-end. */
#include <setjmp.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct { const char* p; int64_t len; } frt_str_view_t;

static __thread sigjmp_buf frt_test_jmp;
static __thread int frt_test_active = 0;
static int frt_tests_run = 0;
static int frt_tests_fail = 0;

static void frt_test_abort_handler(int sig) {
  (void)sig;
  if (frt_test_active) {
    frt_test_active = 0;
    siglongjmp(frt_test_jmp, 1);
  }
  /* not inside a test (worker thread / after the run): default */
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
}

void frt_test_setup(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = frt_test_abort_handler;
  sigaction(SIGABRT, &sa, NULL);
}

int32_t frt_test_run(frt_str_view_t name, void (*fn)(void)) {
  printf("test %.*s ... ", (int)name.len, name.p);
  fflush(stdout);
  frt_tests_run++;
  if (sigsetjmp(frt_test_jmp, 1) == 0) {
    frt_test_active = 1;
    fn();                      /* the test body runs in THIS live frame */
    frt_test_active = 0;
    printf("ok\n");
    fflush(stdout);
    return 1;
  }
  /* the test aborted: the handler long-jumped back here */
  printf("FAIL\n");
  fflush(stdout);
  frt_tests_fail++;
  return 0;
}

int32_t frt_test_summary(void) {
  printf("%d/%d tests passed\n", frt_tests_run - frt_tests_fail,
         frt_tests_run);
  return frt_tests_fail ? 1 : 0;
}

/* ---------------- args / env / files (report §4.7/§9) -------------------- */
/* The Freshet-facing surface (via the shipped `fsh` module):
 *   args_count() -> i64          number of program arguments
 *   args_get(i) -> str           argument i (views main's argv — borrowed)
 *   env_get(name) -> str         environment lookup ("" when unset)
 *   file_read(path) -> str       whole file (empty + errno when missing)
 *   file_size(path) -> i64       -1 when missing
 *   file_write(path, data) -> i64  bytes written, -1 on error
 * str values are the { ptr, i64 } view pair, NUL-terminated for C interop
 * where a path is involved. */

static char** frt_main_argv = NULL;
static int frt_main_argc = 0;

/* lazy argv capture: /proc/self/cmdline is NUL-separated; read once on
 * first use (the compiler-emitted main calls frt_init() without args) */
static void frt_args_lazy_init(void) {
  if (frt_main_argv) return;
  static char* argv_slots[4096];
  static char data[262144];
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if (!f) return;
  size_t n = fread(data, 1, sizeof data - 1, f);
  fclose(f);
  if (n == 0) return;
  data[n] = '\0';
  int argc = 0;
  size_t i = 0;
  while (i < n && argc < 4095) {
    argv_slots[argc++] = data + i;
    size_t e = i;
    while (e < n && data[e] != '\0') ++e;
    i = e + 1;
  }
  argv_slots[argc] = NULL;
  frt_main_argc = argc;
  frt_main_argv = argv_slots;
}

int64_t frt_args_count(void) {
  frt_args_lazy_init();
  return frt_main_argc;
}

frt_str_view_t frt_args_get(int64_t i) {
  frt_args_lazy_init();
  frt_str_view_t v = { "", 0 };
  if (frt_main_argv && i >= 0 && i < frt_main_argc) {
    v.p = frt_main_argv[i];
    v.len = (int64_t)strlen(frt_main_argv[i]);
  }
  return v;
}

frt_str_view_t frt_env_get(frt_str_view_t name) {
  frt_str_view_t v = { "", 0 };
  /* name is a str view; getenv needs a NUL-terminated copy */
  char buf[512];
  size_t n = (size_t)(name.len < 511 ? name.len : 511);
  if (name.len > 511) return v;
  memcpy(buf, name.p, n);
  buf[n] = '\0';
  const char* val = getenv(buf);
  if (val) {
    v.p = val;
    v.len = (int64_t)strlen(val);
  }
  return v;
}

int64_t frt_file_size(frt_str_view_t path) {
  char buf[4096];
  if (path.len <= 0 || path.len > 4095) return -1;
  memcpy(buf, path.p, (size_t)path.len);
  buf[path.len] = '\0';
  struct stat st;
  if (stat(buf, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  return (int64_t)st.st_size;
}

/* whole-file read into a FRESH NUL-terminated buffer (caller frees with
 * free()); len == -1 on error (errno set) */
frt_str_view_t frt_file_read(frt_str_view_t path) {
  frt_str_view_t v = { "", 0 };
  int64_t sz = frt_file_size(path);
  if (sz < 0) return v;
  char buf[4096];
  memcpy(buf, path.p, (size_t)path.len);
  buf[path.len] = '\0';
  FILE* f = fopen(buf, "rb");
  if (!f) return v;
  char* data = (char*)malloc((size_t)sz + 1);
  if (!data) { fclose(f); return v; }
  size_t got = fread(data, 1, (size_t)sz, f);
  fclose(f);
  data[got] = '\0';
  v.p = data;
  v.len = (int64_t)got;
  return v;
}

int64_t frt_file_write(frt_str_view_t path, frt_str_view_t data) {
  if (path.len <= 0 || path.len > 4095) return -1;
  char buf[4096];
  memcpy(buf, path.p, (size_t)path.len);
  buf[path.len] = '\0';
  FILE* f = fopen(buf, "wb");
  if (!f) return -1;
  size_t wrote = 0;
  if (data.len > 0) wrote = fwrite(data.p, 1, (size_t)data.len, f);
  int bad = ferror(f) ? 1 : 0;
  fclose(f);
  return bad ? -1 : (int64_t)wrote;
}
