// optimisation-audit §6.5 PartialUnroll: constant trip 20 unrolled by 8
// with an exact 4-iteration remainder loop; the iteration domain is
// partitioned exactly, so the sum is unchanged.
fn main() -> i32 {
    let mut s: i64 = 0;
    for (i in 0..20) { s = s + (i * 2 + 3); }
    print_i64(s); println();    // 440
    // odd trip: remainder of 1 must not be lost
    let mut t: i64 = 0;
    for (i in 0..33) { t = t + (1); }
    print_i64(t); println();    // 33
    return 0;
}
