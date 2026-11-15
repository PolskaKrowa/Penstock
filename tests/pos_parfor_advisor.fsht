// optimisation-audit §6.10 AutoParforAdvisor: analysis-only advice for
// provably independent loops (disjoint injective writes) and for
// accumulate-into-scalar reduction shapes.  No rewrite happens.
fn main() -> i32 {
    let n: usize = 64;
    let a = malloc(i64, n);
    let b = malloc(i64, n);
    // parallel-safe: injective writes to a, reads only b
    for (i in 0..n) { a[i] = i as i64; }
    for (i in 0..n) { b[i] = a[i] * 2; }
    // reduction shape: accumulate into a scalar
    let mut s: i64 = 0;
    for (i in 0..n) { s = s + (i); }
    print_i64(b[9]); print(" "); print_i64(s); println();   // 18 2016
    free(a);
    free(b);
    return 0;
}
