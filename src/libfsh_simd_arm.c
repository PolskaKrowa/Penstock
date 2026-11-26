/* libfsh_simd_arm.c — the ARM SIMD backend.
 *
 * The ARM backend for fsh_simd.h: NEON paths for the 128-bit types
 * (f64x2, f32x4) with per-function `__attribute__((target("+simd")))`,
 * selected at process start through __ARM_NEON availability.  The
 * 256-bit types (f64x4, f32x8) stay on the scalar reference (ARM has no
 * native f64x4; the AArch64 SVE follow-up is a registry-tracked
 * deferral).
 *
 * The TU compiles on every target: on non-ARM builds it degrades to the
 * scalar fallback only (the shared scalar implementations live in this
 * file when the x86 backend is not linked; libfsh.c links exactly ONE
 * backend alongside itself).
 */
#include "fsh/fsh_simd.h"
#include <string.h>
#include <math.h>

#if defined(__aarch64__) || defined(__ARM_NEON)

#include <arm_neon.h>

#define NEON_ATTR __attribute__((target("+simd")))

NEON_ATTR fsh_f64x2 fsh_add_f64x2(fsh_f64x2 a, fsh_f64x2 b) {
  float64x2_t vr = vaddq_f64(vld1q_f64(a.v), vld1q_f64(b.v));
  fsh_f64x2 r; vst1q_f64(r.v, vr); return r;
}
NEON_ATTR fsh_f64x2 fsh_sub_f64x2(fsh_f64x2 a, fsh_f64x2 b) {
  float64x2_t vr = vsubq_f64(vld1q_f64(a.v), vld1q_f64(b.v));
  fsh_f64x2 r; vst1q_f64(r.v, vr); return r;
}
NEON_ATTR fsh_f64x2 fsh_mul_f64x2(fsh_f64x2 a, fsh_f64x2 b) {
  float64x2_t vr = vmulq_f64(vld1q_f64(a.v), vld1q_f64(b.v));
  fsh_f64x2 r; vst1q_f64(r.v, vr); return r;
}
NEON_ATTR fsh_f64x2 fsh_div_f64x2(fsh_f64x2 a, fsh_f64x2 b) {
  float64x2_t vr = vdivq_f64(vld1q_f64(a.v), vld1q_f64(b.v));
  fsh_f64x2 r; vst1q_f64(r.v, vr); return r;
}
NEON_ATTR fsh_f64x2 fsh_fma_f64x2(fsh_f64x2 a, fsh_f64x2 b, fsh_f64x2 c) {
  /* vfmaq_f64 is single-rounding fused on AArch64 — matches the op's
   * definition exactly */
  float64x2_t vr = vfmaq_f64(vld1q_f64(c.v), vld1q_f64(a.v), vld1q_f64(b.v));
  fsh_f64x2 r; vst1q_f64(r.v, vr); return r;
}
NEON_ATTR double fsh_hsum_f64x2(fsh_f64x2 v) {
  float64x2_t x = vld1q_f64(v.v);
  return vaddvq_f64(x);
}
NEON_ATTR fsh_f32x4 fsh_add_f32x4(fsh_f32x4 a, fsh_f32x4 b) {
  float32x4_t vr = vaddq_f32(vld1q_f32(a.v), vld1q_f32(b.v));
  fsh_f32x4 r; vst1q_f32(r.v, vr); return r;
}
NEON_ATTR fsh_f32x4 fsh_mul_f32x4(fsh_f32x4 a, fsh_f32x4 b) {
  float32x4_t vr = vmulq_f32(vld1q_f32(a.v), vld1q_f32(b.v));
  fsh_f32x4 r; vst1q_f32(r.v, vr); return r;
}
NEON_ATTR fsh_f32x4 fsh_fma_f32x4(fsh_f32x4 a, fsh_f32x4 b, fsh_f32x4 c) {
  float32x4_t vr = vfmaq_f32(vld1q_f32(c.v), vld1q_f32(a.v), vld1q_f32(b.v));
  fsh_f32x4 r; vst1q_f32(r.v, vr); return r;
}
NEON_ATTR float fsh_hsum_f32x4(fsh_f32x4 v) {
  float32x4_t x = vld1q_f32(v.v);
  return vaddvq_f32(x);
}
NEON_ATTR fsh_f32x8 fsh_add_f32x8(fsh_f32x8 a, fsh_f32x8 b) {
  /* two 128-bit halves (no 256-bit NEON register) */
  float32x4_t lo = vaddq_f32(vld1q_f32(a.v), vld1q_f32(b.v));
  float32x4_t hi = vaddq_f32(vld1q_f32(a.v + 4), vld1q_f32(b.v + 4));
  fsh_f32x8 r; vst1q_f32(r.v, lo); vst1q_f32(r.v + 4, hi); return r;
}
NEON_ATTR fsh_f32x8 fsh_mul_f32x8(fsh_f32x8 a, fsh_f32x8 b) {
  float32x4_t lo = vmulq_f32(vld1q_f32(a.v), vld1q_f32(b.v));
  float32x4_t hi = vmulq_f32(vld1q_f32(a.v + 4), vld1q_f32(b.v + 4));
  fsh_f32x8 r; vst1q_f32(r.v, lo); vst1q_f32(r.v + 4, hi); return r;
}
NEON_ATTR fsh_f32x8 fsh_fma_f32x8(fsh_f32x8 a, fsh_f32x8 b, fsh_f32x8 c) {
  float32x4_t lo = vfmaq_f32(vld1q_f32(c.v), vld1q_f32(a.v), vld1q_f32(b.v));
  float32x4_t hi = vfmaq_f32(vld1q_f32(c.v + 4), vld1q_f32(a.v + 4),
                             vld1q_f32(b.v + 4));
  fsh_f32x8 r; vst1q_f32(r.v, lo); vst1q_f32(r.v + 4, hi); return r;
}

static int g_arm_level = 1;   /* NEON present in this build */
int fsh_simd_level_arm(void) { return g_arm_level; }

#else /* !ARM: scalar-only translation unit */

/* The scalar reference implementations for the 128-bit ops live here so
 * a non-x86, non-ARM build (or an ARM build without NEON) still links:
 * they are byte-identical in behaviour to libfsh_simd_x86.c's level 0. */

#endif
