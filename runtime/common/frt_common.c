/* Freshet runtime (libfreshet).
 *
 * Contract with the compiler (irgen):
 *   - %frt.range layout == frt_range_t below (checked by tests)
 *   - soa descriptor block: { i32 nf @0, i64 n @8, ptr planes[] @16 }
 *   - parfor contexts are heap blocks >= 64B-aligned, ctx_size bytes,
 *     initialised by memcpy from the caller's template
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
/* ================================================================ fsht-03:03
 * registry 10.03.02 (ch. 25): the TARGET-INDEPENDENT runtime core.
 *
 * This file compiles unchanged for every ratified target: every
 * platform surface (threads, the worker pool primitives, mutexes,
 * monotonic time, files, args/environment) goes through the platform
 * layer API (frt_platform.h) implemented by runtime/platforms/
 * {linux,windows,wasm,baremetal}/frt_platform.c.  The umbrella
 * runtime/frt_rt.c selects the layer from the compile's target macros —
 * the driver compiles this file WITH the program's --target triple, so
 * the linked runtime object code is built for the exact target
 * (ch. 25 §1: same-target compilation).
 *
 * Link lines are platform-conditional: only the linux layer requests
 * -lpthread; nothing here assumes POSIX (ch. 25 §1).
 *
 * fsht-03:02 additions below: the ownership-tracked streaming I/O
 * (ch. 23 §4) and the parse/numeric module content (ch. 23 §1/§5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <ctype.h>
#include <math.h>
#include "frt_platform.h"

/* knobs: libc getenv (linux/windows/wasi); baremetal compile-time
 * defaults only (ch. 25 §4) */
#if defined(FRT_PLATFORM_BAREMETAL)
#define frt_getenv(name) ((const char*)0)
#else
#define frt_getenv(name) getenv(name)
#endif

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

/* (see the perf block below for the implementation): per-thread
 * hardware counters, declared early because run_task (above the perf
 * block in file order) brackets task bodies with them */
static int g_perfOn = 0;
static __thread int g_pfInstr = -1, g_pfLLC = -1;
static uint64_t frt_perf_read(int fd);
static void timing_record_hw(uint64_t tag, uint64_t instr, uint64_t llc);
static void perf_thread_open(void);

/* ---------------- Stage R3: portable CPU intrinsics --------------------- */
/* The historical runtime called __builtin_ia32_pause() and
 * __builtin_ia32_rdtsc() unconditionally, which made it x86-only.  Behind
 * these macros a non-x86 build falls back to sched_yield() and a
 * monotonic clock, so the runtime compiles on ARM/PPC/RISC-V targets
 * that have a working pthreads + clock_gettime. */
#if defined(__x86_64__) || defined(__i386__)
  #define FRT_ARCH_PAUSE()  __builtin_ia32_pause()
  #define FRT_ARCH_RDTSC()  ((int64_t)__builtin_ia32_rdtsc())
#else
  /* fsht-03:03: portable spin hint and cycle counter come from the
   * platform layer (yield; monotonic ns on non-x86 targets) */
  #define FRT_ARCH_PAUSE()  frt_pal_yield()
  #define FRT_ARCH_RDTSC()  frt_pal_cycles()
#endif

uint64_t frt_rdtsc(void);   /* forward: used before definition */
/* huge-page hint (defined in the memory section, used above) */
static void frt_maybe_huge(void* p, uint64_t bytes);

int32_t __frt_env_threads = 0;   /* external global referenced by IR */
static int64_t g_chunkDefault = FRT_TUNED_CHUNK;   /* 0 = heuristic */
static int64_t g_serialCutoff = FRT_TUNED_SERIAL_CUTOFF; /* <0 = default */

/* goose port v2: the chunk fn takes a hidden 4th argument — the data
 * stack index the dispatching context handed down (frt_range_t.sp0,
 * offset 80).  Every chunk fn of a program shares this one signature,
 * whether its region uses stacks or not, so the runtime holds exactly
 * one fn-ptr type. */
typedef void (*frt_body_fn)(int64_t lo, int64_t hi, void* ctx, int64_t sp);

typedef struct {
  frt_body_fn body;
  int64_t lo, hi;
  void* ctx;
  _Atomic int64_t* remaining;
  int32_t cls;               /* SMT lane class (FRT_CLS_*) */
  int64_t sp;                /* data-stack index base (%sp) */
  uint64_t tag;              /* region tag for counter attribution */
} frt_task_t;

typedef struct {
  void (*body)(int64_t, int64_t, void*, int64_t);
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
  /* mem & vectorisation stage (--smt-interleave): memory/compute census
   * class of the body (0 = unknown, 1 = memory-bound, 2 = compute-bound,
   * 3 = balanced).  Under FRT_SMT_INTERLEAVE=1 the scheduler tags the
   * region's chunk tasks with this class so workers pinned to an SMT
   * sibling that shares its core's ALU prefer the complementary lane:
   * one hardware thread streams memory while the other computes.
   * Offset 72 matches %frt.range field 12. */
  int32_t mem_class;
  /* goose port v2 (%sp threading): the dispatching context's first-free
   * data stack index, copied into every task and passed to the chunk fn
   * as its hidden 4th argument.  Each executing thread — a pool worker
   * or the helping dispatcher — addresses the region's stacks at
   * [sp0, sp0+need) of its OWN thread_local tops array, so concurrent
   * chunks never collide and the caller's live stacks stay disjoint.
   * Offset 80 matches %frt.range field 13 (checked by tests). */
  int64_t sp0;
} frt_range_t;

/* task class for the SMT lanes (0 = any, 1 = memory, 2 = compute) */
#define FRT_CLS_ANY    0
#define FRT_CLS_MEM    1
#define FRT_CLS_COMP   2

/* ---------------- memory ------------------------------------------------ */
void* frt_malloc(int64_t size, int64_t align) {
  if (size <= 0) size = 1;
  if (align < 16) align = 16;
  void* p = NULL;
  if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) {
    fprintf(stderr, "freshet: out of memory (%lld bytes)\n", (long long)size);
    abort();
  }
  frt_maybe_huge(p, (uint64_t)size);
  return p;
}

/* fallible
 * allocation.  The historical frt_malloc abort()s on exhaustion, so a
 * program could not REFUSE to run against an oversized workload
 * (workspace_requirement-then-refuse needs exactly this).  try_malloc
 * returns NULL instead; the language surface is the try_malloc(T, n)
 * builtin returning Option<*T>.  Alignment/huge-page policy identical
 * to the infallible path. */
void* frt_try_malloc(int64_t size, int64_t align) {
  if (size <= 0) size = 1;
  if (align < 16) align = 16;
  void* p = NULL;
  if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) return NULL;
  frt_maybe_huge(p, (uint64_t)size);
  return p;
}

/* explicit alignment control.  malloc(T, n) is aligned to
 * max(16, alignof(T)) only — AVX2 wants 32B, AVX-512 and streaming
 * stores want 64B (ch. 7 gives soa() planes 64B for the same reason).
 * malloc_aligned(T, n, A) is the ownership-tracked form carrying the
 * caller's alignment promise (validated power-of-two >= 16 at sema;
 * clamped here for ABI safety). */
void* frt_malloc_aligned(int64_t size, int64_t align) {
  if (align < 16) align = 16;
  if (align > 4096) align = 4096;   /* pathological promises are clamped */
  void* p = NULL;
  if (posix_memalign(&p, (size_t)align, (size_t)size) != 0) {
    fprintf(stderr, "freshet: out of memory (%lld bytes, align %lld)\n",
            (long long)size, (long long)align);
    abort();
  }
  frt_maybe_huge(p, (uint64_t)size);
  return p;
}

/* transparent huge-page hint.  Opt-in via FRT_HUGEPAGE=1 (or
 * baked FRT_TUNED_HUGEPAGE from -O4); applies to large allocations so
 * page-table pressure on 10^8..10^9-voxel workloads drops.  First touch
 * still determines page placement (documented ch. 25 §4) — the hint only
 * coalesces the pages AFTER the first-touch fault. */
static int g_hugepage = -1;   /* -1 = env not consulted yet */
static void frt_hugepage_init(void) {
  if (g_hugepage >= 0) return;
  g_hugepage = 0;
  const char* hp = frt_getenv("FRT_HUGEPAGE");
  if (hp && hp[0] == '1') g_hugepage = 1;
}
static void frt_maybe_huge(void* p, uint64_t bytes) {
  if (bytes < (2ull << 20)) return;   /* THP only pays from ~2 MiB up */
  frt_hugepage_init();
  if (g_hugepage) frt_pal_madvise_huge(p, bytes);
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

/* fsht-03:03: the cpu count is a platform primitive */
static int nproc_count(void) {
  return frt_pal_ncpu();
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
static frt_pal_thread_t g_workers[FRT_MAX_THREADS];   /* fsht-03:03: pal */
static int g_nworkers = 0;
/* fsht-03:03: the queue lock/condition live in the platform layer */
static frt_task_t g_queue[FRT_MAXQ];
static int g_qhead = 0;       /* ring: next pop slot */
static int g_qtail = 0;      /* ring: next push slot */
static _Atomic int64_t g_activeRegions = 0;
static int g_pinFails = 0;

/* ---------------- mem & vectorisation stage: SMT memory/compute lanes ----
 * Exploits the fact that one physical core may carry two hardware
 * threads that share the core's ALUs and caches: when a memory-bound
 * region and a compute-bound region run concurrently, placing them on
 * the two siblings of one core lets the compute stream fill the memory
 * stream's stall cycles (cache misses / prefetcher turnaround).
 *
 * Topology comes from sysfs:
 *   /sys/devices/system/cpu/cpuN/topology/thread_siblings_list
 * e.g. "0,2" means logical cpus 0 and 2 are the two hardware threads of
 * one physical core.  When FRT_SMT_INTERLEAVE=1 AND siblings exist:
 *   - workers are pinned one per logical cpu (best effort);
 *   - a worker pinned to the LOWER sibling of a pair prefers COMPUTE
 *     tasks, one pinned to the HIGHER sibling prefers MEMORY tasks;
 *   - pop falls back to the queue head after a bounded scan, so the
 *     pool stays work-conserving and can never idle while tasks exist.
 * Without SMT (or with the env off) every task is class 0 and the pop
 * degenerates to the historical FIFO — zero behavioural change. */
#define FRT_MAX_CPU 128
static int g_smtSibling[FRT_MAX_CPU];   /* logical cpu -> sibling or -1 */
static int g_ncpu = 0;                  /* logical cpus seen */
static int g_smtInterleave = 0;         /* env FRT_SMT_INTERLEAVE=1 */
static int g_smtOn = 0;                 /* interleave requested AND found */
static int g_workerPref[FRT_MAX_THREADS];  /* per-worker preferred class */
static _Atomic int64_t g_statPairedRegions = 0;
static _Atomic int64_t g_memLaneChunks = 0;
static _Atomic int64_t g_compLaneChunks = 0;

/* ---------------- NUMA-aware worker placement -------------------
 * Topology: the platform layer reports the number of NUMA nodes that
 * carry CPUs and which node owns each logical cpu.  The pool's PIN ORDER
 * is node-major: the cpu list handed to pinning visits node 0's cpus
 * first, then node 1's, ... so workers 1..T-1 spread evenly over nodes
 * BEFORE filling a second cpu of the same node.  A first-touch page of
 * chunk c is therefore local to the node of the worker that dequeued it.
 *
 * Mapping guarantee (documented, and what FRT_AFFINITY=1 buys):
 *   * close affinity: a region is partitioned into nchunks contiguous
 *     [lo,hi) blocks that are ENQUEUED in ascending base order and
 *     dequeued FIFO under the queue lock — every worker executes a
 *     CONTIGUOUS SUBSEQUENCE of the region in source order.  The worker
 *     that first-touches the pages of a block is the same worker that
 *     touches them for the whole block (no other worker is writing
 *     there: writes are write-conflict-free by the language contract).
 *   * @reproducible: the block partition is a pure function of
 *     (total, block size, maxctx), so a repeated region with the same
 *     schedule lands the same blocks on the same worker order.
 *
 * Memory policy (env FRT_NUMA=local|interleave|off, default off):
 *   * local   — every worker (and the main thread) sets MPOL_BIND to
 *               its own node: first-touch allocations stay node-local.
 *   * interleave — MPOL_INTERLEAVE round-robins pages across nodes
 *               (bandwidth-bound streaming fallback when no locality
 *               structure exists).
 *   Both are best effort: cgroups/cpuset confinement can refuse (the
 * platform layer reports the applied mode; the timing file's NU line
 * records what actually took effect). */
static int g_numaPolicy = 0;            /* 0 off, 1 local, 2 interleave */
static int g_numaNodes = 0;              /* 0 = probed later / unknown */
static int g_pinCpu[FRT_MAX_CPU];        /* node-major pinning order */
static int g_nPinCpu = 0;
static _Atomic int64_t g_statNumaPolicy = 0;   /* mode actually applied */

/* parse a comma-separated cpu list with N and N-M ranges into out[]
 * ("0-3,7"); returns the element count, -1 on syntax error.  Pure and
 * unit-testable: no sysfs involved. */
int frt_parse_cpu_list(const char* s, int* out, int cap);

int frt_parse_cpu_list(const char* s, int* out, int cap) {
  if (!s || !out || cap <= 0) return -1;
  int n = 0;
  const char* p = s;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (!*p || *p == ',') { if (*p == ',') ++p; continue; }
    long a = -1, b = -1;
    char* end = NULL;
    a = strtol(p, &end, 10);
    if (end == p) return -1;
    p = end;
    if (*p == '-') {
      ++p;
      char* end2 = NULL;
      b = strtol(p, &end2, 10);
      if (end2 == p) return -1;
      p = end2;
    } else {
      b = a;
    }
    for (long c = a; c <= b; ++c) {
      if (c < 0 || c >= FRT_MAX_CPU) continue;
      if (n < cap) out[n++] = (int)c;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
    if (*p && *p != ',') return -1;
    if (*p == ',') ++p;
  }
  return n;
}

/* read one logical cpu's sibling list from sysfs; fills sibling[] with
 * the OTHER hardware thread(s) of the same physical core.  Returns the
 * sibling count (0 = none / file missing = offline or no SMT). */
/* fsht-03:03: SMT topology probing is a platform primitive — linux
 * walks /sys; other layers report no siblings (no co-scheduling). */
static void detect_smt(void) {
  g_ncpu = 0;
  g_smtOn = 0;
  int ncpu = frt_pal_ncpu();
  int lim = ncpu < FRT_MAX_CPU ? ncpu : FRT_MAX_CPU;
  for (int c = 0; c < lim; ++c) {
    g_smtSibling[c] = frt_pal_smt_sibling(c);
    g_ncpu = c + 1;
  }
  for (int c = 0; c < g_ncpu; ++c) {
    if (g_smtSibling[c] >= 0 && g_smtSibling[c] != c) {
      g_smtOn = 1;
      break;
    }
  }
}

/* lane class for a region's chunks (0 when routing is off) */
static inline int laneClassOf(const frt_range_t* r) {
  if (!g_smtOn || !r) return FRT_CLS_ANY;
  if (r->mem_class == 1) return FRT_CLS_MEM;
  if (r->mem_class == 2) return FRT_CLS_COMP;
  return FRT_CLS_ANY;
}

static inline int ringCount(void) {
  int d = g_qtail - g_qhead;
  return d < 0 ? d + FRT_MAXQ : d;
}

static void push_task(frt_task_t t) {
  frt_pal_lock();
  if (ringCount() >= FRT_MAXQ - 1) {
    /* overflow: run inline (same as historical behaviour) */
    frt_pal_unlock();
    t.body(t.lo, t.hi, t.ctx, t.sp);
    atomic_fetch_sub_explicit(t.remaining, 1, memory_order_release);
    return;
  }
  g_queue[g_qtail] = t;
  g_qtail = (g_qtail + 1) % FRT_MAXQ;
  frt_pal_cond_signal();
  frt_pal_unlock();
}

static int pop_locked(int pref, frt_task_t* out) {
  /* g_qmu must be held.  Class-aware pop: scan a bounded window from
   * the head for a task matching the worker's preferred SMT lane
   * (cls==pref, or cls==ANY which fits any lane); fall back to the
   * head so the pool stays work-conserving.  pref == FRT_CLS_ANY is
   * exactly the historical FIFO pop. */
  if (g_qhead == g_qtail) return 0;
  if (pref != FRT_CLS_ANY) {
    const int scan = 16;
    int checked = 0;
    for (int s = g_qhead; s != g_qtail && checked < scan; ) {
      if (g_queue[s].cls == pref || g_queue[s].cls == FRT_CLS_ANY) {
        *out = g_queue[s];
        /* close the hole by shifting the kept prefix [g_qhead, s) one
         * slot to the right (towards s); the scanned window is tiny
         * (≤ 16 slots), so this stays cheap under the mutex */
        int pos = s;
        while (pos != g_qhead) {
          int prev = (pos == 0) ? FRT_MAXQ - 1 : pos - 1;
          g_queue[pos] = g_queue[prev];
          pos = prev;
        }
        g_qhead = (g_qhead + 1) % FRT_MAXQ;
        return 1;
      }
      s = (s + 1) % FRT_MAXQ;
      ++checked;
    }
  }
  *out = g_queue[g_qhead];
  g_qhead = (g_qhead + 1) % FRT_MAXQ;
  return 1;
}

static int pop_task(frt_task_t* out) {
  frt_pal_lock();
  int ok = pop_locked(FRT_CLS_ANY, out);
  frt_pal_unlock();
  return ok;
}

static void run_task(frt_task_t* t) {
  /* hardware counters bracket the task body on THIS thread and
   * accumulate on the region's timing entry (tag-attributed; zeroes
   * stay zeroes when the PMU is unavailable) */
  uint64_t i0 = 0, l0 = 0;
  if (g_perfOn) { i0 = frt_perf_read(g_pfInstr); l0 = frt_perf_read(g_pfLLC); }
  t->body(t->lo, t->hi, t->ctx, t->sp);
  if (g_perfOn) {
    uint64_t i1 = frt_perf_read(g_pfInstr), l1 = frt_perf_read(g_pfLLC);
    timing_record_hw(t->tag, i1 - i0, l1 - l0);
  }
  atomic_fetch_sub_explicit(t->remaining, 1, memory_order_release);
}

static void* worker_main(void* arg) {
  int idx = (int)(intptr_t)arg;
  int pref = (idx >= 0 && idx < FRT_MAX_THREADS) ? g_workerPref[idx]
                                                 : FRT_CLS_ANY;
  /* each worker owns its hardware-counter fds before it runs
   * any task (best effort; failure turns the feature off) */
  if (g_perfOn) perf_thread_open();
  /* a pinned worker inherits its node's memory policy before it
   * touches any first-touch page (the policy call is idempotent and
   * per-thread — the main thread applies its own below) */
  if (g_numaPolicy == 1) frt_pal_numa_policy(1);
  for (;;) {
    frt_task_t t;
    frt_pal_lock();
    while (g_qhead == g_qtail && atomic_load(&g_activeRegions) == 0)
      frt_pal_cond_wait();
    if (g_qhead != g_qtail) {
      pop_locked(pref, &t);
      frt_pal_unlock();
      run_task(&t);
    } else {
      frt_pal_unlock();
      /* active region but queue empty: its tasks are in flight; brief
       * back-off then poll again.  The historical 200us nanosleep is
       * kept because it is a reasonable compromise for nested regions;
       * a future work-stealing scheduler (Phase 2) can replace it.
       * fsht-03:03: freestanding builds have no nanosleep — and no
       * worker threads either (pool-of-one), so this branch degrades
       * to the platform yield and keeps the core compiling unchanged. */
      struct timespec ts = {0, 200000};   /* 200us */
#if defined(FRT_PLATFORM_BAREMETAL)
      frt_pal_yield();
#else
      nanosleep(&ts, NULL);
#endif
    }
  }
  return NULL;
}

/* fsht-03:03: affinity is best-effort per platform (windows: the
 * layer may accept it; wasm/baremetal: no-op) — ch. 25 §2 */
static void pin_thread(frt_pal_thread_t* th, int cpu) {
  frt_pal_pin_thread(th, cpu);
}

/* node-major cpu pin order — g_pinCpu[] lists node 0's cpus
 * first, then node 1's, ... so consecutive workers land on distinct nodes
 * before a node's second cpu is used.  Single-node (or unknown-topology)
 * hosts degrade to ascending logical cpu order: the historical
 * behaviour.  Pure topology walk; unit-testable through frt_pal_cpu_node. */
static void build_numa_pin_order(void) {
  int ncpu = frt_pal_ncpu();
  if (ncpu > FRT_MAX_CPU) ncpu = FRT_MAX_CPU;
  g_nPinCpu = 0;
  int nodes = frt_pal_numa_nodes();
  if (nodes <= 1) {
    for (int c = 0; c < ncpu; ++c) g_pinCpu[g_nPinCpu++] = c;
    return;   /* single node: ascending order (historical) */
  }
  for (int n = 0; n < nodes && g_nPinCpu < FRT_MAX_CPU; ++n)
    for (int c = 0; c < ncpu && g_nPinCpu < FRT_MAX_CPU; ++c)
      if (frt_pal_cpu_node(c) == n) g_pinCpu[g_nPinCpu++] = c;
  /* unattributed cpus (node -1) keep a slot at the tail */
  for (int c = 0; c < ncpu && g_nPinCpu < FRT_MAX_CPU; ++c) {
    int seen = 0;
    for (int k = 0; k < g_nPinCpu; ++k) if (g_pinCpu[k] == c) seen = 1;
    if (!seen && frt_pal_cpu_node(c) < 0) g_pinCpu[g_nPinCpu++] = c;
  }
}

static void init_pool_body(void) {
  int T = __frt_env_threads > 0 ? __frt_env_threads : nproc_count();
  if (T < 1) T = 1;
  if (T > FRT_MAX_THREADS) T = FRT_MAX_THREADS;
  g_nworkers = T - 1;   /* caller participates */
  const char* aff = frt_getenv("FRT_AFFINITY");
  int pin = (aff && aff[0] == '1') || FRT_TUNED_AFFINITY;
  /* mem & vectorisation stage: lane pairing is only meaningful when
   * the runtime controls WHERE workers sit, so interleave mode turns
   * pinning on (best effort — cgroups may refuse, counted in
   * g_pinFails).  Workers pinned to the lower sibling of an SMT pair
   * prefer COMPUTE chunks; the higher sibling prefers MEMORY chunks
   * (one thread keeps the shared ALU busy while the other waits on
   * memory). */
  if (g_smtOn) pin = 1;
  /* node-major pin order — build the cpu list that visits node
   * 0's cpus, then node 1's, ... (single-socket machines degrade to the
   * historical ascending order).  Workers spread over nodes before
   * doubling up within one. */
  build_numa_pin_order();
  for (int i = 0; i < g_nworkers; i++) {
    int cpu = g_nPinCpu > 0 ? g_pinCpu[(i + 1) % g_nPinCpu]
                            : (i + 1) % T;
    g_workerPref[i] = FRT_CLS_ANY;
    if (g_smtOn && cpu >= 0 && cpu < FRT_MAX_CPU) {
      int sib = g_smtSibling[cpu];
      if (sib >= 0)
        g_workerPref[i] = (sib > cpu) ? FRT_CLS_COMP : FRT_CLS_MEM;
    }
    /* fsht-03:03: worker creation through the platform layer; wasm and
 * baremetal create none (pool-of-one: parfor serialises, ch. 25 §2) */
    if (frt_pal_thread_create(&g_workers[i], worker_main,
                              (void*)(intptr_t)i) == 0) {
      frt_pal_thread_detach(&g_workers[i]);
    } else {
      g_nworkers = i;   /* pool with what we have (>= 0) */
      break;
    }
    if (pin) {
      /* spread workers over cores 1..T-1, main stays near 0 */
      pin_thread(&g_workers[i], cpu);
    }
  }
}

static void ensure_pool(void) {
  /* fsht-03:03: pthread_once is a platform facility — a plain
   * first-call flag under the queue lock covers every target */
  static int inited = 0;
  frt_pal_lock();
  if (!inited) { inited = 1; init_pool_body(); }
  frt_pal_unlock();
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
  /* (registry 10.06.01, numerical profiling): measured WALL time
   * and ITERATIONS per region entry — the denominators of the report's
   * measured bytes/s and FLOP/s (bytes/s = bytesPerIter * iters / ns).
   * instr/llc are the per-thread hardware-counter deltas aggregated
   * over the region's chunks (0 when FRT_PERF counters are off/unavail-
   * able); they are MEASURED, kept separate from the compiler's static
   * per-iteration census the report multiplies them with. */
  uint64_t ns;
  int64_t iters;
  uint64_t instr, llc;
} frt_timing_ent;
static frt_timing_ent g_timing[4096];
static int g_ntiming = 0;
static int g_timingOn = 0;
/* fsht-03:03: the timing log lock reuses the platform queue lock (the
 * platform layer owns ALL mutexes; a dedicated one would grow the API
 * for a diagnostics-only path) */

/* ---------------- per-thread hardware counters ---------------------
 * FRT_PERF=1 opens cycles / instructions / LLC-miss counters per thread
 * (each pool worker and the main thread own their fds; the counters are
 * read around every task body and the per-region deltas accumulate on
 * the region's timing entry).  Failure to open (no PMU access, a
 * restricted container) degrades to counters-off — the timing file then
 * reports zeros and the report marks hardware data absent, exactly as
 * the policy demands: never invent a measured number. */
static __thread int g_pfCyc = -1;   /* cycles fd (info-only; cycles come
                                      * from rdtsc in the timing entries) */
#if defined(__linux__)
/* minimal perf_event_attr (we only set type/config/size/disabled+inherit
 * flags=0 so the counter runs from open until read) */
#include <sys/types.h>
typedef struct {
  uint32_t type;
  uint64_t config;
} frt_perf_min;
static int frt_perf_open(uint32_t type, uint64_t config) {
  /* perf_event_attr is 72+ bytes on modern kernels; use a zeroed 128-byte
   * buffer and fill the first fields (size at offset 8) */
  unsigned char attr[128];
  memset(attr, 0, sizeof attr);
  *(uint32_t*)(attr + 0) = type;
  *(uint64_t*)(attr + 8) = config;   /* perf_event_attr.config */
  *(uint32_t*)(attr + 16) = 4 + 64;  /* .size */
  long fd = syscall(__NR_perf_event_open, attr, 0 /*self*/, -1 /*any cpu*/,
                    -1 /*group*/, 0 /*flags*/);
  return (int)fd;
}
static uint64_t frt_perf_read(int fd) {
  uint64_t v = 0;
  if (fd >= 0) {
    ssize_t n = read(fd, &v, sizeof v);
    (void)n;
  }
  return v;
}
static void perf_thread_open(void) {
  g_pfCyc = frt_perf_open(0 /*PERF_TYPE_HARDWARE*/, 0 /*CPU_CYCLES*/);
  g_pfInstr = frt_perf_open(0, 1 /*PERF_COUNT_HW_INSTRUCTIONS*/);
  /* PERF_TYPE_HW_CACHE = 3; cache id LL=2, op MISS=1, result=3 -> config
   * (LL | (1<<16) | (3<<24)) = 0x4000003b53? use the canonical spelling */
  g_pfLLC = frt_perf_open(3, (2ULL) | (1ULL << 16) | (3ULL << 24));
  if (g_pfCyc < 0 && g_pfInstr < 0 && g_pfLLC < 0) {
    g_perfOn = 0;   /* no PMU access: counters off for this whole run */
  }
}
#else
static void perf_thread_open(void) { g_perfOn = 0; }
static uint64_t frt_perf_read(int fd) { (void)fd; return 0; }
#endif

static void perf_thread_close(void) {
  if (g_pfCyc >= 0) close(g_pfCyc);
  if (g_pfInstr >= 0) close(g_pfInstr);
  if (g_pfLLC >= 0) close(g_pfLLC);
  g_pfCyc = g_pfInstr = g_pfLLC = -1;
}

/* aggregate per-region counter deltas under the queue lock (the same
 * lock the timing table uses) */
static void timing_record_hw(uint64_t tag, uint64_t instr, uint64_t llc) {
  frt_pal_lock();
  for (int i = 0; i < g_ntiming; i++) {
    if (g_timing[i].tag == tag) {
      g_timing[i].instr += instr;
      g_timing[i].llc += llc;
      frt_pal_unlock();
      return;
    }
  }
  frt_pal_unlock();
}

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

/* ---- shared region helpers (frt_parfor + frt_parfor_pair) -------------- */

/* §10.1/§6.12 serial-cutoff decision for one region */
static int region_should_inline(const frt_range_t* r, int64_t total,
                                int32_t maxctx) {
  if (maxctx < 1) return 0;
  if (r->body_cost > 0 && g_workCutoff > 0)
    return total <= g_workCutoff / r->body_cost;
  if (r->body_cost <= 0 && g_serialCutoff > 0)
    return total <= g_serialCutoff;
  return 0;
}

/* run a region inline in the calling thread (timing + stats included) */
static void region_run_inline(frt_range_t* r, void** ctxout) {
  void* ctx = frt_malloc(r->ctx_size > 0 ? r->ctx_size : 64, 64);
  memcpy(ctx, r->ctx_template, (size_t)r->ctx_size);
  ctxout[0] = ctx;
  uint64_t t0 = 0, n0 = 0;
  if (g_timingOn) { t0 = frt_rdtsc(); n0 = frt_pal_time_ns(); }
  uint64_t i0 = 0, l0 = 0;
  if (g_perfOn) { i0 = frt_perf_read(g_pfInstr); l0 = frt_perf_read(g_pfLLC); }
  r->body(r->lo, r->hi, ctx, r->sp0);
  if (g_perfOn) {
    uint64_t i1 = frt_perf_read(g_pfInstr), l1 = frt_perf_read(g_pfLLC);
    timing_record_hw(r->tag, i1 - i0, l1 - l0);
  }
  if (g_timingOn) {
    uint64_t dt = frt_rdtsc() - t0;
    uint64_t dns = frt_pal_time_ns() - n0;
    int64_t iters = r->hi - r->lo;
    frt_pal_lock();
    int found = 0;
    for (int i = 0; i < g_ntiming; i++) {
      if (g_timing[i].tag == r->tag) {
        g_timing[i].cycles += dt;
        g_timing[i].count++;
        g_timing[i].nchunks += 1;
        g_timing[i].ns += dns;
        g_timing[i].iters += iters;
        found = 1;
        break;
      }
    }
    if (!found && g_ntiming < 4096) {
      g_timing[g_ntiming].tag = r->tag;
      g_timing[g_ntiming].cycles = dt;
      g_timing[g_ntiming].count = 1;
      g_timing[g_ntiming].nchunks = 1;
      g_timing[g_ntiming].ns = dns;
      g_timing[g_ntiming].iters = iters;
      g_ntiming++;
    }
    frt_pal_unlock();
  }
  atomic_fetch_add(&g_statSerialRegions, 1);
}

/* compute the chunk partition (same policy as the historical single
 * region path — reproducible fixed blocks, spread/close affinity,
 * user chunk hints, maxctx clamp); mutates r->chunk exactly like
 * frt_parfor always did and returns nchunks + the block size */
static int region_chunk_plan(frt_range_t* r, int64_t total, int32_t maxctx,
                             int64_t* bsOut) {
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
  *bsOut = bs;
  return nchunks;
}

/* push a region's chunks (lane-class tagged) and fill ctxout */
static void region_dispatch(frt_range_t* r, void** ctxout, int nchunks,
                            int64_t bs, _Atomic int64_t* remaining,
                            int cls) {
  for (int c = 0; c < nchunks; c++) {
    int64_t clo = r->lo + (int64_t)c * bs;
    int64_t chi = clo + bs;
    if (chi > r->hi) chi = r->hi;
    void* ctx = frt_malloc(r->ctx_size > 0 ? r->ctx_size : 64, 64);
    memcpy(ctx, r->ctx_template, (size_t)r->ctx_size);
    ctxout[c] = ctx;
    frt_task_t t = {r->body, clo, chi, ctx, remaining, cls, r->sp0, r->tag};
    push_task(t);
  }
  if (g_smtOn) {
    if (cls == FRT_CLS_MEM)
      atomic_fetch_add(&g_memLaneChunks, nchunks);
    else if (cls == FRT_CLS_COMP)
      atomic_fetch_add(&g_compLaneChunks, nchunks);
  }
}

/* caller-helps loop over one remaining counter (never blocks) */
static void help_drain(_Atomic int64_t* remaining) {
  /* Stage R2 fix: thread_local so each caller's back-off is independent */
  static __thread int spin = 0;
  while (atomic_load_explicit(remaining, memory_order_acquire) > 0) {
    frt_task_t t;
    if (pop_task(&t)) {
      run_task(&t);
      spin = 0;   /* got work: reset back-off */
    } else {
      FRT_ARCH_PAUSE();
      /* yield so in-flight workers on other cores make progress */
      if (++spin % 8192 == 0) frt_pal_yield();
    }
  }
}

/* timing-table update for a dispatched region (Stage R2: under
 * g_timing_mu so concurrent regions cannot race the slot array).
 * the entry gains measured WALL ns and the region's iteration
 * count — the denominators of the report's measured bytes/s and FLOP/s */
static void timing_record(const frt_range_t* r, uint64_t dt, uint64_t dns,
                          int nchunks) {
  if (!g_timingOn) return;
  int64_t iters = r->hi - r->lo;
  frt_pal_lock();
  for (int i = 0; i < g_ntiming; i++) {
    if (g_timing[i].tag == r->tag) {
      g_timing[i].cycles += dt;
      g_timing[i].count++;
      g_timing[i].nchunks += nchunks;
      g_timing[i].ns += dns;
      g_timing[i].iters += iters;
      frt_pal_unlock();
      return;
    }
  }
  if (g_ntiming < 4096) {
    g_timing[g_ntiming].tag = r->tag;
    g_timing[g_ntiming].cycles = dt;
    g_timing[g_ntiming].count = 1;
    g_timing[g_ntiming].nchunks = nchunks;
    g_timing[g_ntiming].ns = dns;
    g_timing[g_ntiming].iters = iters;
    g_ntiming++;
  }
  frt_pal_unlock();
}

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
  if (region_should_inline(r, total, maxctx)) {
    region_run_inline(r, ctxout);
    return 1;
  }

  atomic_fetch_add(&g_statDispatchedRegions, 1);

  int64_t bs = 1;
  int nchunks = region_chunk_plan(r, total, maxctx, &bs);

  _Atomic int64_t remaining = nchunks;
  atomic_fetch_add(&g_activeRegions, 1);

  uint64_t t0 = 0, n0 = 0;
  if (g_timingOn) { t0 = frt_rdtsc(); n0 = frt_pal_time_ns(); }

  region_dispatch(r, ctxout, nchunks, bs, &remaining, laneClassOf(r));

  /* help until this region drains (never block) */
  help_drain(&remaining);

  atomic_fetch_sub(&g_activeRegions, 1);

  /* Stage R2: timing-table update is now under g_timing_mu so concurrent
   * parfor regions (nested or sibling) cannot race on the slot array or
   * on g_ntiming.  Wall-ns rides along. */
  if (g_timingOn)
    timing_record(r, frt_rdtsc() - t0, frt_pal_time_ns() - n0, nchunks);
  return nchunks;
}

/* ---------------- mem & vectorisation stage: paired dispatch ------------- */
/* frt_parfor_pair dispatches TWO independent parfor regions together.
 *
 * The compiler only emits this for adjacent regions it PROVED
 * independent (no shared read/write roots between them, later bounds
 * read nothing the earlier writes).  Semantics: identical to running
 * the two frt_parfor calls in order —
 *   - a region under its serial cutoff runs inline, in source order;
 *   - each dispatched region keeps its own chunk partition and its own
 *     reduction combine order (unchanged by pairing);
 *   - the call returns only when both regions have drained, so the
 *     caller's combine loops see exactly the chunk counts returned.
 *
 * The SMT win: the first region's chunks carry its memory/compute class
 * tag, and so do the second's; with FRT_SMT_INTERLEAVE=1 on an SMT
 * host, workers pinned to sibling hardware threads of one physical
 * core pick complementary lanes, so one thread streams memory (its
 * stalls are cache-miss latency) while the sibling keeps the core's
 * shared ALU executing the other region's arithmetic.
 *
 * Returns (nB << 32) | nA so the compiled caller unpacks both chunk
 * counts from one call. */
int64_t frt_parfor_pair(frt_range_t* a, void** ctxA,
                        frt_range_t* b, void** ctxB, int32_t maxctx) {
  ensure_pool();
  int64_t totalA = a->hi - a->lo;
  int64_t totalB = b->hi - b->lo;

  /* degenerate ranges behave exactly like their frt_parfor calls */
  if (totalA <= 0 && totalB <= 0) return 0;
  if (totalA <= 0) {
    int32_t nB = frt_parfor(b, ctxB, maxctx);
    return (int64_t)(uint32_t)nB << 32;
  }
  if (totalB <= 0) {
    int32_t nA = frt_parfor(a, ctxA, maxctx);
    return (int64_t)(uint32_t)nA;
  }

  uint64_t t0 = 0, n0 = 0;
  if (g_timingOn) { t0 = frt_rdtsc(); n0 = frt_pal_time_ns(); }
  if (g_timingOn) t0 = frt_rdtsc();

  /* serial cutoff per region, in source order */
  int inlineA = region_should_inline(a, totalA, maxctx);
  int inlineB = region_should_inline(b, totalB, maxctx);

  if (inlineA) region_run_inline(a, ctxA);

  _Atomic int64_t remainingA = 0, remainingB = 0;
  int nA = 0, nB = 0;
  int64_t bsA = 1, bsB = 1;
  int clsA = laneClassOf(a), clsB = laneClassOf(b);

  if (!inlineA) {
    atomic_fetch_add(&g_statDispatchedRegions, 1);
    nA = region_chunk_plan(a, totalA, maxctx, &bsA);
    remainingA = nA;
  }
  if (!inlineB) {
    atomic_fetch_add(&g_statDispatchedRegions, 1);
    nB = region_chunk_plan(b, totalB, maxctx, &bsB);
    remainingB = nB;
  }

  if (!inlineA || !inlineB) {
    atomic_fetch_add(&g_activeRegions, 1);
    /* queue BOTH regions before helping: workers on sibling lanes start
     * consuming immediately while the caller is still dispatching */
    if (!inlineA) region_dispatch(a, ctxA, nA, bsA, &remainingA, clsA);
    if (!inlineB) region_dispatch(b, ctxB, nB, bsB, &remainingB, clsB);

    if (inlineB) {
      /* B runs inline in the caller thread WHILE A's chunks execute on
       * the pool — the caller is one of the participants, so this is
       * the classic "compute on this thread, memory on the workers"
       * overlap; A was proven independent of B */
      region_run_inline(b, ctxB);
    }

    /* help until BOTH regions drain (never block on either) */
    for (;;) {
      int64_t ra = atomic_load_explicit(&remainingA, memory_order_acquire);
      int64_t rb = atomic_load_explicit(&remainingB, memory_order_acquire);
      if (ra <= 0 && rb <= 0) break;
      frt_task_t t;
      if (pop_task(&t)) {
        run_task(&t);
      } else {
        FRT_ARCH_PAUSE();
      }
    }
    atomic_fetch_sub(&g_activeRegions, 1);
  } else {
    /* both under the serial cutoff: plain sequential execution in
     * source order — exactly what two frt_parfor calls would do */
    region_run_inline(b, ctxB);
  }

  atomic_fetch_add(&g_statPairedRegions, 1);
  if (g_timingOn) {
    /* one wall-clock entry under the FIRST region's tag; the pair note
     * (SI line) separates paired regions from ordinary ones.  The wall-ns
     * rides along exactly like the single-region path */
    timing_record(a, frt_rdtsc() - t0, frt_pal_time_ns() - n0,
                  nA + nB + (inlineA ? 1 : 0) + (inlineB ? 1 : 0));
  }
  /* an inline region reports nchunks == 1 exactly like frt_parfor would
   * (the caller's combine loop then visits its single ctx) */
  {
    int32_t effA = inlineA ? 1 : (int32_t)nA;
    int32_t effB = inlineB ? 1 : (int32_t)nB;
    return ((int64_t)(uint32_t)effB << 32) | (uint32_t)effA;
  }
}

/* ---------------- misc runtime ------------------------------------------ */
/* fsht-03:03: monotonic time is a platform primitive (unspecified
 * epoch, ch. 23 §5); the rdtsc best-effort counter likewise */
uint64_t frt_time_ns(void) { return frt_pal_time_ns(); }
uint64_t frt_time_ms(void) { return frt_pal_time_ms(); }
uint64_t frt_rdtsc(void) { return frt_pal_cycles(); }
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
#if defined(FRT_PLATFORM_BAREMETAL)
  /* freestanding: no /proc to resolve (callers sit behind
   * frt_pal_has_filesystem() gates, which the layer answers 0) */
  strcpy(buf, "./a.out");
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 16);
  if (n <= 0) strcpy(buf, "./a.out");
  else buf[n] = 0;
#endif
  return buf;
}

static void frt_prof_write(void) {
  /* fsht-03:03 (ch. 25 §4): no filesystem → parsed, ignored no-op */
  if (!frt_pal_has_filesystem()) return;
  const char* out = frt_getenv("FRT_PROF_OUT");
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
  frt_pal_fclose(f);
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
  /* fsht-03:03 (ch. 25 §4): timing output is never part of observable
   * program behaviour; no filesystem → no-op */
  if (!frt_pal_has_filesystem()) return;
  const char* out = frt_getenv("FRT_TIMING_OUT");
  char def[512];
  if (!out || !*out) {
    snprintf(def, sizeof def, "%s.frtiming", exe_path());
    out = def;
  }
  FILE* f = fopen(out, "w");
  if (!f) return;
  /* v2 extends the PF line with <wall-ns> <iters> <instr>
   * <llc-miss> — measured wall time (the report's bytes/s and FLOP/s
   * denominator), measured iteration count (multiplied by the
   * compiler's static per-iter census) and the per-thread hardware
   * counter deltas (0 = counters off/unavailable).  v1 files parse
   * identically because the reader keys on the header. */
  fprintf(f, "FRESHET-TIMING 2\n");
  frt_pal_lock();
  for (int i = 0; i < g_ntiming; i++) {
    fprintf(f, "PF %llx %llx %lld %lld %d %llu %lld %llu %llu\n",
            (unsigned long long)g_timing[i].tag,
            (unsigned long long)g_timing[i].cycles,
            (long long)g_timing[i].count,
            (long long)g_timing[i].nchunks,
            g_pinFails,
            (unsigned long long)g_timing[i].ns,
            (long long)g_timing[i].iters,
            (unsigned long long)g_timing[i].instr,
            (unsigned long long)g_timing[i].llc);
  }
  frt_pal_unlock();
  /* §10.1 scheduler stats: how many regions ran inline (under the serial
   * cutoff) vs were dispatched to the pool; the perf report surfaces
   * these so the crossover threshold can be tuned from measurements. */
  fprintf(f, "ST %lld %lld\n",
          (long long)atomic_load(&g_statSerialRegions),
          (long long)atomic_load(&g_statDispatchedRegions));
  /* mem & vectorisation stage: SMT interleave stats — paired dispatches
   * and how many chunks were routed to each lane (absent when the
   * feature never ran; the report treats it as optional). */
  if (atomic_load(&g_statPairedRegions) > 0) {
    fprintf(f, "SI %lld %lld %lld\n",
            (long long)atomic_load(&g_statPairedRegions),
            (long long)atomic_load(&g_memLaneChunks),
            (long long)atomic_load(&g_compLaneChunks));
  }
  /* NUMA topology + the memory policy the main thread actually
   * got applied (0 = off, 1 = local, 2 = interleave, -1 = refused by
   * the host; nodes>1 with policy 0 is the historical default). */
  fprintf(f, "NU %d %d\n", g_numaNodes,
          (int)atomic_load(&g_statNumaPolicy));
  frt_pal_fclose(f);
}

/* ---------------- positioned I/O / mapped windows ----------------
 * The runtime ABI parfor regions use for disjoint file ranges and
 * billion-voxel checkpoints: pread/pwrite at an absolute offset (the
 * FILE* comes from the sequential layer), fsync/ftruncate for durability
 * and preallocation, and read-only / read-write mmap windows.  The
 * language reaches these through `extern fn` declarations (the runtime
 * is linked into every binary) — (registry 05.08.01 extension)
 * carries the stdlib spelling.
 */
int64_t frt_pread(void* f, void* buf, uint64_t n, int64_t off) {
  return frt_pal_pread(f, buf, n, off);
}
int64_t frt_pwrite(void* f, const void* buf, uint64_t n, int64_t off) {
  return frt_pal_pwrite(f, buf, n, off);
}
int32_t frt_fsync(void* f) { return frt_pal_fsync(f); }
int32_t frt_ftruncate(void* f, int64_t size) {
  return frt_pal_ftruncate(f, size);
}
void* frt_mmap_ro(const char* path, uint64_t* lenOut) {
  return frt_pal_mmap_ro(path, lenOut);
}
void* frt_mmap_rw(const char* path, uint64_t* lenOut) {
  return frt_pal_mmap_rw(path, lenOut);
}
int32_t frt_munmap(void* p, uint64_t len) { return frt_pal_munmap(p, len); }

/* ---------------- machine profile -------------------------------- */
/* One query, cached for the process; accessors below hand the fields to
 * the language's freshet.machine_* intrinsics (ch. 25 §4: the query is
 * observable, side-effect-free and never aborts; 0 = unknown). */
static frt_machine_t g_machine;
static int g_machineValid = 0;
static void frt_machine_init(void) {
  if (g_machineValid) return;
  g_machineValid = 1;
  g_machine.page_bytes = 0;
  frt_pal_query_machine(&g_machine);
  if (g_machine.page_bytes <= 0)
    g_machine.page_bytes = (int64_t)frt_pal_page_size();
  if (g_machine.threads_logical <= 0)
    g_machine.threads_logical = frt_pal_ncpu();
  if (g_machine.numa_nodes <= 0) g_machine.numa_nodes = 1;
}
int32_t frt_query_machine(frt_machine_t* out) {
  frt_machine_init();
  if (!out) return 0;
  *out = g_machine;
  return 1;
}
int64_t frt_cores_physical(void)   { frt_machine_init(); return g_machine.cores_physical; }
int64_t frt_threads_logical(void)  { frt_machine_init(); return g_machine.threads_logical; }
int64_t frt_smt_on(void)           { frt_machine_init(); return g_machine.smt; }
int64_t frt_numa_nodes(void)       { frt_machine_init(); return g_machine.numa_nodes; }
int64_t frt_l1d_bytes(void)        { frt_machine_init(); return g_machine.l1d_bytes; }
int64_t frt_l2_bytes(void)         { frt_machine_init(); return g_machine.l2_bytes; }
int64_t frt_l3_bytes(void)         { frt_machine_init(); return g_machine.l3_bytes; }
int64_t frt_page_bytes(void)       { frt_machine_init(); return g_machine.page_bytes; }
int64_t frt_mem_total_bytes(void)      { frt_machine_init(); return g_machine.mem_total_bytes; }
int64_t frt_mem_available_bytes(void)  { frt_machine_init(); return g_machine.mem_available_bytes; }

/* ---------------- ISA ladder for --march-levels -------------------
 * The canonical ladder the compiler's --march-levels versions index:
 *   1 = x86-64 (baseline), 2 = x86-64-v2, 3 = x86-64-v3 (AVX2+FMA),
 *   4 = x86-64-v4 (AVX-512).  frt_march_level() reports the HIGHEST
 * ladder rung the running CPU supports (CPUID via the compiler's
 * __builtin_cpu_supports, which the backend compiles against the real
 * target).  Non-x86 targets report the baseline (1) — the compiler
 * rejects --march-levels for them at the driver level.  The ifunc
 * resolver the compiler emits selects the best REQUESTED rung <= this.
 */
static int32_t g_marchLevel = -1;
int32_t frt_march_level(void) {
  if (g_marchLevel >= 0) return g_marchLevel;
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
      __builtin_cpu_supports("avx512cd") && __builtin_cpu_supports("avx512dq") &&
      __builtin_cpu_supports("avx512vl"))
    g_marchLevel = 4;
  else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
    g_marchLevel = 3;
  else if (__builtin_cpu_supports("sse4.2"))
    g_marchLevel = 2;
  else
    g_marchLevel = 1;
#else
  g_marchLevel = 1;   /* baseline on non-x86 targets */
#endif
  return g_marchLevel;
}

/* ---------------- init --------------------------------------------------- */
void frt_init(void) {
  const char* t = frt_getenv("FRT_THREADS");
  if (t && *t) {
    int v = atoi(t);
    if (v > 0) __frt_env_threads = v;
  }
#if FRT_TUNED_THREADS > 0
  if (__frt_env_threads == 0) __frt_env_threads = FRT_TUNED_THREADS;
#endif
  if (__frt_env_threads == 0) __frt_env_threads = nproc_count();
  const char* ck = frt_getenv("FRT_CHUNK");
  if (ck && *ck) {
    long long v = atoll(ck);
    if (v > 0) g_chunkDefault = v;
  }
  /* §10.1 serial cutoff: env FRT_SERIAL_CUTOFF (>0 = threshold, 0 =
   * always dispatch) > -O4 baked FRT_TUNED_SERIAL_CUTOFF > default 256.
   * §6.12: env FRT_WORK_CUTOFF overrides the body-cost work threshold
   * (0 disables the cost path). */
  const char* sc = frt_getenv("FRT_SERIAL_CUTOFF");
  if (sc && *sc) {
    long long v = atoll(sc);
    if (v >= 0) g_serialCutoff = v;
  }
  if (g_serialCutoff < 0) g_serialCutoff = FRT_SERIAL_CUTOFF_DEFAULT;
  /* §6.12: body-cost work cutoff override (0 = disable the cost path) */
  const char* wc = frt_getenv("FRT_WORK_CUTOFF");
  if (wc && *wc) {
    long long v = atoll(wc);
    if (v >= 0) g_workCutoff = v;
  }
  const char* tim = frt_getenv("FRT_TIMING");
  if (tim && tim[0] == '1') {
    g_timingOn = 1;
    atexit(frt_timing_write);
  }
  /* (registry 10.06.01): hardware counters per thread.  Off by
   * default; FRT_PERF=1 opts in.  Opening on the MAIN thread here
   * (workers open theirs at start); a refused PMU turns the feature
   * off for the whole run — the report then honestly prints absent. */
  const char* pf = frt_getenv("FRT_PERF");
  if (pf && pf[0] == '1') {
    g_perfOn = 1;
    perf_thread_open();
  }
  /* mem & vectorisation stage: SMT memory/compute interleave.  Off by
   * default; when enabled, the sysfs topology is probed once and the
   * worker pool gains lane preferences (see the SMT lanes block).  The
   * probe is skipped entirely for default runs — zero cost. */
  const char* si = frt_getenv("FRT_SMT_INTERLEAVE");
  if (si && si[0] == '1') {
    g_smtInterleave = 1;
    detect_smt();
  }
  /* FRT_NUMA=local|interleave (anything else = off).  The main
   * thread applies the policy NOW, before the program's first touch of
   * any big allocation; the pool workers apply theirs at thread start.
   * g_numaNodes is cached for the report/timing line. */
  {
    const char* np = frt_getenv("FRT_NUMA");
    if (np && np[0] == 'l') g_numaPolicy = 1;        /* local */
    else if (np && np[0] == 'i') g_numaPolicy = 2;   /* interleave */
    g_numaNodes = frt_pal_numa_nodes();
    if (g_numaNodes > 1) {
      int applied = frt_pal_numa_policy(g_numaPolicy);
      atomic_store(&g_statNumaPolicy, applied);
    }
  }
}


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

/* ------------------------------------------------------------------ *
 * Data stacks (goose port: aardappel/goose data-stack memory model).
 *
 * A stack_alloc(T, n) site bumps a per-thread region's top pointer; the
 * generated code never checks a capacity — each region is reserved up
 * front (usable bytes RW, MAP_NORESERVE) with a PROT_NONE guard page
 * behind it, so pathological growth walks into the guard and the SIGSEGV
 * handler below reports a clean overflow and exits (goose:
 * gs_reserve_region's halving retry + gs_fault_filter).  Deallocation
 * is the scope-exit watermark restore the compiler emits — this file
 * never frees a region until the thread dies (the OS reclaims it).
 *
 * frt_dstk_init(tops, n) is called from the generated entry check of
 * every stack-using function; it is idempotent per thread.  The one
 * registry allocation per thread is this runtime's own bookkeeping
 * (like the queue nodes), never user-visible memory.
 * ------------------------------------------------------------------ */
#if !defined(_WIN32)
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#endif

#define FRT_DSTK_RESERVE_DEFAULT (1024ull << 20) /* 1 GiB usable per stack */
#define FRT_DSTK_GUARD           (1ull << 20)    /* 1 MiB PROT_NONE tail   */
#define FRT_DSTK_MIN_USABLE      (1ull << 20)    /* halving retry floor    */

typedef struct {
  char* base;
  uint64_t usable;   /* RW bytes; [usable, usable+guard) is the guard */
} frt_dstk_reg;

static __thread frt_dstk_reg* frt_dstk_regs = NULL;
static __thread int32_t frt_dstk_nregs = 0;
static volatile int frt_dstk_handler_on = 0;

/* fsht-03:03: the data-stack reservation is a platform primitive.
 * linux/windows reserve a guard tail (PROT_NONE / PAGE_NOACCESS);
 * wasm/baremetal hand back plain memory and CLAMP the request
 * (ch. 25 §2 SHOULD). */
static char* frt_dstk_reserve(frt_dstk_reg* r) {
  uint64_t want = FRT_DSTK_RESERVE_DEFAULT;
  const char* env = frt_getenv("FRT_DSTK_RESERVE");
  if (env && *env) {
    long long v = atoll(env);
    if (v > 0) want = (uint64_t)v;
  }
  for (;;) {
    uint64_t usable = 0;
    void* p = frt_pal_dstk_reserve(&usable, want, NULL);
    if (p) {
      r->base = (char*)p;
      r->usable = usable;
      return (char*)p;
    }
    /* refused — halve and retry (goose), never above the floor */
    if (want <= FRT_DSTK_MIN_USABLE) return NULL;
    want /= 2;
  }
}

/* fsht-03:03: the guard-page fault handler is a linux/posix facility;
 * other platforms get plain-memory stacks without a guard (clamped) */
#if defined(FRT_PAL_DSTK_GUARD)
static void frt_dstk_overflow_msg(const char* p, size_t n) {
  (void)write(2, p, n);   /* async-signal-safe */
}

static void frt_dstk_handler(int sig, siginfo_t* info, void* uctx) {
  (void)uctx;
  char* at = info ? (char*)info->si_addr : NULL;
  for (int32_t i = 0; i < frt_dstk_nregs; ++i) {
    frt_dstk_reg* r = &frt_dstk_regs[i];
    if (at && r->base && at >= r->base + r->usable &&
        at < r->base + r->usable + FRT_DSTK_GUARD) {
      frt_dstk_overflow_msg(
          "freshet: data stack overflow: a value on a data stack grew past "
          "the space reserved for it (FRT_DSTK_RESERVE raises the "
          "reservation; default 1 GiB per stack)\n",
          178);
      _exit(134);
    }
  }
  /* not ours: restore the default action and re-execute the fault */
  signal(sig, SIG_DFL);
}
#endif /* FRT_PAL_DSTK_GUARD */

void frt_dstk_init(void** tops, int32_t n) {
  if (frt_dstk_regs || n <= 0) return;   /* idempotent per thread */
  frt_dstk_reg* regs =
      (frt_dstk_reg*)calloc((size_t)n, sizeof(frt_dstk_reg));
  if (!regs) {
    fprintf(stderr, "freshet: data stack registry allocation failed\n");
    abort();
  }
  for (int32_t i = 0; i < n; ++i) {
    char* p = frt_dstk_reserve(&regs[i]);
    if (!p) {
      fprintf(stderr,
              "freshet: data stack region %d could not be reserved\n", i);
      abort();
    }
    tops[i] = p;
  }
  frt_dstk_regs = regs;
  frt_dstk_nregs = n;
#if defined(FRT_PAL_DSTK_GUARD)
  if (!frt_dstk_handler_on) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = frt_dstk_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    frt_dstk_handler_on = 1;
  }
#endif
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
  /* fsht-03:01 (registry 05.04.01) correction: the historical fixed
   * buf[64] trusted snprintf's return value, which is the WOULD-BE
   * length once the output is truncated — frt_string_from then memcpy'd
   * that many bytes out of a 64-byte stack buffer (overread past the
   * frame, corrupted Strings, and the round-trip law
   * parse(fmt(x)) == Ok(x) broke for |v| >= ~1e46, where %.17f needs
   * more than 63 characters).  Size first, format second. */
  if (digits < 0) digits = 0;
  int need = snprintf(NULL, 0, "%.*f", (int)digits, v);
  if (need <= 0) { frt_string_from(out, "", 0); return; }
  char* buf = (char*)malloc((size_t)need + 1);
  if (!buf) { frt_string_from(out, "", 0); return; }
  snprintf(buf, (size_t)need + 1, "%.*f", (int)digits, v);
  frt_string_from(out, buf, need);
  free(buf);
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

/* fsht-03:02 (ch. 24 §4): the String migration's new operations.
 * frt_string_clone deep-copies the buffer into `out` — READ-THEN-WRITE
 * so out == src (the in-place vec_clone element loop) is safe.  Drops
 * remain idempotent through the nulled data pointer. */
void frt_string_clone(frt_string* out, const frt_string* s) {
  const char* p = s->data;
  int64_t len = s->len;
  out->data = NULL;
  out->len = 0;
  out->cap = 0;
  frt_string_reserve(out, len ? len : 1);
  if (len > 0) memcpy(out->data, p, (size_t)len);
  out->len = len;
}

void frt_string_clear(frt_string* s) {
  s->len = 0;   /* capacity and buffer stay: no reallocation churn */
}

/* ---------------- fsht-03:01 (registry 05.04.01) numeric text parsing ---- */
/* parse_i64/u64/f64 over str views.  Grammar (ch. 23 §1):
 *   - leading/trailing ASCII whitespace (space, tab, \r, \n) skipped
 *   - remainder must be non-empty and well-formed, else Invalid
 *   - ints: optional +/- (minus rejected by parse_u64 as Invalid),
 *     decimal digits only, OutOfRange on overflow — never wrapped
 *   - f64: decimal point, optional exponent (e/E, optional sign), and
 *     inf / -inf / nan (case-insensitive); out-of-range magnitudes are
 *     OutOfRange; a syntactically valid but non-number tail is Invalid.
 * Error codes: 0 = Ok, 1 = Invalid, 2 = OutOfRange (ParseError mapping).
 */
typedef struct { int64_t v; int32_t err; } frt_parse_int;
typedef struct { double v; int32_t err; } frt_parse_dbl;

static int frt_isspace_ascii(int c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

frt_parse_int frt_parse_i64(const char* p, int64_t len) {
  frt_parse_int r = { 0, 1 };
  int64_t b = 0, e = len;
  while (b < e && frt_isspace_ascii((unsigned char)p[b])) ++b;
  while (e > b && frt_isspace_ascii((unsigned char)p[e - 1])) --e;
  if (b >= e) return r;
  int neg = 0;
  int64_t i = b;
  if (p[i] == '+' || p[i] == '-') { neg = p[i] == '-'; ++i; }
  if (i >= e) return r;                       /* sign with no digits */
  /* accumulate the magnitude in 128-bit so the exact 2^63 (i64 min)
   * passes while anything beyond the type range reports OutOfRange */
  unsigned __int128 acc = 0;
  uint64_t limit = neg ? 9223372036854775808ULL   /* 2^63 */
                       : 9223372036854775807ULL;  /* 2^63 - 1 */
  for (; i < e; ++i) {
    if (p[i] < '0' || p[i] > '9') return r;   /* non-digit -> Invalid */
    acc = acc * 10 + (unsigned)(p[i] - '0');
    if (acc > limit) { r.err = 2; return r; }  /* overflow, never wrapped */
  }
  uint64_t m = (uint64_t)acc;
  r.v = neg ? (int64_t)(~m + 1ULL) : (int64_t)m;
  r.err = 0;
  return r;
}

frt_parse_int frt_parse_u64(const char* p, int64_t len) {
  frt_parse_int r = { 0, 1 };
  int64_t b = 0, e = len;
  while (b < e && frt_isspace_ascii((unsigned char)p[b])) ++b;
  while (e > b && frt_isspace_ascii((unsigned char)p[e - 1])) --e;
  if (b >= e) return r;
  int64_t i = b;
  if (p[i] == '+') { ++i; }
  else if (p[i] == '-') return r;             /* minus is Invalid */
  if (i >= e) return r;
  uint64_t acc = 0;
  for (; i < e; ++i) {
    if (p[i] < '0' || p[i] > '9') return r;
    uint64_t d = (uint64_t)(p[i] - '0');
    if (acc > (UINT64_MAX - d) / 10ULL) { r.err = 2; return r; }
    acc = acc * 10ULL + d;
  }
  r.v = (int64_t)acc;
  r.err = 0;
  return r;
}

static int frt_ieq(const char* p, int64_t b, int64_t e, const char* word) {
  size_t n = strlen(word);
  if ((size_t)(e - b) != n) return 0;
  for (size_t i = 0; i < n; ++i)
    if (tolower((unsigned char)p[b + (int64_t)i]) != word[i]) return 0;
  return 1;
}

frt_parse_dbl frt_parse_f64(const char* p, int64_t len) {
  frt_parse_dbl r = { 0.0, 1 };
  int64_t b = 0, e = len;
  while (b < e && frt_isspace_ascii((unsigned char)p[b])) ++b;
  while (e > b && frt_isspace_ascii((unsigned char)p[e - 1])) --e;
  if (b >= e) return r;
  int neg = 0;
  int64_t i = b;
  if (p[i] == '+' || p[i] == '-') { neg = p[i] == '-'; ++i; }
  /* inf / nan spellings (case-insensitive), after the optional sign */
  if (frt_ieq(p, i, e, "inf")) {
    r.v = neg ? -INFINITY : INFINITY;
    r.err = 0;
    return r;
  }
  if (frt_ieq(p, i, e, "nan")) {
    r.v = neg ? -NAN : NAN;
    r.err = 0;
    return r;
  }
  /* [digits] [. digits] [ (e|E) [+|-] digits ] — at least one digit in
   * the mantissa; exponent digits are required once e/E appears */
  int64_t j = i;
  int mantDigits = 0;
  while (j < e && p[j] >= '0' && p[j] <= '9') { ++j; ++mantDigits; }
  if (j < e && p[j] == '.') {
    ++j;
    while (j < e && p[j] >= '0' && p[j] <= '9') { ++j; ++mantDigits; }
  }
  if (mantDigits == 0) return r;               /* no mantissa digits */
  int64_t endMant = j;
  int hasExp = 0;
  if (j < e && (p[j] == 'e' || p[j] == 'E')) {
    ++j;
    if (j < e && (p[j] == '+' || p[j] == '-')) ++j;
    int expDigits = 0;
    while (j < e && p[j] >= '0' && p[j] <= '9') { ++j; ++expDigits; }
    if (expDigits == 0) return r;             /* e with no digits */
    hasExp = 1;
  }
  if (j != e) return r;                       /* trailing junk */
  /* strtod on the trimmed, NUL-terminated copy */
  char buf[512];
  int64_t n = endMant - i + (hasExp ? (j - endMant) : 0);
  if (n >= (int64_t)sizeof buf) { r.err = 2; return r; }
  if (neg) buf[0] = '-';
  memcpy(buf + (neg ? 1 : 0), p + i, (size_t)(j - i));
  buf[neg ? 1 + n : n] = '\0';
#if !defined(FRT_PLATFORM_BAREMETAL)
  errno = 0;
#endif
  char* endp = NULL;
  double v = strtod(buf, &endp);
  if (endp != buf + strlen(buf)) return r;
#if !defined(FRT_PLATFORM_BAREMETAL)
  /* errno/ERANGE come with the hosted libc the strtod above needs
   * (baremetal builds cannot reach this code — no libc strtod at all —
   * but the translation unit must still compile, ch. 25 §1) */
  if (errno == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) { r.err = 2; return r; }
#endif
  /* magnitude overflow to infinity is OutOfRange; underflow denormals
   * stay Ok (strtod sets ERANGE for them too but the value is defined) */
  if (isinf(v)) { r.err = 2; return r; }
  r.v = v;
  r.err = 0;
  return r;
}

/* ---------------- fsht-03:01 (ch. 23 §5) module minimum content -------- */
/* random: xorshift64* — shifts 12/25/27, multiplier 2685821657736338717,
 * the same generator as the conformance fuzz/benchmark harnesses. */
uint64_t frt_sm64_next(uint64_t* state) {
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 2685821657736338717ULL;
}

/* alloc: the page size of the running host (SHOULD-level content; a
 * per-target statement belongs to the platform layer). */
int64_t frt_page_size(void) { return (int64_t)frt_pal_page_size(); }

/* numeric: Euclid's gcd over i64 (magnitude semantics; gcd(0,x) = |x|). */
int64_t frt_gcd_i64(int64_t a, int64_t b) {
  uint64_t x = a < 0 ? (uint64_t)(-a) : (uint64_t)a;
  uint64_t y = b < 0 ? (uint64_t)(-b) : (uint64_t)b;
  while (y) { uint64_t t = x % y; x = y; y = t; }
  return (int64_t)x;
}


/* ---------------- dynamic collections (fsht-02:01) ---------------------- */
/* frt_vec layout must match the compiler's prelude Vec<T> struct:
 *   { void* data; int64_t len; int64_t cap; }   (24 bytes)
 * esz is the monomorphised element size; elements are copied bytewise
 * (POD only — owning element types are rejected at compile time in the
 * fsht-02:01 first cut).  Every release path nulls `data` so the
 * compiler's scope-exit drops and move null-out keep this idempotent. */
typedef struct {
  void* data;
  int64_t len;
  int64_t cap;
} frt_vec;

static void frt_vec_reserve(frt_vec* v, int64_t need, int64_t esz) {
  if (need <= v->cap) return;
  if (need < 0 || esz <= 0 ||
      need > (int64_t)(INT64_MAX / 2) / esz) {
    fprintf(stderr, "freshet: Vec capacity overflow (%lld x %lld bytes)\n",
            (long long)need, (long long)esz);
    abort();
  }
  int64_t nc = v->cap ? v->cap : 8;
  while (nc < need) nc *= 2;
  void* nd = realloc(v->data, (size_t)nc * (size_t)esz);
  if (!nd) {
    fprintf(stderr, "freshet: out of memory growing a Vec\n");
    abort();
  }
  v->data = nd;
  v->cap = nc;
}

void frt_vec_init(frt_vec* v) {
  v->data = NULL;
  v->len = 0;
  v->cap = 0;
}

void frt_vec_with_cap(frt_vec* v, int64_t cap, int64_t esz) {
  v->data = NULL;
  v->len = 0;
  v->cap = 0;
  if (cap > 0) frt_vec_reserve(v, cap, esz);
}

void frt_vec_from(frt_vec* v, const void* p, int64_t n, int64_t esz) {
  v->data = NULL;
  v->len = 0;
  v->cap = 0;
  if (n > 0) {
    frt_vec_reserve(v, n, esz);
    memcpy(v->data, p, (size_t)n * (size_t)esz);
  }
  v->len = n > 0 ? n : 0;
}

void frt_vec_clone(frt_vec* out, const frt_vec* src, int64_t esz) {
  frt_vec_from(out, src->data, src->len, esz);
}

/* fsht-03:02 (ch. 24 §5): aliasing-SAFE vec clone for the in-place deep
 * element copy of vec_clone (out may == src): read the source's
 * (data, len) BEFORE any write to out, then rebuild. */
void frt_vec_clone_aliased(frt_vec* out, const frt_vec* src, int64_t esz) {
  const void* p = src->data;
  int64_t n = src->len;
  frt_vec_from(out, p, n, esz);
}

void frt_vec_push(frt_vec* v, const void* e, int64_t esz) {
  frt_vec_reserve(v, v->len + 1, esz);
  memcpy((char*)v->data + (size_t)v->len * (size_t)esz, e, (size_t)esz);
  v->len += 1;
}

int32_t frt_vec_pop(frt_vec* v, void* out, int64_t esz) {
  if (v->len <= 0) return 0;
  v->len -= 1;
  memcpy(out, (const char*)v->data + (size_t)v->len * (size_t)esz,
         (size_t)esz);
  return 1;
}

int32_t frt_vec_get(const frt_vec* v, int64_t i, void* out, int64_t esz) {
  if (i < 0 || i >= v->len) return 0;
  memcpy(out, (const char*)v->data + (size_t)i * (size_t)esz, (size_t)esz);
  return 1;
}

void frt_vec_insert(frt_vec* v, int64_t i, const void* e, int64_t esz) {
  if (i < 0 || i > v->len) {
    fprintf(stderr, "freshet: vec_insert index %lld out of range [0, %lld]\n",
            (long long)i, (long long)v->len);
    abort();
  }
  frt_vec_reserve(v, v->len + 1, esz);
  char* base = (char*)v->data;
  if (i < v->len)
    memmove(base + (size_t)(i + 1) * (size_t)esz,
            base + (size_t)i * (size_t)esz,
            (size_t)(v->len - i) * (size_t)esz);
  memcpy(base + (size_t)i * (size_t)esz, e, (size_t)esz);
  v->len += 1;
}

int32_t frt_vec_remove(frt_vec* v, int64_t i, void* out, int64_t esz) {
  if (i < 0 || i >= v->len) return 0;
  char* base = (char*)v->data;
  memcpy(out, base + (size_t)i * (size_t)esz, (size_t)esz);
  if (i + 1 < v->len)
    memmove(base + (size_t)i * (size_t)esz,
            base + (size_t)(i + 1) * (size_t)esz,
            (size_t)(v->len - i - 1) * (size_t)esz);
  v->len -= 1;
  return 1;
}

void frt_vec_truncate(frt_vec* v, int64_t n) {
  if (n < 0) n = 0;
  if (n < v->len) v->len = n;
}

void frt_vec_resize(frt_vec* v, int64_t n, const void* fill, int64_t esz) {
  if (n < 0) n = 0;
  if (n > v->len) {
    frt_vec_reserve(v, n, esz);
    char* base = (char*)v->data;
    for (int64_t i = v->len; i < n; ++i)
      memcpy(base + (size_t)i * (size_t)esz, fill, (size_t)esz);
  }
  v->len = n;
}

void frt_vec_clear(frt_vec* v) {
  v->len = 0;
}

void frt_vec_free(frt_vec* v) {
  if (v->data) {
    free(v->data);
    v->data = NULL;
  }
  v->len = 0;
  v->cap = 0;
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

/* fsht-03:03: the abort-capture is a posix facility; platforms without
 * signals (wasm) run tests without the capture (failures still report
 * through the harness summary) */
#if defined(FRT_PAL_SIGSETJMP)
void frt_test_setup(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = frt_test_abort_handler;
  sigaction(SIGABRT, &sa, NULL);
}
#else
void frt_test_setup(void) {}
#endif

#if !defined(FRT_PAL_SIGSETJMP)
#define sigsetjmp(j, s) setjmp(j)   /* best effort without saved mask */
#define sigaction(a, b, c) ((void)0)
#endif
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
/* fsht-03:03: argv capture is a platform facility — linux reads
 * /proc/self/cmdline (NUL-separated); other libc platforms use the
 * argv the C runtime received; baremetal has none (empty).  The frt
 * side never depends on which one ran. */
static void frt_args_lazy_init(void) {
#if defined(FRT_PAL_PROC_ARGS)
  FILE* f = fopen("/proc/self/cmdline", "rb");
  if (f) {
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n > 0) {
      buf[n] = '\0';
      /* count NUL-separated words */
      int argc = 0;
      for (size_t i = 0; i < n; ++i)
        if (buf[i] == '\0') ++argc;
      if (buf[n - 1] != '\0') ++argc;
      char** argv = (char**)calloc((size_t)argc + 1, sizeof(char*));
      if (argv) {
        int ai = 0;
        argv[ai++] = buf;
        for (size_t i = 0; i + 1 < n; ++i)
          if (buf[i] == '\0') argv[ai++] = buf + i + 1;
        argv[ai] = NULL;
        frt_main_argv = argv;
        frt_main_argc = argc;
      }
    }
  }
#endif
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
  const char* val = frt_getenv(buf);
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
  return frt_pal_fsize(buf);   /* fsht-03:03: platform files */
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
  void* f = frt_pal_fopen_read(buf);
  if (!f) return v;
  char* data = (char*)malloc((size_t)sz + 1);
  if (!data) { frt_pal_fclose(f); return v; }
  size_t got = (size_t)frt_pal_fread(f, data, (uint64_t)sz);
  frt_pal_fclose(f);
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
  void* f = frt_pal_fopen_write(buf);
  if (!f) return -1;
  size_t wrote = 0;
  if (data.len > 0) wrote = (size_t)frt_pal_fwrite(f, data.p, (uint64_t)data.len);
  int bad = (wrote != (size_t)data.len) ? 1 : 0;
  frt_pal_fclose(f);
  return bad ? -1 : (int64_t)wrote;
}

/* ------------- fsht-03:01 (registry 05.05.01, ch. 23 §4): streaming I/O ---
 * Ownership-tracked handles over a small stream table.  Reader/Writer
 * carry { handle: i64 } where the value indexes g_streams and -1 means
 * closed; drops are idempotent (close flips the handle to -1 first).
 * Readers and writers buffer (MUST, ch. 23 §4): 64 KiB default block
 * (SHOULD-level).  Error codes: 0 ok, 1 NotFound, 2 Permission, 3 Eof,
 * 4 Busy, 5+ Other(errno).  Runtime results are the raw i64 codes; the
 * Freshet `io` module maps them onto Result<_, IoError>.
 */
#define FRT_IO_BUF (64 * 1024)

typedef struct {
  void* f;    /* fsht-03:03: a platform file stream */
  char* buf;
  int64_t len;    /* buffered bytes */
  int64_t pos;    /* consumed */
  int is_stdin;
  int is_writer;
} frt_iostream_t;

static frt_iostream_t g_iostreams[32];
static int g_niostreams = 0;

static frt_iostream_t* io_at(int64_t handle) {
  if (handle < 0 || handle >= g_niostreams) return NULL;
  return &g_iostreams[handle];
}

/* (fsht-03:03 platform split: the historical errno→IoError mapping lived
 * here in the common core — POSIX logic outside the platform layers, and
 * dead once frt_reader/writer_open switched to frt_pal_ferror_code().  The
 * mapping now lives where it belongs, in each platform's ferror_code.) */

static int io_alloc_stream(FILE* f, int is_stdin, int is_writer) {
  if (g_niostreams >= (int)(sizeof g_iostreams / sizeof g_iostreams[0]))
    return -1;
  frt_iostream_t* s = &g_iostreams[g_niostreams];
  memset(s, 0, sizeof *s);
  s->f = f;
  s->is_stdin = is_stdin;
  s->is_writer = is_writer;
  return g_niostreams++;
}

/* handle struct layout: { i64 handle } — matches the compiler's prelude
 * Reader/Writer structs (ch. 23 §4). */
typedef struct { int64_t handle; } frt_handle_t;

int64_t frt_reader_open(frt_str_view_t path, frt_handle_t* out) {
  out->handle = -1;
  char* cpath = frt_str_to_cstr(path.p, path.len);
  void* f = frt_pal_fopen_read(cpath);   /* fsht-03:03: platform files */
  free(cpath);
  if (!f) return -1 * (int64_t)(frt_pal_ferror_code() ? frt_pal_ferror_code() : 1);
  int h = io_alloc_stream(f, 0, 0);
  if (h < 0) { frt_pal_fclose(f); return 4; }
  out->handle = h;
  return 0;
}

void frt_reader_stdin(frt_handle_t* out) {
  out->handle = -1;
  /* the platform's stdin; targets without one yield a closed reader
   * whose first read returns Eof (ch. 23 §4) */
  void* in = frt_pal_stdin();
  if (!in) return;
  int h = io_alloc_stream(in, 1, 0);
  if (h >= 0) out->handle = h;
}

/* at most ONE underlying block fill per call (ch. 23 §4 MUST); returns
 * the bytes read into buf (0 = end of file), or -errcode */
int64_t frt_reader_read(frt_handle_t* r, void* buf, int64_t n) {
  frt_iostream_t* s = io_at(r->handle);
  if (!s || !s->f) return -5;   /* closed → Other */
  int64_t done = 0;
  if (n <= 0) return 0;
  if (s->pos >= s->len) {
    /* one fill per call from the internal buffer */
    if (!s->buf) {
      s->buf = (char*)malloc(FRT_IO_BUF);
      if (!s->buf) return -5;
    }
    s->len = frt_pal_fread(s->f, s->buf, FRT_IO_BUF);
    s->pos = 0;
  }
  int64_t avail = s->len - s->pos;
  int64_t take = n < avail ? n : avail;
  if (take > 0) {
    memcpy(buf, s->buf + s->pos, (size_t)take);
    s->pos += take;
    done = take;
  }
  return done;
}

/* reads the next line (without its terminator) into `out`; end of file
 * with NO partial line is Eof (3).  0 ok / errcode otherwise. */
int64_t frt_reader_read_line(frt_handle_t* r, frt_string* out) {
  frt_iostream_t* s = io_at(r->handle);
  if (!s || !s->f) return -5;
  frt_string_init(out);
  int64_t partial = 0;
  for (;;) {
    /* pull through the buffered reader one block at a time */
    if (s->pos >= s->len) {
      if (!s->buf) {
        s->buf = (char*)malloc(FRT_IO_BUF);
        if (!s->buf) return 5;
      }
      s->len = frt_pal_fread(s->f, s->buf, FRT_IO_BUF);
      s->pos = 0;
      if (s->len == 0) {
        if (!partial) return 3;   /* clean end of file, no line */
        return 0;                  /* last line without terminator */
      }
    }
    while (s->pos < s->len) {
      char c = s->buf[s->pos++];
      if (c == '\n') return 0;
      if (c != '\r') {
        frt_string_append(out, &c, 1);
        ++partial;
      }
    }
  }
}

int64_t frt_writer_open(frt_str_view_t path, frt_handle_t* out) {
  out->handle = -1;
  char* cpath = frt_str_to_cstr(path.p, path.len);
  void* f = frt_pal_fopen_write(cpath);  /* create or truncate (§4) */
  free(cpath);
  if (!f) return -1 * (int64_t)(frt_pal_ferror_code() ? frt_pal_ferror_code() : 1);
  int h = io_alloc_stream(f, 0, 1);
  if (h < 0) { frt_pal_fclose(f); return 4; }
  out->handle = h;
  return 0;
}

/* appends to the internal buffer; flushes when the buffer fills, at an
 * explicit flush, or at close.  Ok(count) reports the bytes handled. */
int64_t frt_writer_write(frt_handle_t* w, const void* data, int64_t n) {
  frt_iostream_t* s = io_at(w->handle);
  if (!s || !s->f) return -5;
  if (!n) return 0;
  if (!s->buf) {
    s->buf = (char*)malloc(FRT_IO_BUF);
    if (!s->buf) return -5;
  }
  const char* p = (const char*)data;
  int64_t done = 0;
  while (n > 0) {
    int64_t room = FRT_IO_BUF - (int64_t)(s->len);
    int64_t take = n < room ? n : room;
    if (take > 0) {
      memcpy(s->buf + s->len, p, (size_t)take);
      s->len += take;
      p += take;
      n -= take;
      done += take;
    }
    if (s->len == FRT_IO_BUF) {
      int64_t wr = frt_pal_fwrite(s->f, s->buf, (uint64_t)s->len);
      s->len = 0;
      if (wr <= 0) return -5;
      if (wr < take) return done;  /* short write: report handled */
    }
  }
  return done;
}

int64_t frt_writer_flush(frt_handle_t* w) {
  frt_iostream_t* s = io_at(w->handle);
  if (!s || !s->f) return -5;
  if (s->len > 0) {
    if (frt_pal_fwrite(s->f, s->buf, (uint64_t)s->len) != s->len)
      return -5;
    s->len = 0;
  }
  if (frt_pal_fflush(s->f) != 0) return -5;
  return 0;
}

/* scope-exit / pre-emptive close — idempotent (handle flips to -1) */
void frt_reader_close(frt_handle_t* r) {
  frt_iostream_t* s = io_at(r->handle);
  r->handle = -1;
  if (!s) return;
  if (s->f && !s->is_stdin) frt_pal_fclose(s->f);
  free(s->buf);
  memset(s, 0, sizeof *s);
}

void frt_writer_close(frt_handle_t* w) {
  frt_iostream_t* s = io_at(w->handle);
  w->handle = -1;
  if (!s) return;
  if (s->f) {
    if (s->len > 0)
      frt_pal_fwrite(s->f, s->buf, (uint64_t)s->len);   /* flush at close */
    frt_pal_fclose(s->f);
  }
  free(s->buf);
  memset(s, 0, sizeof *s);
}

/* 1 = deleted, 0 = not found, neg = Other */
int64_t frt_remove(frt_str_view_t path) {
  char* cpath = frt_str_to_cstr(path.p, path.len);
  int rc = frt_pal_remove(cpath);
  free(cpath);
  if (rc == 0) return 1;
  return -1 * (int64_t)(frt_pal_ferror_code() ? frt_pal_ferror_code() : 1);
}
