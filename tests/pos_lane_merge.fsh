// R4: cross-lane predicate reuse — two adjacent branches with an
// identical, iteration-dependent condition make the vectoriser compute
// the same vector compare twice per iteration.  Neither arm writes
// anything the condition reads, so they merge (F0901): the predicate
// is computed once and reused.
fn main() {
    let n: i64 = 2048;
    let a: *f64 = malloc(f64, n);
    let b: *f64 = malloc(f64, n);
    for (i in 0..n) {
        a[i] = (i as f64);
        b[i] = 0.0;
    }
    let mut s1: f64 = 0.0;
    let mut s2: f64 = 0.0;
    for (i in 0..n) {
        if (a[i] > 128.0) { s1 = s1 + a[i]; }
        if (a[i] > 128.0) { s2 = s2 + a[i] * 0.5; }
    }
    free(a);
    free(b);
    print_f64(s1, 2); println();
    print_f64(s2, 2); println();
}
