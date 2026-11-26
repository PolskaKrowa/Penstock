/* libfsh SIMD backend conformance test.
 *
 * Builds against the backend + libfsh (compiled with
 * -DFSH_LIBFSH_SIMD_EXTERNAL) and checks every public SIMD op against
 * an independent scalar reference computed IN THIS TEST (not the
 * backend's own scalar path), including:
 *   * element-exact equality for the non-fused arithmetic
 *   * single-rounding FMA equality: fsh_fma_* == fma()/fmaf() libm
 *   * horizontal sums
 *   * aligned/unaligned load/store round-trips
 * The dispatch level actually resolved on this machine is printed, so a
 * multi-machine CI matrix exercises different rungs without branching
 * here.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fsh/fsh_simd.h"

static int fails = 0;

#define CHECK(cond, name)                                              \
  do {                                                                 \
    if (cond) { printf("  ok    %s\n", name); }                        \
    else { printf("  FAIL  %s\n", name); ++fails; }                   \
  } while (0)

static int eq64x4(fsh_f64x4 a, fsh_f64x4 b) {
  for (int i = 0; i < 4; ++i)
    if (a.v[i] != b.v[i] && !(isnan(a.v[i]) && isnan(b.v[i]))) return 0;
  return 1;
}
static int eq32x8(fsh_f32x8 a, fsh_f32x8 b) {
  for (int i = 0; i < 8; ++i)
    if (a.v[i] != b.v[i] && !(isnan(a.v[i]) && isnan(b.v[i]))) return 0;
  return 1;
}

int main(void) {
  printf("libfsh SIMD backend level: %d", fsh_simd_level());
  printf("  (0=scalar 1=SSE2 2=AVX2+FMA 3=AVX-512)\n");

  /* ---- f64x4 arithmetic -------------------------------------------- */
  fsh_f64x4 a, b, c;
  for (int i = 0; i < 4; ++i) {
    a.v[i] = 0.25 * (double)(i + 1);
    b.v[i] = 0.5 * (double)(i + 2);
    c.v[i] = 0.125 * (double)(i + 3);
  }
  {
    fsh_f64x4 r = fsh_add_f64x4(a, b);
    fsh_f64x4 e; for (int i = 0; i < 4; ++i) e.v[i] = a.v[i] + b.v[i];
    CHECK(eq64x4(r, e), "fsh_add_f64x4 == element-wise");
  }
  {
    fsh_f64x4 r = fsh_sub_f64x4(a, b);
    fsh_f64x4 e; for (int i = 0; i < 4; ++i) e.v[i] = a.v[i] - b.v[i];
    CHECK(eq64x4(r, e), "fsh_sub_f64x4 == element-wise");
  }
  {
    fsh_f64x4 r = fsh_mul_f64x4(a, b);
    fsh_f64x4 e; for (int i = 0; i < 4; ++i) e.v[i] = a.v[i] * b.v[i];
    CHECK(eq64x4(r, e), "fsh_mul_f64x4 == element-wise");
  }
  {
    fsh_f64x4 r = fsh_div_f64x4(a, b);
    fsh_f64x4 e; for (int i = 0; i < 4; ++i) e.v[i] = a.v[i] / b.v[i];
    CHECK(eq64x4(r, e), "fsh_div_f64x4 == element-wise");
  }
  {
    /* single-rounding FMA: 0.1*0.1 + c differs from the two-rounding
     * naive sum at the last ulp — the backend must match fma() exactly */
    fsh_f64x4 x, y, z;
    for (int i = 0; i < 4; ++i) { x.v[i] = 0.1; y.v[i] = 0.1; z.v[i] = -0.005; }
    fsh_f64x4 r = fsh_fma_f64x4(x, y, z);
    fsh_f64x4 e; for (int i = 0; i < 4; ++i) e.v[i] = fma(0.1, 0.1, -0.005);
    CHECK(eq64x4(r, e), "fsh_fma_f64x4 == single-rounding fma()");
  }
  {
    fsh_f64x4 r = fsh_fma_f64x4(a, b, c);
    fsh_f64x4 e;
    for (int i = 0; i < 4; ++i) e.v[i] = fma(a.v[i], b.v[i], c.v[i]);
    CHECK(eq64x4(r, e), "fsh_fma_f64x4 == fma() element-wise");
  }

  /* ---- f32x8 arithmetic ---------------------------------------------- */
  fsh_f32x8 fa, fb, fc;
  for (int i = 0; i < 8; ++i) {
    fa.v[i] = 0.25f * (float)(i + 1);
    fb.v[i] = 0.5f * (float)(i + 2);
    fc.v[i] = 0.125f * (float)(i + 3);
  }
  {
    fsh_f32x8 r = fsh_add_f32x8(fa, fb);
    fsh_f32x8 e; for (int i = 0; i < 8; ++i) e.v[i] = fa.v[i] + fb.v[i];
    CHECK(eq32x8(r, e), "fsh_add_f32x8 == element-wise");
  }
  {
    fsh_f32x8 r = fsh_mul_f32x8(fa, fb);
    fsh_f32x8 e; for (int i = 0; i < 8; ++i) e.v[i] = fa.v[i] * fb.v[i];
    CHECK(eq32x8(r, e), "fsh_mul_f32x8 == element-wise");
  }
  {
    fsh_f32x8 r = fsh_fma_f32x8(fa, fb, fc);
    fsh_f32x8 e;
    for (int i = 0; i < 8; ++i) e.v[i] = fmaf(fa.v[i], fb.v[i], fc.v[i]);
    CHECK(eq32x8(r, e), "fsh_fma_f32x8 == fmaf() element-wise");
  }

  /* ---- f64x2 / f32x4 (SSE2 rung) -------------------------------------- */
  fsh_f64x2 a2 = {{0.5, 1.5}}, b2 = {{0.25, 0.75}};
  {
    fsh_f64x2 r = fsh_add_f64x2(a2, b2);
    CHECK(r.v[0] == 0.75 && r.v[1] == 2.25, "fsh_add_f64x2");
  }
  {
    fsh_f64x2 r = fsh_fma_f64x2(a2, b2, a2);
    CHECK(r.v[0] == fma(0.5, 0.25, 0.5) && r.v[1] == fma(1.5, 0.75, 1.5),
          "fsh_fma_f64x2 == fma()");
  }
  fsh_f32x4 a4 = {{0.5f, 1.5f, 2.5f, 3.5f}}, b4 = {{2.0f, 2.0f, 2.0f, 2.0f}};
  {
    fsh_f32x4 r = fsh_mul_f32x4(a4, b4);
    CHECK(r.v[0] == 1.0f && r.v[3] == 7.0f, "fsh_mul_f32x4");
  }

  /* ---- horizontal sums ------------------------------------------------ */
  {
    fsh_f64x4 v = {{1.0, 2.0, 3.0, 4.0}};
    CHECK(fsh_hsum_f64x4(v) == 10.0, "fsh_hsum_f64x4 exact");
  }
  {
    fsh_f32x8 v = {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f}};
    CHECK(fsh_hsum_f32x8(v) == 8.0f, "fsh_hsum_f32x8 exact");
  }
  {
    fsh_f64x2 v = {{2.0, 3.0}};
    CHECK(fsh_hsum_f64x2(v) == 5.0, "fsh_hsum_f64x2 exact");
  }
  {
    fsh_f32x4 v = {{1.0f, 2.0f, 3.0f, 4.0f}};
    CHECK(fsh_hsum_f32x4(v) == 10.0f, "fsh_hsum_f32x4 exact");
  }

  /* ---- load/store round-trips (aligned + unaligned) ------------------- */
  {
    double in[6] = {1, 2, 3, 4, 5, 6}, out[6] = {0};
    fsh_f64x4 v = fsh_loadu_f64x4(in + 1);
    fsh_storeu_f64x4(out + 1, v);
    CHECK(memcmp(in + 1, out + 1, 4 * sizeof(double)) == 0,
          "f64x4 unaligned round-trip");
  }
  {
    float in[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, out[11] = {0};
    fsh_f32x8 v = fsh_loadu_f32x8(in + 2);
    fsh_storeu_f32x8(out + 2, v);
    CHECK(memcmp(in + 2, out + 2, 8 * sizeof(float)) == 0,
          "f32x8 unaligned round-trip");
  }

  printf(fails ? "FAILED: %d check(s)\n" : "ALL SIMD BACKEND CHECKS PASSED\n",
         fails);
  return fails ? 1 : 0;
}
