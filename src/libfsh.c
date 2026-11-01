/* libfsh.c — implementation of the fsh_* public API.
 *
 * This is a thin wrapper over the existing frt_* runtime ABI in
 * runtime/frt_rt.c.  The public API boundary (include/fsh/.h) is
 * typed and ownership-aware; this file lowers each call to the
 * corresponding frt_* symbol so existing Freshet-compiled programs
 * keep working unchanged.
 *
 * Build: compile frt_rt.c + libfsh.c into one library (libfsh.a or
 * libfsh.so).  Freshet programs already link against the frt_* symbols;
 * adding libfsh gives them the typed API as well.
 *
 * Stage: MVP.  Many functions are forwarding stubs; the contract is
 * documented in the headers and the implementations are deliberately
 * minimal so they can be filled in incrementally.
 */
#include "fsh.h"
#include "fsh_core.h"
#include "fsh_parallel.h"
#include "fsh_math.h"
#include "fsh_simd.h"
#include "fsh_io.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>

/* ---------------- runtime ABI forward declarations -------------------- */
extern void* frt_malloc(int64_t size, int64_t align);
extern void  frt_free(void* p);
extern void  frt_memset(void* p, int64_t n, int64_t unused);
extern void  frt_memcpy(void* d, void* s, int64_t n);
extern void* frt_soa_new(int64_t n, int32_t nf, int64_t* sizes);
extern void  frt_soa_free(void* blk);
extern void  frt_clflush(void* p);
extern void  frt_pause(void);
extern int32_t __frt_env_threads;
extern void  frt_init(void);

/* ---------------- build info ----------------------------------------- */
const char* fsh_build_info(void) {
  return "libfsh " FSH_VERSION_STRING " (experimental)";
}

/* ---------------- error model --------------------------------------- */
static fsh_fail_mode g_fail_mode = FSH_FAIL_RETURN;
static fsh_err g_last_err = FSH_OK;

void fsh_set_alloc_fail_mode(fsh_fail_mode m) { g_fail_mode = m; }
const char* fsh_err_name(fsh_err e) {
  switch (e) {
    case FSH_OK:             return "ok";
    case FSH_ERR_NOMEM:      return "out of memory";
    case FSH_ERR_OVERFLOW:   return "size overflow";
    case FSH_ERR_INVALID:    return "invalid argument";
    case FSH_ERR_RANGE:      return "out of range";
    case FSH_ERR_UNSUPPORTED:return "unsupported";
  }
  return "?";
}

/* ---------------- memory --------------------------------------------- */
void* fsh_alloc(int64_t bytes, int64_t align) {
  if (bytes < 0 || align < 0) { g_last_err = FSH_ERR_INVALID; return NULL; }
  if (align < 16) align = 16;
  void* p = frt_malloc(bytes, align);
  if (!p) {
    g_last_err = FSH_ERR_NOMEM;
    if (g_fail_mode == FSH_FAIL_ABORT) {
      fprintf(stderr, "libfsh: abort on alloc failure (%lld bytes)\n",
              (long long)bytes);
      abort();
    }
  }
  return p;
}

void* fsh_calloc(int64_t count, int64_t elem_size) {
  int64_t total = fsh_safe_mul(count, elem_size);
  if (total == 0 && (count != 0 || elem_size != 0)) {
    g_last_err = FSH_ERR_OVERFLOW;
    if (g_fail_mode == FSH_FAIL_ABORT) abort();
    return NULL;
  }
  void* p = fsh_alloc(total, 16);
  if (p) frt_memset(p, total, 0);
  return p;
}

void fsh_free(void* p) { frt_free(p); }

void* fsh_realloc(void* p, int64_t new_bytes) {
  /* frt_malloc/free are posix_memalign-backed; realloc semantics require
   * a manual copy. */
  if (!p) return fsh_alloc(new_bytes, 16);
  if (new_bytes == 0) { fsh_free(p); return NULL; }
  void* q = fsh_alloc(new_bytes, 16);
  if (!q) return NULL;
  /* We do not know the old size; copy up to new_bytes safely.  This is
   * a known limitation — the caller should keep the old size if it
   * cares.  In practice Freshet-side wrappers will pass the old size
   * to a future fsh_realloc_exact. */
  memcpy(q, p, (size_t)new_bytes);
  fsh_free(p);
  return q;
}

int64_t fsh_safe_mul(int64_t a, int64_t b) {
  if (a == 0 || b == 0) return 0;
  if (a > 0) {
    if (b > 0 && a > INT64_MAX / b) { g_last_err = FSH_ERR_OVERFLOW; return 0; }
    if (b < 0 && b < INT64_MIN / a) { g_last_err = FSH_ERR_OVERFLOW; return 0; }
  } else {
    if (b > 0 && a < INT64_MIN / b) { g_last_err = FSH_ERR_OVERFLOW; return 0; }
    if (b < 0 && a < INT64_MAX / b) { g_last_err = FSH_ERR_OVERFLOW; return 0; }
  }
  return a * b;
}

/* ---------------- spans ---------------------------------------------- */
fsh_span fsh_span_make(void* data, int64_t len) {
  fsh_span s = { data, len, 0 };
  return s;
}
fsh_span fsh_span_make_strided(void* data, int64_t len, int64_t stride) {
  fsh_span s = { data, len, stride };
  return s;
}
void* fsh_span_at(const fsh_span* s, int64_t index) {
  if (!s || index < 0 || index >= s->len) { g_last_err = FSH_ERR_RANGE; return NULL; }
  if (s->stride == 0) {
    return (char*)s->data + index * 1;   /* unknown elem size in C; caller knows */
  }
  return (char*)s->data + index * s->stride;
}

/* ---------------- SoA ------------------------------------------------- */
void* fsh_soa_alloc(int64_t n, int32_t nf, const int64_t* sizes) {
  if (n < 0 || nf <= 0 || !sizes) { g_last_err = FSH_ERR_INVALID; return NULL; }
  return frt_soa_new(n, nf, (int64_t*)sizes);
}
void fsh_soa_free(void* blk) { frt_soa_free(blk); }
void* fsh_soa_plane(void* blk, int32_t i) {
  if (!blk || i < 0) return NULL;
  int32_t nf = *(int32_t*)blk;
  if (i >= nf) { g_last_err = FSH_ERR_RANGE; return NULL; }
  return *(void**)((char*)blk + 16 + 8 * (size_t)i);
}
int64_t fsh_soa_len(void* blk) {
  if (!blk) return 0;
  return *(int64_t*)((char*)blk + 8);
}
int64_t fsh_alloc_min_align(void) { return 16; }
int64_t fsh_soa_plane_align(void) { return 64; }

/* ---------------- parallel for -------------------------------------- */
/* Forwards to frt_parfor via a small adaptor that constructs the
 * frt_range_t struct the runtime expects.  The body fn signature is
 * identical so no closure is needed. */
typedef struct {
  void (*body)(int64_t lo, int64_t hi, void* ctx);
  void* ctx;
} fsh_body_call;
typedef struct frt_range_s frt_range_t_;  /* forward */

/* The actual frt_range_t struct lives in runtime/frt_rt.c; the compiler
 * knows its layout.  We mirror it here so libfsh can construct one
 * without depending on the compiler-generated header. */
#pragma pack(push, 8)
typedef struct {
  void (*body)(int64_t, int64_t, void*);
  int64_t lo, hi, chunk;
  int32_t nthreads, affinity, ctx_size;
  int32_t repro;            /* @reproducible flag (Phase 5, offset 44) */
  int64_t tag;
  void* ctx_template;
} fsh_frt_range;
#pragma pack(pop)

extern int32_t frt_parfor(fsh_frt_range* r, void** ctxout, int32_t maxctx);

int32_t fsh_parallel_for(const fsh_range* r, fsh_body_fn body, void* ctx) {
  if (!r || !body) return 0;
  fsh_frt_range rr;
  rr.body = body;
  rr.lo = r->lo; rr.hi = r->hi;
  rr.chunk = r->chunk;
  rr.nthreads = r->threads;
  rr.affinity = (r->sched == FSH_SCHED_SPREAD) ? 1 : 0;
  rr.repro = 0;
  rr.ctx_size = 64;   /* must match IrGen's MAXCTX-1 padding */
  rr.tag = 0;
  rr.ctx_template = ctx;
  void* ctxout[64];
  return frt_parfor(&rr, ctxout, 64);
}

/* ---------------- reductions ---------------------------------------- */
/* chunk body closure for the parallel fast path: each worker computes a
 * partial over its [lo,hi) block into its ctx slot */
typedef struct {
  fsh_red_op op;
  double (*body)(int64_t i, void* ctx);
  void* user_ctx;
  double acc;
} fsh_red_ctx;

static void fsh_reduce_chunk(int64_t lo, int64_t hi, void* vctx) {
  fsh_red_ctx* c = (fsh_red_ctx*)vctx;
  for (int64_t i = lo; i < hi; ++i) {
    double v = c->body(i, c->user_ctx);
    switch (c->op) {
      case FSH_RED_SUM:      c->acc += v; break;
      case FSH_RED_PRODUCT:  c->acc *= v; break;
      case FSH_RED_MIN:      if (i == lo || v < c->acc) c->acc = v; break;
      case FSH_RED_MAX:      if (i == lo || v > c->acc) c->acc = v; break;
    }
  }
}

double fsh_reduce(int64_t lo, int64_t hi, fsh_red_op op, fsh_red_mode mode,
                  double (*body)(int64_t i, void* ctx), void* ctx) {
  if (!body || hi <= lo) return 0.0;
  /* deterministic mode: exact sequential order — bitwise-reproducible
   * by construction (documented contract: "deterministic, not parallel") */
  if (mode == FSH_RED_DETERMINISTIC) {
    double acc = (op == FSH_RED_PRODUCT) ? 1.0 : 0.0;
    for (int64_t i = lo; i < hi; ++i) {
      double v = body(i, ctx);
      switch (op) {
        case FSH_RED_SUM:      acc += v; break;
        case FSH_RED_PRODUCT:  acc *= v; break;
        case FSH_RED_MIN:      if (i == lo || v < acc) acc = v; break;
        case FSH_RED_MAX:      if (i == lo || v > acc) acc = v; break;
      }
    }
    return acc;
  }
  /* fast mode: REAL parallel partials via the runtime's parfor pool,
   * combined in chunk-index order (report §9: the MVP ran both modes
   * sequentially — fast now actually uses the workers) */
  fsh_red_ctx tmpl;
  tmpl.op = op;
  tmpl.body = body;
  tmpl.user_ctx = ctx;
  tmpl.acc = (op == FSH_RED_PRODUCT) ? 1.0 :
             (op == FSH_RED_MIN) ? INFINITY :
             (op == FSH_RED_MAX) ? -INFINITY : 0.0;
  fsh_frt_range rr;
  rr.body = fsh_reduce_chunk;
  rr.lo = lo; rr.hi = hi;
  rr.chunk = 0;
  rr.nthreads = 0;
  rr.affinity = 0;
  rr.repro = 0;
  rr.ctx_size = sizeof(fsh_red_ctx);
  rr.tag = 0;
  rr.ctx_template = &tmpl;
  void* ctxout[64];
  int32_t n = frt_parfor((fsh_frt_range*)&rr, ctxout, 64);
  double acc = tmpl.acc;
  for (int32_t c = 0; c < n && c < 64; ++c) {
    fsh_red_ctx* part = (fsh_red_ctx*)ctxout[c];
    if (!part) continue;
    switch (op) {
      case FSH_RED_SUM:      acc += part->acc; break;
      case FSH_RED_PRODUCT:  acc *= part->acc; break;
      case FSH_RED_MIN:      if (part->acc < acc) acc = part->acc; break;
      case FSH_RED_MAX:      if (part->acc > acc) acc = part->acc; break;
    }
    frt_free(part);
  }
  return acc;
}

/* ---------------- errno surface (report §9) --------------------------- */
/* The guide previously listed fsh_last_errno() as PLANNED.  Contract:
 * call it IMMEDIATELY after an fsh call that failed — it returns the
 * thread's errno snapshot and resets it, mirroring the Win32
 * GetLastError discipline (anything else the thread calls in between
 * may overwrite errno). */
int32_t fsh_last_errno(void) {
  int32_t e = (int32_t)errno;
  errno = 0;
  return e;
}

/* ---------------- worker introspection ------------------------------ */
int32_t fsh_thread_count(void) { return __frt_env_threads; }
/* report §9: the MVP returned 0 — ids are now assigned per thread from
 * an atomic counter on first use (main is typically 1) */
static _Atomic int32_t fsh_next_tid = 1;
static __thread int32_t fsh_my_tid = 0;
int32_t fsh_thread_id(void) {
  if (fsh_my_tid == 0)
    fsh_my_tid = atomic_fetch_add(&fsh_next_tid, 1);
  return fsh_my_tid;
}

/* ---------------- barriers ----------------------------------------- */
void fsh_barrier_init(fsh_barrier* b, int32_t n) {
  b->n = n;
  atomic_init(&b->count, 0);
  atomic_init(&b->generation, 0);
}
void fsh_barrier_wait(fsh_barrier* b) {
  int32_t gen = atomic_load(&b->generation);
  int32_t arrived = atomic_fetch_add(&b->count, 1) + 1;
  if (arrived == b->n) {
    atomic_store(&b->count, 0);
    atomic_store(&b->generation, gen + 1);
  } else {
    while (atomic_load(&b->generation) == gen) {
      /* spin */
    }
  }
}

/* ---------------- atomics ----------------------------------------- */
int64_t fsh_atomic_load_i64(const _Atomic int64_t* p, fsh_memory_order mo) {
  switch (mo) {
    case FSH_MO_RELAXED: return atomic_load_explicit(p, memory_order_relaxed);
    case FSH_MO_ACQUIRE: return atomic_load_explicit(p, memory_order_acquire);
    case FSH_MO_SEQ_CST: return atomic_load_explicit(p, memory_order_seq_cst);
    default:             return atomic_load_explicit(p, memory_order_seq_cst);
  }
}
void fsh_atomic_store_i64(_Atomic int64_t* p, int64_t v, fsh_memory_order mo) {
  switch (mo) {
    case FSH_MO_RELAXED: atomic_store_explicit(p, v, memory_order_relaxed); break;
    case FSH_MO_RELEASE: atomic_store_explicit(p, v, memory_order_release); break;
    case FSH_MO_SEQ_CST: atomic_store_explicit(p, v, memory_order_seq_cst); break;
    default:             atomic_store_explicit(p, v, memory_order_seq_cst); break;
  }
}
int64_t fsh_atomic_fetch_add_i64(_Atomic int64_t* p, int64_t v, fsh_memory_order mo) {
  switch (mo) {
    case FSH_MO_RELAXED: return atomic_fetch_add_explicit(p, v, memory_order_relaxed);
    case FSH_MO_ACQ_REL: return atomic_fetch_add_explicit(p, v, memory_order_acq_rel);
    case FSH_MO_SEQ_CST: return atomic_fetch_add_explicit(p, v, memory_order_seq_cst);
    default:             return atomic_fetch_add_explicit(p, v, memory_order_seq_cst);
  }
}

void fsh_yield_hint(void) { frt_pause(); }

/* ---------------- math --------------------------------------------- */
/* Accurate variants forward to libm / LLVM intrinsics. */
double fsh_math_sin_f64(double x)  { return sin(x); }
double fsh_math_cos_f64(double x)  { return cos(x); }
double fsh_math_tan_f64(double x)  { return tan(x); }
double fsh_math_log2_f64(double x) { return log2(x); }
double fsh_math_log10_f64(double x){ return log10(x); }
double fsh_math_log_f64(double x)  { return log(x); }
double fsh_math_exp_f64(double x)  { return exp(x); }
double fsh_math_pow_f64(double x, double y) { return pow(x, y); }
double fsh_math_sqrt_f64(double x) { return sqrt(x); }
double fsh_math_fabs_f64(double x) { return fabs(x); }
double fsh_math_floor_f64(double x){ return floor(x); }
double fsh_math_ceil_f64(double x) { return ceil(x); }
double fsh_math_round_f64(double x){ return round(x); }
double fsh_math_fmin_f64(double a, double b) { return fmin(a, b); }
double fsh_math_fmax_f64(double a, double b) { return fmax(a, b); }

float fsh_math_sin_f32(float x)  { return sinf(x); }
float fsh_math_cos_f32(float x)  { return cosf(x); }
float fsh_math_log2_f32(float x) { return log2f(x); }
float fsh_math_exp_f32(float x)  { return expf(x); }
float fsh_math_sqrt_f32(float x) { return sqrtf(x); }
float fsh_math_fabs_f32(float x) { return fabsf(x); }
float fsh_math_fmin_f32(float a, float b) { return fminf(a, b); }
float fsh_math_fmax_f32(float a, float b) { return fmaxf(a, b); }

/* Fast variants: in the MVP they're the same as accurate.  A future
 * version will dispatch to a vector backend when available. */
double fsh_math_sin_fast_f64(double x)  { return sin(x); }
double fsh_math_cos_fast_f64(double x)  { return cos(x); }
double fsh_math_log_fast_f64(double x)  { return log(x); }
double fsh_math_exp_fast_f64(double x)  { return exp(x); }

/* FMA: lower to libm's fma which the compiler will turn into @llvm.fma.*
 * when the target supports it. */
double fsh_math_fma_f64(double a, double b, double c) { return fma(a, b, c); }
float  fsh_math_fma_f32(float a, float b, float c)   { return fmaf(a, b, c); }

/* ---------------- RNG (counter-based, Philox-4x32-10-ish) ----------- */
/* Simple splitmix64-derived RNG.  Not cryptographically secure; the goal
 * is reproducibility across (seed, stream, counter) tuples.  A proper
 * Philox implementation should replace this once the runtime gets a
 * vetted counter-based generator (report §5.2). */
static uint64_t splitmix64(uint64_t* state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}
uint64_t fsh_random_u64(uint64_t seed, uint64_t stream, uint64_t counter) {
  uint64_t st = seed ^ (stream * 0x9E3779B97F4A7C15ULL)
                     ^ (counter * 0xBB67AE8584CAA73BULL);
  return splitmix64(&st);
}
double fsh_random_uniform_f64(uint64_t seed, uint64_t stream, uint64_t counter) {
  uint64_t u = fsh_random_u64(seed, stream, counter);
  /* 53-bit mantissa, [0, 1) */
  return (double)(u >> 11) * (1.0 / 9007199254740992.0);
}
int64_t fsh_random_range_i64(uint64_t seed, uint64_t stream, uint64_t counter,
                              int64_t lo, int64_t hi) {
  if (hi <= lo) return lo;
  uint64_t range = (uint64_t)(hi - lo);
  uint64_t u = fsh_random_u64(seed, stream, counter);
  return lo + (int64_t)(u % range);
}

/* ---------------- SIMD (scalar fallback) ---------------------------- */
#if !FSH_HAS_NATIVE_SIMD
fsh_f64x4 fsh_load_f64x4(const double* p) {
  fsh_f64x4 v; for (int i = 0; i < 4; ++i) v.v[i] = p[i]; return v;
}
fsh_f64x4 fsh_loadu_f64x4(const double* p) {
  fsh_f64x4 v; for (int i = 0; i < 4; ++i) v.v[i] = p[i]; return v;
}
void fsh_store_f64x4(double* p, fsh_f64x4 v) {
  for (int i = 0; i < 4; ++i) p[i] = v.v[i];
}
void fsh_storeu_f64x4(double* p, fsh_f64x4 v) {
  for (int i = 0; i < 4; ++i) p[i] = v.v[i];
}
fsh_f64x4 fsh_add_f64x4(fsh_f64x4 a, fsh_f64x4 b) {
  fsh_f64x4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] + b.v[i]; return r;
}
fsh_f64x4 fsh_sub_f64x4(fsh_f64x4 a, fsh_f64x4 b) {
  fsh_f64x4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] - b.v[i]; return r;
}
fsh_f64x4 fsh_mul_f64x4(fsh_f64x4 a, fsh_f64x4 b) {
  fsh_f64x4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i]; return r;
}
fsh_f64x4 fsh_div_f64x4(fsh_f64x4 a, fsh_f64x4 b) {
  fsh_f64x4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] / b.v[i]; return r;
}
fsh_f64x4 fsh_fma_f64x4(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c) {
  fsh_f64x4 r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] * b.v[i] + c.v[i];
  return r;
}
double fsh_hsum_f64x4(fsh_f64x4 v) {
  double s = 0; for (int i = 0; i < 4; ++i) s += v.v[i]; return s;
}
#else
/* Native SIMD implementations: filled in per-target in separate .c files
 * (libfsh_simd_x86.c, libfsh_simd_arm.c).  For the MVP the scalar
 * fallback above is the default; a future build can compile the
 * native backends and link them in. */
fsh_f64x4 fsh_load_f64x4(const double* p);
fsh_f64x4 fsh_loadu_f64x4(const double* p);
void fsh_store_f64x4(double* p, fsh_f64x4 v);
void fsh_storeu_f64x4(double* p, fsh_f64x4 v);
fsh_f64x4 fsh_add_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_sub_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_mul_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_div_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_fma_f64x4(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c);
double fsh_hsum_f64x4(fsh_f64x4 v);
#endif

void fsh_prefetch_r(const void* p) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_prefetch(p, 0, 3);
#else
  (void)p;
#endif
}
void fsh_prefetch_w(const void* p) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_prefetch(p, 1, 3);
#else
  (void)p;
#endif
}
void fsh_clflush(const void* p) { frt_clflush((void*)p); }
void fsh_cpu_pause(void) { frt_pause(); }

/* ---------------- IO ------------------------------------------------ */
void fsh_print_str(const char* s, int64_t len) {
  extern void frt_print_str(const char*, int64_t);
  frt_print_str(s, len);
}
void fsh_print_f64(double v, int64_t digits) {
  extern void frt_print_f64(double, int64_t);
  frt_print_f64(v, digits);
}
void fsh_print_i64(int64_t v) { extern void frt_print_i64(int64_t); frt_print_i64(v); }
void fsh_print_u64(uint64_t v) { extern void frt_print_u64(uint64_t); frt_print_u64(v); }
void fsh_println(void) { extern void frt_println(void); frt_println(); }

struct fsh_file {
  FILE* fp;
};

fsh_file* fsh_file_open(const char* path, const char* mode) {
  if (!path || !mode) return NULL;
  FILE* fp = fopen(path, mode);
  if (!fp) return NULL;
  fsh_file* f = (fsh_file*)fsh_alloc(sizeof(fsh_file), 16);
  if (!f) { fclose(fp); return NULL; }
  f->fp = fp;
  return f;
}
void fsh_file_close(fsh_file* f) {
  if (!f) return;
  if (f->fp) fclose(f->fp);
  fsh_free(f);
}

static void swap_bytes(void* p, size_t n) {
  char* b = (char*)p;
  for (size_t i = 0; i < n / 2; ++i) {
    char t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t;
  }
}

size_t fsh_file_read_f64(fsh_file* f, double* dst, size_t count, fsh_endian e) {
  if (!f || !f->fp || !dst) return 0;
  size_t got = fread(dst, sizeof(double), count, f->fp);
  if (e != FSH_ENDIAN_NATIVE) {
    int big_endian_host = (((union { int i; char c; }){ .i = 1 }).c == 0);
    if ((e == FSH_ENDIAN_LITTLE && big_endian_host) ||
        (e == FSH_ENDIAN_BIG && !big_endian_host)) {
      for (size_t i = 0; i < got; ++i) swap_bytes(&dst[i], sizeof(double));
    }
  }
  return got;
}
size_t fsh_file_write_f64(fsh_file* f, const double* src, size_t count, fsh_endian e) {
  if (!f || !f->fp || !src) return 0;
  /* report §9: the MVP ignored the requested byte order on write — the
   * conversion now matches the read path (swap into a staging buffer so
   * the caller's data stays untouched) */
  int big_endian_host = (((union { int i; char c; }){ .i = 1 }).c == 0);
  int need_swap = (e != FSH_ENDIAN_NATIVE) &&
                  ((e == FSH_ENDIAN_LITTLE && big_endian_host) ||
                   (e == FSH_ENDIAN_BIG && !big_endian_host));
  if (!need_swap) return fwrite(src, sizeof(double), count, f->fp);
  size_t written = 0;
  double buf[64];
  while (written < count) {
    size_t n = count - written;
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; ++i) {
      buf[i] = src[written + i];
      swap_bytes(&buf[i], sizeof(double));
    }
    size_t w = fwrite(buf, sizeof(double), n, f->fp);
    written += w;
    if (w != n) break;
  }
  return written;
}
size_t fsh_file_read_i32(fsh_file* f, int32_t* dst, size_t count, fsh_endian e) {
  if (!f || !f->fp || !dst) return 0;
  size_t got = fread(dst, sizeof(int32_t), count, f->fp);
  if (e != FSH_ENDIAN_NATIVE) {
    int big_endian_host = (((union { int i; char c; }){ .i = 1 }).c == 0);
    if ((e == FSH_ENDIAN_LITTLE && big_endian_host) ||
        (e == FSH_ENDIAN_BIG && !big_endian_host)) {
      for (size_t i = 0; i < got; ++i) swap_bytes(&dst[i], sizeof(int32_t));
    }
  }
  return got;
}
size_t fsh_file_write_i32(fsh_file* f, const int32_t* src, size_t count, fsh_endian e) {
  if (!f || !f->fp || !src) return 0;
  /* report §9: honour the requested byte order (see fsh_file_write_f64) */
  int big_endian_host = (((union { int i; char c; }){ .i = 1 }).c == 0);
  int need_swap = (e != FSH_ENDIAN_NATIVE) &&
                  ((e == FSH_ENDIAN_LITTLE && big_endian_host) ||
                   (e == FSH_ENDIAN_BIG && !big_endian_host));
  if (!need_swap) return fwrite(src, sizeof(int32_t), count, f->fp);
  size_t written = 0;
  int32_t buf[64];
  while (written < count) {
    size_t n = count - written;
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; ++i) {
      buf[i] = src[written + i];
      swap_bytes(&buf[i], sizeof(int32_t));
    }
    size_t w = fwrite(buf, sizeof(int32_t), n, f->fp);
    written += w;
    if (w != n) break;
  }
  return written;
}

int fsh_file_write_header(fsh_file* f, const fsh_dataset_header* h) {
  if (!f || !f->fp || !h) return -1;
  if (fwrite("FSH1", 1, 4, f->fp) != 4) return -1;
  if (fwrite(h, sizeof(fsh_dataset_header), 1, f->fp) != 1) return -1;
  return 0;
}
int fsh_file_read_header(fsh_file* f, fsh_dataset_header* h) {
  if (!f || !f->fp || !h) return -1;
  char magic[4];
  if (fread(magic, 1, 4, f->fp) != 4) return -1;
  if (memcmp(magic, "FSH1", 4) != 0) return -1;
  if (fread(h, sizeof(fsh_dataset_header), 1, f->fp) != 1) return -1;
  return 0;
}
