/* libfsh_simd_x86.c — the x86 SIMD backend.
 *
 * The x86 backend for fsh_simd.h: ONE compilation unit compiled at the
 * BASELINE instruction set, containing per-ISA implementations carrying
 * their own __attribute__((target(...))), and dispatching through IFUNC
 * resolvers (glibc-style) that a CPUID probe (__builtin_cpu_supports)
 * resolves once at load time:
 *
 *   level 0  scalar  — element loops + libm fma() for the fused ops
 *   level 1  SSE2    — 128-bit ops (f64x2 / f32x4)
 *   level 2  AVX2+FMA— 256-bit ops (f64x4 / f32x8), vfmadd fused paths
 *   level 3  AVX-512 — 256-bit EVEX encodings for the widest f64 ops
 *                      (the ABI types stay 32 bytes; 512-bit-heavy
 *                      forms deliberately avoid the clock throttle)
 *
 * Correctness contract: the fused ops (fsh_fma_*) are single-rounding
 * fused multiply-add by DEFINITION — the scalar path uses libm fma(),
 * the vector paths use vfmadd, so every rung agrees bit-for-bit.  The
 * non-fused arithmetic is element-exact on every rung.  The scalar
 * level doubles as the reference implementation the conformance test
 * compares against (tests/libfsh_simd_test.c).
 *
 * Link contract: exactly one SIMD backend TU links into libfsh beside
 * libfsh.c (cmake picks this file on x86 hosts); libfsh.c is compiled
 * with -DFSH_LIBFSH_SIMD_EXTERNAL so its scalar fallback is not
 * double-defined.  On non-x86 hosts this TU is simply not compiled.
 */
#include "fsh/fsh_simd.h"
#include <string.h>
#include <math.h>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

#define AVX2_ATTR __attribute__((target("avx2,fma")))
#define AVX512_ATTR __attribute__((target("avx512f,avx512vl")))
#define SSE2_ATTR __attribute__((target("sse2")))

/* ================= scalar reference (level 0) ========================== */
static fsh_f64x2 ld_f64x2(const double* p) {
  fsh_f64x2 v; memcpy(v.v, p, sizeof v.v); return v;
}
static fsh_f64x4 ld_f64x4(const double* p) {
  fsh_f64x4 v; memcpy(v.v, p, sizeof v.v); return v;
}
static fsh_f32x4 ld_f32x4(const float* p) {
  fsh_f32x4 v; memcpy(v.v, p, sizeof v.v); return v;
}
static fsh_f32x8 ld_f32x8(const float* p) {
  fsh_f32x8 v; memcpy(v.v, p, sizeof v.v); return v;
}

#define SCALAR_BIN(NAME, TY, N, OP) \
  static TY NAME##_##s(TY a, TY b) { \
    TY r; for (int i = 0; i < N; ++i) r.v[i] = a.v[i] OP b.v[i]; return r; \
  }
SCALAR_BIN(add_f64x2, fsh_f64x2, 2, +)
SCALAR_BIN(sub_f64x2, fsh_f64x2, 2, -)
SCALAR_BIN(mul_f64x2, fsh_f64x2, 2, *)
SCALAR_BIN(div_f64x2, fsh_f64x2, 2, /)
SCALAR_BIN(add_f64x4, fsh_f64x4, 4, +)
SCALAR_BIN(sub_f64x4, fsh_f64x4, 4, -)
SCALAR_BIN(mul_f64x4, fsh_f64x4, 4, *)
SCALAR_BIN(div_f64x4, fsh_f64x4, 4, /)
SCALAR_BIN(add_f32x4, fsh_f32x4, 4, +)
SCALAR_BIN(mul_f32x4, fsh_f32x4, 4, *)
SCALAR_BIN(add_f32x8, fsh_f32x8, 8, +)
SCALAR_BIN(mul_f32x8, fsh_f32x8, 8, *)

static fsh_f64x2 fma_f64x2_s(fsh_f64x2 a, fsh_f64x2 b, fsh_f64x2 c) {
  fsh_f64x2 r;
  for (int i = 0; i < 2; ++i) r.v[i] = fma(a.v[i], b.v[i], c.v[i]);
  return r;
}
static fsh_f64x4 fma_f64x4_s(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c) {
  fsh_f64x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = fma(a.v[i], b.v[i], c.v[i]);
  return r;
}
static fsh_f32x4 fma_f32x4_s(fsh_f32x4 a, fsh_f32x4 b, fsh_f32x4 c) {
  fsh_f32x4 r;
  for (int i = 0; i < 4; ++i) r.v[i] = fmaf(a.v[i], b.v[i], c.v[i]);
  return r;
}
static fsh_f32x8 fma_f32x8_s(fsh_f32x8 a, fsh_f32x8 b, fsh_f32x8 c) {
  fsh_f32x8 r;
  for (int i = 0; i < 8; ++i) r.v[i] = fmaf(a.v[i], b.v[i], c.v[i]);
  return r;
}
static double hsum_f64x2_s(fsh_f64x2 v) { return v.v[0] + v.v[1]; }
static double hsum_f64x4_s(fsh_f64x4 v) {
  return (v.v[0] + v.v[1]) + (v.v[2] + v.v[3]);
}
static float hsum_f32x4_s(fsh_f32x4 v) {
  return (v.v[0] + v.v[1]) + (v.v[2] + v.v[3]);
}
static float hsum_f32x8_s(fsh_f32x8 v) {
  float lo = (v.v[0] + v.v[1]) + (v.v[2] + v.v[3]);
  float hi = (v.v[4] + v.v[5]) + (v.v[6] + v.v[7]);
  return lo + hi;
}

/* ================= SSE2 (level 1) ====================================== */
SSE2_ATTR static fsh_f64x2 add_f64x2_sse2(fsh_f64x2 a, fsh_f64x2 b) {
  __m128d vr = _mm_add_pd(_mm_loadu_pd(a.v), _mm_loadu_pd(b.v));
  fsh_f64x2 r; _mm_storeu_pd(r.v, vr); return r;
}
SSE2_ATTR static fsh_f64x2 sub_f64x2_sse2(fsh_f64x2 a, fsh_f64x2 b) {
  __m128d vr = _mm_sub_pd(_mm_loadu_pd(a.v), _mm_loadu_pd(b.v));
  fsh_f64x2 r; _mm_storeu_pd(r.v, vr); return r;
}
SSE2_ATTR static fsh_f64x2 mul_f64x2_sse2(fsh_f64x2 a, fsh_f64x2 b) {
  __m128d vr = _mm_mul_pd(_mm_loadu_pd(a.v), _mm_loadu_pd(b.v));
  fsh_f64x2 r; _mm_storeu_pd(r.v, vr); return r;
}
SSE2_ATTR static fsh_f64x2 div_f64x2_sse2(fsh_f64x2 a, fsh_f64x2 b) {
  __m128d vr = _mm_div_pd(_mm_loadu_pd(a.v), _mm_loadu_pd(b.v));
  fsh_f64x2 r; _mm_storeu_pd(r.v, vr); return r;
}
SSE2_ATTR static fsh_f32x4 add_f32x4_sse2(fsh_f32x4 a, fsh_f32x4 b) {
  __m128 vr = _mm_add_ps(_mm_loadu_ps(a.v), _mm_loadu_ps(b.v));
  fsh_f32x4 r; _mm_storeu_ps(r.v, vr); return r;
}
SSE2_ATTR static fsh_f32x4 mul_f32x4_sse2(fsh_f32x4 a, fsh_f32x4 b) {
  __m128 vr = _mm_mul_ps(_mm_loadu_ps(a.v), _mm_loadu_ps(b.v));
  fsh_f32x4 r; _mm_storeu_ps(r.v, vr); return r;
}

/* ================= AVX2 + FMA (level 2) ================================ */
AVX2_ATTR static fsh_f64x4 add_f64x4_avx2(fsh_f64x4 a, fsh_f64x4 b) {
  __m256d vr = _mm256_add_pd(_mm256_loadu_pd(a.v), _mm256_loadu_pd(b.v));
  fsh_f64x4 r; _mm256_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static fsh_f64x4 sub_f64x4_avx2(fsh_f64x4 a, fsh_f64x4 b) {
  __m256d vr = _mm256_sub_pd(_mm256_loadu_pd(a.v), _mm256_loadu_pd(b.v));
  fsh_f64x4 r; _mm256_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static fsh_f64x4 mul_f64x4_avx2(fsh_f64x4 a, fsh_f64x4 b) {
  __m256d vr = _mm256_mul_pd(_mm256_loadu_pd(a.v), _mm256_loadu_pd(b.v));
  fsh_f64x4 r; _mm256_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static fsh_f64x4 div_f64x4_avx2(fsh_f64x4 a, fsh_f64x4 b) {
  __m256d vr = _mm256_div_pd(_mm256_loadu_pd(a.v), _mm256_loadu_pd(b.v));
  fsh_f64x4 r; _mm256_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static fsh_f64x4 fma_f64x4_avx2(fsh_f64x4 a, fsh_f64x4 b,
                                           fsh_f64x4 c) {
  __m256d vr = _mm256_fmadd_pd(_mm256_loadu_pd(a.v), _mm256_loadu_pd(b.v),
                               _mm256_loadu_pd(c.v));
  fsh_f64x4 r; _mm256_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static double hsum_f64x4_avx2(fsh_f64x4 v) {
  __m256d x = _mm256_loadu_pd(v.v);
  __m128d s = _mm_add_pd(_mm256_castpd256_pd128(x),
                          _mm256_extractf128_pd(x, 1));
  s = _mm_add_pd(s, _mm_permute_pd(s, 1));
  return _mm_cvtsd_f64(s);
}
AVX2_ATTR static fsh_f64x2 fma_f64x2_avx2(fsh_f64x2 a, fsh_f64x2 b,
                                          fsh_f64x2 c) {
  __m128d vr = _mm_fmadd_pd(_mm_loadu_pd(a.v), _mm_loadu_pd(b.v),
                            _mm_loadu_pd(c.v));
  fsh_f64x2 r; _mm_storeu_pd(r.v, vr); return r;
}
AVX2_ATTR static fsh_f32x4 fma_f32x4_avx2(fsh_f32x4 a, fsh_f32x4 b,
                                           fsh_f32x4 c) {
  __m128 vr = _mm_fmadd_ps(_mm_loadu_ps(a.v), _mm_loadu_ps(b.v),
                           _mm_loadu_ps(c.v));
  fsh_f32x4 r; _mm_storeu_ps(r.v, vr); return r;
}
AVX2_ATTR static fsh_f32x8 add_f32x8_avx2(fsh_f32x8 a, fsh_f32x8 b) {
  __m256 vr = _mm256_add_ps(_mm256_loadu_ps(a.v), _mm256_loadu_ps(b.v));
  fsh_f32x8 r; _mm256_storeu_ps(r.v, vr); return r;
}
AVX2_ATTR static fsh_f32x8 mul_f32x8_avx2(fsh_f32x8 a, fsh_f32x8 b) {
  __m256 vr = _mm256_mul_ps(_mm256_loadu_ps(a.v), _mm256_loadu_ps(b.v));
  fsh_f32x8 r; _mm256_storeu_ps(r.v, vr); return r;
}
AVX2_ATTR static fsh_f32x8 fma_f32x8_avx2(fsh_f32x8 a, fsh_f32x8 b,
                                          fsh_f32x8 c) {
  __m256 vr = _mm256_fmadd_ps(_mm256_loadu_ps(a.v), _mm256_loadu_ps(b.v),
                              _mm256_loadu_ps(c.v));
  fsh_f32x8 r; _mm256_storeu_ps(r.v, vr); return r;
}
AVX2_ATTR static float hsum_f32x8_avx2(fsh_f32x8 v) {
  __m256 x = _mm256_loadu_ps(v.v);
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(x),
                        _mm256_extractf128_ps(x, 1));
  s = _mm_add_ps(s, _mm_movehdup_ps(s));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
  return _mm_cvtss_f32(s);
}

/* ================= AVX-512 (level 3, 256-bit EVEX forms) =============== */
AVX512_ATTR static fsh_f64x4 add_f64x4_evex(fsh_f64x4 a, fsh_f64x4 b) {
  __m512d va = _mm512_castpd256_pd512(_mm256_loadu_pd(a.v));
  __m512d vb = _mm512_castpd256_pd512(_mm256_loadu_pd(b.v));
  __m512d vr = _mm512_add_pd(va, vb);
  fsh_f64x4 r;
  _mm256_storeu_pd(r.v, _mm512_castpd512_pd256(vr));
  return r;
}
AVX512_ATTR static fsh_f64x4 fma_f64x4_evex(fsh_f64x4 a, fsh_f64x4 b,
                                             fsh_f64x4 c) {
  __m512d vr = _mm512_fmadd_pd(_mm512_castpd256_pd512(_mm256_loadu_pd(a.v)),
                               _mm512_castpd256_pd512(_mm256_loadu_pd(b.v)),
                               _mm512_castpd256_pd512(_mm256_loadu_pd(c.v)));
  fsh_f64x4 r;
  _mm256_storeu_pd(r.v, _mm512_castpd512_pd256(vr));
  return r;
}

/* ================= level probe + ifunc dispatch ======================== */
static int fsh_level(void) {
  if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vl"))
    return 3;
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"))
    return 2;
  if (__builtin_cpu_supports("sse2"))
    return 1;
  return 0;
}

int fsh_simd_level(void) { return fsh_level(); }

/* resolvers: ifunc resolves each public symbol once at load time */
/* ---- one resolver per public op ---------------------------------------- */

static fsh_f64x2 (*r_add_f64x2(void))(fsh_f64x2, fsh_f64x2) {
  return fsh_level() >= 1 ? add_f64x2_sse2 : add_f64x2_s;
}
static fsh_f64x2 (*r_sub_f64x2(void))(fsh_f64x2, fsh_f64x2) {
  return fsh_level() >= 1 ? sub_f64x2_sse2 : sub_f64x2_s;
}
static fsh_f64x2 (*r_mul_f64x2(void))(fsh_f64x2, fsh_f64x2) {
  return fsh_level() >= 1 ? mul_f64x2_sse2 : mul_f64x2_s;
}
static fsh_f64x2 (*r_div_f64x2(void))(fsh_f64x2, fsh_f64x2) {
  return fsh_level() >= 1 ? div_f64x2_sse2 : div_f64x2_s;
}
static fsh_f64x2 (*r_fma_f64x2(void))(fsh_f64x2, fsh_f64x2, fsh_f64x2) {
  (void)fma_f64x2_s; (void)fma_f64x2_avx2;
  return fsh_level() >= 2 ? fma_f64x2_avx2 : fma_f64x2_s;
}
static double (*r_hsum_f64x2(void))(fsh_f64x2) { return hsum_f64x2_s; }

static fsh_f64x4 (*r_add_f64x4(void))(fsh_f64x4, fsh_f64x4) {
  int L = fsh_level();
  return L >= 3 ? add_f64x4_evex : L >= 2 ? add_f64x4_avx2 : add_f64x4_s;
}
static fsh_f64x4 (*r_sub_f64x4(void))(fsh_f64x4, fsh_f64x4) {
  return fsh_level() >= 2 ? sub_f64x4_avx2 : sub_f64x4_s;
}
static fsh_f64x4 (*r_mul_f64x4(void))(fsh_f64x4, fsh_f64x4) {
  return fsh_level() >= 2 ? mul_f64x4_avx2 : mul_f64x4_s;
}
static fsh_f64x4 (*r_div_f64x4(void))(fsh_f64x4, fsh_f64x4) {
  return fsh_level() >= 2 ? div_f64x4_avx2 : div_f64x4_s;
}
static fsh_f64x4 (*r_fma_f64x4(void))(fsh_f64x4, fsh_f64x4, fsh_f64x4) {
  int L = fsh_level();
  return L >= 3 ? fma_f64x4_evex : L >= 2 ? fma_f64x4_avx2 : fma_f64x4_s;
}
static double (*r_hsum_f64x4(void))(fsh_f64x4) {
  return fsh_level() >= 2 ? hsum_f64x4_avx2 : hsum_f64x4_s;
}

static fsh_f32x4 (*r_add_f32x4(void))(fsh_f32x4, fsh_f32x4) {
  return fsh_level() >= 1 ? add_f32x4_sse2 : add_f32x4_s;
}
static fsh_f32x4 (*r_mul_f32x4(void))(fsh_f32x4, fsh_f32x4) {
  return fsh_level() >= 1 ? mul_f32x4_sse2 : mul_f32x4_s;
}
static fsh_f32x4 (*r_fma_f32x4(void))(fsh_f32x4, fsh_f32x4, fsh_f32x4) {
  return fsh_level() >= 2 ? fma_f32x4_avx2 : fma_f32x4_s;
}
static float (*r_hsum_f32x4(void))(fsh_f32x4) { return hsum_f32x4_s; }

static fsh_f32x8 (*r_add_f32x8(void))(fsh_f32x8, fsh_f32x8) {
  return fsh_level() >= 2 ? add_f32x8_avx2 : add_f32x8_s;
}
static fsh_f32x8 (*r_mul_f32x8(void))(fsh_f32x8, fsh_f32x8) {
  return fsh_level() >= 2 ? mul_f32x8_avx2 : mul_f32x8_s;
}
static fsh_f32x8 (*r_fma_f32x8(void))(fsh_f32x8, fsh_f32x8, fsh_f32x8) {
  return fsh_level() >= 2 ? fma_f32x8_avx2 : fma_f32x8_s;
}
static float (*r_hsum_f32x8(void))(fsh_f32x8) {
  return fsh_level() >= 2 ? hsum_f32x8_avx2 : hsum_f32x8_s;
}

/* ---- public symbols: ifunc-dispatched ---------------------------------- */
fsh_f64x2 fsh_add_f64x2(fsh_f64x2 a, fsh_f64x2 b)
    __attribute__((ifunc("r_add_f64x2")));
fsh_f64x2 fsh_sub_f64x2(fsh_f64x2 a, fsh_f64x2 b)
    __attribute__((ifunc("r_sub_f64x2")));
fsh_f64x2 fsh_mul_f64x2(fsh_f64x2 a, fsh_f64x2 b)
    __attribute__((ifunc("r_mul_f64x2")));
fsh_f64x2 fsh_div_f64x2(fsh_f64x2 a, fsh_f64x2 b)
    __attribute__((ifunc("r_div_f64x2")));
fsh_f64x2 fsh_fma_f64x2(fsh_f64x2 a, fsh_f64x2 b, fsh_f64x2 c)
    __attribute__((ifunc("r_fma_f64x2")));
double fsh_hsum_f64x2(fsh_f64x2 v) __attribute__((ifunc("r_hsum_f64x2")));

fsh_f64x4 fsh_add_f64x4(fsh_f64x4 a, fsh_f64x4 b)
    __attribute__((ifunc("r_add_f64x4")));
fsh_f64x4 fsh_sub_f64x4(fsh_f64x4 a, fsh_f64x4 b)
    __attribute__((ifunc("r_sub_f64x4")));
fsh_f64x4 fsh_mul_f64x4(fsh_f64x4 a, fsh_f64x4 b)
    __attribute__((ifunc("r_mul_f64x4")));
fsh_f64x4 fsh_div_f64x4(fsh_f64x4 a, fsh_f64x4 b)
    __attribute__((ifunc("r_div_f64x4")));
fsh_f64x4 fsh_fma_f64x4(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c)
    __attribute__((ifunc("r_fma_f64x4")));
double fsh_hsum_f64x4(fsh_f64x4 v) __attribute__((ifunc("r_hsum_f64x4")));

fsh_f32x4 fsh_add_f32x4(fsh_f32x4 a, fsh_f32x4 b)
    __attribute__((ifunc("r_add_f32x4")));
fsh_f32x4 fsh_mul_f32x4(fsh_f32x4 a, fsh_f32x4 b)
    __attribute__((ifunc("r_mul_f32x4")));
fsh_f32x4 fsh_fma_f32x4(fsh_f32x4 a, fsh_f32x4 b, fsh_f32x4 c)
    __attribute__((ifunc("r_fma_f32x4")));
float fsh_hsum_f32x4(fsh_f32x4 v) __attribute__((ifunc("r_hsum_f32x4")));

fsh_f32x8 fsh_add_f32x8(fsh_f32x8 a, fsh_f32x8 b)
    __attribute__((ifunc("r_add_f32x8")));
fsh_f32x8 fsh_mul_f32x8(fsh_f32x8 a, fsh_f32x8 b)
    __attribute__((ifunc("r_mul_f32x8")));
fsh_f32x8 fsh_fma_f32x8(fsh_f32x8 a, fsh_f32x8 b, fsh_f32x8 c)
    __attribute__((ifunc("r_fma_f32x8")));
float fsh_hsum_f32x8(fsh_f32x8 v) __attribute__((ifunc("r_hsum_f32x8")));

/* ---- loads/stores: memcpy-shaped on every rung (the compiler folds the
 * memcpy into the right width at the call site; the ABI types are plain
 * structs, so no cross-TU alignment hazard exists) ------------------------ */
fsh_f64x2 fsh_load_f64x2(const double* p) { return ld_f64x2(p); }
fsh_f64x2 fsh_loadu_f64x2(const double* p) { return ld_f64x2(p); }
void fsh_store_f64x2(double* p, fsh_f64x2 v) { memcpy(p, v.v, sizeof v.v); }
void fsh_storeu_f64x2(double* p, fsh_f64x2 v) { memcpy(p, v.v, sizeof v.v); }
fsh_f64x4 fsh_load_f64x4(const double* p) { return ld_f64x4(p); }
fsh_f64x4 fsh_loadu_f64x4(const double* p) { return ld_f64x4(p); }
void fsh_store_f64x4(double* p, fsh_f64x4 v) { memcpy(p, v.v, sizeof v.v); }
void fsh_storeu_f64x4(double* p, fsh_f64x4 v) { memcpy(p, v.v, sizeof v.v); }
fsh_f32x4 fsh_load_f32x4(const float* p) { return ld_f32x4(p); }
fsh_f32x4 fsh_loadu_f32x4(const float* p) { return ld_f32x4(p); }
void fsh_store_f32x4(float* p, fsh_f32x4 v) { memcpy(p, v.v, sizeof v.v); }
void fsh_storeu_f32x4(float* p, fsh_f32x4 v) { memcpy(p, v.v, sizeof v.v); }
fsh_f32x8 fsh_load_f32x8(const float* p) { return ld_f32x8(p); }
fsh_f32x8 fsh_loadu_f32x8(const float* p) { return ld_f32x8(p); }
void fsh_store_f32x8(float* p, fsh_f32x8 v) { memcpy(p, v.v, sizeof v.v); }
void fsh_storeu_f32x8(float* p, fsh_f32x8 v) { memcpy(p, v.v, sizeof v.v); }

#else /* !x86: this TU is not compiled for the target (cmake decides);
         the stub keeps the file self-contained for syntax checks */

int fsh_simd_level(void) { return -1; }

#endif /* __x86_64__ || __i386__ */
