// optimisation-audit §6.5 LoopPeel: the first-iteration guard `k == 0`
// resolves statically in the peeled copy and disappears from the
// steady-state loop.
fn main() -> i32 {
    let mut p: i64 = 0;
    for (k in 0..17) {
        if (k == 0) { p = p + (100); } else { p = p + (1); }
    }
    print_i64(p); println();    // 100 + 16 = 116
    // skip-first pattern: `k > 0` is false only at k = 0
    let mut q: i64 = 0;
    for (k in 0..10) {
        if (k > 0) { q = q + (1); } else { q = q + (1000); }
    }
    print_i64(q); println();    // 1000 + 9 = 1009
    return 0;
}
