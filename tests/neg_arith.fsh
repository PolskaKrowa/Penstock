// report §7: runtime integer division is a PERFORMANCE warning now
// (F0402), not a compile error — a slow-but-correct program compiles.
// `--strict-performance` restores the historical hard error (see
// neg_arith_strict.fsh).
fn main() {
    let d: i64 = 0;
    let x: i64 = 100;
    let r: i64 = x / d;      // runtime divisor: F0402
    print_i64(r); println();
}
