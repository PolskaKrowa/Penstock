/* fsh_simd.h — portable SIMD abstraction (report §5.2).
 *
 * The historical runtime exposes `frt_clflush` and `frt_pause` as
 * target-specific intrinsics.  The fsh_simd layer adds portable
 * abstractions over the common SIMD operations HPC kernels need, with
 * a scalar fallback so the same source compiles on every target.
 *
 * the native
 * backends (libfsh_simd_x86.c: SSE2 baseline + AVX2 + AVX-512 with
 * per-function target attributes and constructor-time dispatch;
 * libfsh_simd_arm.c: NEON) are IMPLEMENTED.  The vector TYPES are now
 * UNCONDITIONALLY plain structs — the ABI is identical whether a
 * translation unit is compiled with -mavx2 or not (the historical
 * native-typedef switched the type between builds, which would have
 * broken linking between differently-flagged TUs).  The backends load
 * these structs into the target's vector registers and store back.
 *
 * The scalar fallback stays the default when no backend is linked
 * (FSH_NO_SIMD_BACKEND compiles libfsh_simd_x86.c's scalar paths only);
 * the compiler's auto-vectoriser can still vectorise the fallback.
 */
#ifndef FSH_SIMD_H
#define FSH_SIMD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- vector types ----------------------------------------- */
/* Two-wide, four-wide and eight-wide vectors of the common HPC element
 * types.  The user-facing API treats these as opaque; access goes
 * through the load/store/op functions below.  Aligned loads/stores
 * assume the pointer is aligned to the vector width (16B for f64x2,
 * 32B for f64x4/f32x8, 16B for f32x4); use the unaligned variants for
 * arbitrary pointers.
 *
 * ABI stability note: these are plain struct typedefs on EVERY target
 * The size/element order never depends on compiler flags. */
typedef struct { double v[2]; } fsh_f64x2;
typedef struct { double v[4]; } fsh_f64x4;
typedef struct { float  v[4]; } fsh_f32x4;
typedef struct { float  v[8]; } fsh_f32x8;

/* ---------------- load / store --------------------------------------- */
fsh_f64x2 fsh_load_f64x2(const double* p);
fsh_f64x2 fsh_loadu_f64x2(const double* p);
void      fsh_store_f64x2(double* p, fsh_f64x2 v);
void      fsh_storeu_f64x2(double* p, fsh_f64x2 v);
fsh_f64x4 fsh_load_f64x4(const double* p);
fsh_f64x4 fsh_loadu_f64x4(const double* p);
void      fsh_store_f64x4(double* p, fsh_f64x4 v);
void      fsh_storeu_f64x4(double* p, fsh_f64x4 v);
fsh_f32x4 fsh_load_f32x4(const float* p);
fsh_f32x4 fsh_loadu_f32x4(const float* p);
void      fsh_store_f32x4(float* p, fsh_f32x4 v);
void      fsh_storeu_f32x4(float* p, fsh_f32x4 v);
fsh_f32x8 fsh_load_f32x8(const float* p);
fsh_f32x8 fsh_loadu_f32x8(const float* p);
void      fsh_store_f32x8(float* p, fsh_f32x8 v);
void      fsh_storeu_f32x8(float* p, fsh_f32x8 v);

/* ---------------- arithmetic ------------------------------------------ */
fsh_f64x2 fsh_add_f64x2(fsh_f64x2 a, fsh_f64x2 b);
fsh_f64x2 fsh_sub_f64x2(fsh_f64x2 a, fsh_f64x2 b);
fsh_f64x2 fsh_mul_f64x2(fsh_f64x2 a, fsh_f64x2 b);
fsh_f64x2 fsh_div_f64x2(fsh_f64x2 a, fsh_f64x2 b);
fsh_f64x2 fsh_fma_f64x2(fsh_f64x2 a, fsh_f64x2 b, fsh_f64x2 c);
fsh_f64x4 fsh_add_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_sub_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_mul_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_div_f64x4(fsh_f64x4 a, fsh_f64x4 b);
fsh_f64x4 fsh_fma_f64x4(fsh_f64x4 a, fsh_f64x4 b, fsh_f64x4 c);
fsh_f32x4 fsh_add_f32x4(fsh_f32x4 a, fsh_f32x4 b);
fsh_f32x4 fsh_mul_f32x4(fsh_f32x4 a, fsh_f32x4 b);
fsh_f32x4 fsh_fma_f32x4(fsh_f32x4 a, fsh_f32x4 b, fsh_f32x4 c);
fsh_f32x8 fsh_add_f32x8(fsh_f32x8 a, fsh_f32x8 b);
fsh_f32x8 fsh_mul_f32x8(fsh_f32x8 a, fsh_f32x8 b);
fsh_f32x8 fsh_fma_f32x8(fsh_f32x8 a, fsh_f32x8 b, fsh_f32x8 c);

/* horizontal reductions */
double fsh_hsum_f64x2(fsh_f64x2 v);
double fsh_hsum_f64x4(fsh_f64x4 v);
float  fsh_hsum_f32x4(fsh_f32x4 v);
float  fsh_hsum_f32x8(fsh_f32x8 v);

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

/* ---------------- dispatch introspection -------------------- */
/* the ladder the x86 backend resolved at process start:
 * 0 = scalar, 1 = SSE2, 2 = AVX2+FMA, 3 = AVX-512 (512-bit EVEX);
 * -1 on targets without the x86 backend.  Diagnostics only. */
int fsh_simd_level(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  /* FSH_SIMD_H */
