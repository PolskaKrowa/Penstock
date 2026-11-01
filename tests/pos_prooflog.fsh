// optimisation-audit §5.2/§10.4: --proof-log records one line per
// APPLIED transform with the legality premises it relied on.
fn main() -> i32 {
    let mut s: i64 = 0;
    for (i in 0..20) { s = s + (i * 2 + 3); }
    print_i64(s); println();    // 440
    return 0;
}
