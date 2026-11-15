// optimisation-audit §10.1: independent loops over disjoint regions
// (write-a then read-a across the pair, same-iteration affine indices)
// may still fuse — the tier-2 dependence proof covers the flow.
fn main() {
    let n: usize = 64;
    let a = malloc(i64, n);
    let b = malloc(i64, n);
    for (i in 0..n) { a[i] = i as i64; }
    for (i in 0..n) { b[i] = a[i] + 1; }
    print_i64(b[10]); println();    // 11
    free(a);
    free(b);
}
