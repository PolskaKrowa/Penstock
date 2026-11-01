/* fsh_math.h — accurate and optional fast/vector math (report §5.2 + §2.10).
 *
 * The historical runtime declares libm bindings for sin/cos/tan/log2/log10
 * and LLVM intrinsics for sqrt/fabs/floor/ceil/round/fmin/fmax/pow/exp/log.
 * The fsh_math layer:
 *   - separates *scalar math semantics* (always IEEE-754-accurate) from
 *     *vector math backends* (SLEEF / SVML / libmvec where the target
 *     supports them),
 *   - exposes both an `fsh_math_*_accurate` family (high-precision) and
 *     an `fsh_math_*_fast` family (lower precision, vectorisable) so
 *     the user controls the trade-off explicitly,
 *   - provides explicit fused-multiply-add with documented contraction
 *     policy,
 *   - exposes a parallel counter-based RNG (each iteration derives a
 *     reproducible stream from (seed, stream_id, counter) without shared
 *     mutable RNG state — see report §5.2).
 */
#ifndef FSH_MATH_H
#define FSH_MATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- accurate scalar math (always IEEE-754) --------------- */
double fsh_math_sin_f64(double x);
double fsh_math_cos_f64(double x);
double fsh_math_tan_f64(double x);
double fsh_math_log2_f64(double x);
double fsh_math_log10_f64(double x);
double fsh_math_log_f64(double x);
double fsh_math_exp_f64(double x);
double fsh_math_pow_f64(double x, double y);
double fsh_math_sqrt_f64(double x);
double fsh_math_fabs_f64(double x);
double fsh_math_floor_f64(double x);
double fsh_math_ceil_f64(double x);
double fsh_math_round_f64(double x);
double fsh_math_fmin_f64(double a, double b);
double fsh_math_fmax_f64(double a, double b);

float fsh_math_sin_f32(float x);
float fsh_math_cos_f32(float x);
float fsh_math_log2_f32(float x);
float fsh_math_exp_f32(float x);
float fsh_math_sqrt_f32(float x);
float fsh_math_fabs_f32(float x);
float fsh_math_fmin_f32(float a, float b);
float fsh_math_fmax_f32(float a, float b);

/* ---------------- fast math (lower precision, vectorisable) ------------- */
/* Fast variants have documented error envelopes (typically ~1 ULP for
 * transcendentals).  The library selects a vector backend if one is
 * available for the target; otherwise falls back to libm. */
double fsh_math_sin_fast_f64(double x);
double fsh_math_cos_fast_f64(double x);
double fsh_math_log_fast_f64(double x);
double fsh_math_exp_fast_f64(double x);

/* ---------------- fused multiply-add ---------------------------------- */
/* fsh_math_fma_f64(a, b, c) = a*b + c with single rounding.  Lowers to
 * @llvm.fma.* on targets with FMA hardware.  Contraction policy: the
 * compiler is NOT permitted to contract a*b+c into FMA unless the user
 * calls this function explicitly (Freshet default).  Mark a function
 * @strictfp to forbid contraction entirely; mark it with the (future)
 * @fastmath attribute to allow contraction of any a*b+c. */
double fsh_math_fma_f64(double a, double b, double c);
float  fsh_math_fma_f32(float a, float b, float c);

/* ---------------- parallel RNG (counter-based, report §5.2) ----------- */
/* Threefish / Philox-style counter-based RNG.  Each call returns a
 * deterministic u64 derived from (seed, stream, counter); the caller
 * advances `counter` independently per iteration so parallel loops get
 * reproducible streams without shared mutable RNG state. */
uint64_t fsh_random_u64(uint64_t seed, uint64_t stream, uint64_t counter);
double   fsh_random_uniform_f64(uint64_t seed, uint64_t stream, uint64_t counter);
int64_t  fsh_random_range_i64(uint64_t seed, uint64_t stream, uint64_t counter,
                              int64_t lo, int64_t hi);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_MATH_H
