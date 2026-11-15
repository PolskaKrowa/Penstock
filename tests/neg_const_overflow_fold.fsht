// optimisation-audit §4.4 (P0), regression per §10.1:
// constant evaluation must use typed bit-vector semantics — never host
// int64_t overflow — and narrowing casts must truncate.
fn main() -> i32 {
    // INT64_MIN built without a literal-overflow: -(2^63 - 1) - 1
    let a: i64 = -9223372036854775807 - 1;
    // negating INT64_MIN wraps back to INT64_MIN (two's complement);
    // the historical evaluator negated int64_t directly = UB in the
    // compiler itself
    let b: i64 = 0 - a;
    if (b == a) { print("1"); } else { print("0"); } println();

    // u64 value above INT64_MAX: must not be reinterpreted as negative
    let u: u64 = 18446744073709551615;
    if (u > 9223372036854775807) { print("1"); } else { print("0"); } println();

    // narrowing cast truncates to the low 8 bits: 255
    let t: u8 = (u as u8);
    print_i64(t as i64); println();          // 255

    // subtraction across the signed boundary wraps: hi - lo == -1
    let lo: i64 = -9223372036854775807 - 1;
    let hi: i64 = 9223372036854775807;
    if (hi - lo == -1) { print("1"); } else { print("0"); } println();

    // 1  (2^64-1 wraps to -1)6-iteration unroll threshold boundary: trip exactly 16 unrolls,
    // 17 does not (regression for the checked trip analysis)
    let mut s1: i64 = 0;
    for (i in 0..16) { s1 = s1 + (1); }
    let mut s2: i64 = 0;
    for (i in 0..17) { s2 = s2 + (1); }
    print_i64(s1 + s2); println();           // 33
    return 0;
}
