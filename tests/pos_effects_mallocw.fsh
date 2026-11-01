// audit §2.4 — a wrapper around malloc whose result is USED must not be
// described as memory(none); the old builtin allow-list treated malloc
// as a pure, memory-free builtin.

@noinline fn fresh(n: i64) -> *f64 {
    let p: *f64 = malloc(f64, n);
    for (i in 0..n) {
        let fi: f64 = i as f64;
        p[i] = fi * 2.0;
    }
    return p;
}

fn main() {
    let mut a: *f64 = fresh(16);
    let mut b: *f64 = fresh(16);
    a[0] = a[0] + b[0];       // distinct allocations must not alias
    print_f64(a[0], 2);
    println();
    free(a); free(b);
}
