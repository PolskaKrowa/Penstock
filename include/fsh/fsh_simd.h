/* fsh_simd.h — portable SIMD abstraction (report §5.2).
 *
 * The historical runtime exposes `frt_clflush` and `frt_pause` as
 * target-specific intrinsics.  The fsh_simd layer adds portable
 * abstractions over the common SIMD operations HPC kernels need, with
 * a scalar fallback so the same source compiles on every target.
 *
 * Design: the fsh_vecN_T types are typedefs to the target's native SIMD
 * type when one is available (e.g. __m256d on x86_64 with AVX2, or
 * float32x4_t on ARM NEON).  When no native type is available, they
 * fall back to a struct of N elements and the operations lower to
 * scalar loops.  The compiler's vectoriser can still vectorise the
 * scalar fallback if it chooses to.
 *
 * NOTE: this header is intentionally minimal for the MVP.  A complete
 * SIMD abstraction (mask types, gather/scatter, horizontal ops, ISA
 * detection) is a larger project; see §2.7 + §5.2 of the report.
 */
#ifndef FSH_SIMD_H
#define FSH_SIMD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- vector types ----------------------------------------- */
/* Two-wide and four-wide vectors of the common HPC element types.
 * The user-facing API treats these as opaque; access goes through the
 * load/store/op functions below.  Aligned loads/stores assume the
 * pointer is aligned to the vector width (16B for f64x2, 32B for f64x4,
 * etc.).  Use the unaligned variants for arbitrary pointers. */

#if defined(__AVX2__)
  #include <immintrin.h>
  typedef __m256d fsh_f64x4;
  typedef __m128d fsh_f64x2;
  typedef __m256  fsh_f32x8;
  typedef __m128  fsh_f32x4;
  #define FSH_HAS_NATIVE_SIMD 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
  #include <arm_neon.h>
  typedef float32x4_t fsh_f32x4;
  typedef float32x2_t fsh_f32x2;
  typedef float64x2_t fsh_f64x2;
  /* ARM NEON has no native f64x4; fall back to struct. */
  typedef struct { double v[4]; } fsh_f64x4;
  #define FSH_HAS_NATIVE_SIMD 1
#else
  /* Scalar fallback. */
  typedef struct { double v[2]; } fsh_f64x2;
  typedef struct { double v[4]; } fsh_f64x4;
  typedef struct { float  v[4]; } fsh_f32x4;
  typedef struct { float  v[8]; } fsh_f32x8;
  #define FSH_HAS_NATIVE_SIMD 0
#endif

/* ---------------- load / store --------------------------------------- */
fsh_f64x4 fsh_load_f64x4(const double* p);
fsh_f64x4 fsh_loadu_f64x4(const double* p);
void      fsh_store_f64x4(double* p, fsh_f64x4 v);
void      fsh_storeu_f64x4(double* p, fsh_f64x4 v);

/* ---------------- arithmetic ------------------------------------------ */
fsh_f64x4 fsh_add_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_sub_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_mul_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_div_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_fma_f64x4(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c);

/* horizontal reduction */
double fsh_hsum_f64x4(fsh_f64x4 v);

/* ---------------- cache control -------------------------------------- */
/* Report §2.7: software prefetch and cache flush are advanced APIs with
 * target-specific implementations.  The default policy is "do not use
 * them" — they can make performance worse when applied without a
 * measured reason.  These wrappers expose them behind a portable name
 * so a kernel that genuinely needs them can compile across targets. */
void fsh_prefetch_r(const void* p);   /* prefetch for read */
void fsh_prefetch_w(const void* p);   /* prefetch for write */
void fsh_clflush(const void* p);       /* write-back + invalidate */

/* ---------------- low-level CPU hint --------------------------------- */
/* Cheap CPU-spin hint (maps to PAUSE on x86, yield elsewhere). */
void fsh_cpu_pause(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_SIMD_H
