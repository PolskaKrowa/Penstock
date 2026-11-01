// R5: cross-lane analysis must search below the loop's direct children.
// The `mode` branch is nested under a data-dependent IF, but `mode` is
// invariant and never written in the loop.  Hoisting it around the complete
// loop specialises the kernel without changing the data-dependent guard.
fn main() {
    let n: i64 = 4096;
    let a: *f64 = malloc(f64, n);
    for (i in 0..n) { a[i] = (i as f64) * 0.25; }
    let mode: bool = true;
    for (i in 0..n) {
        if (a[i] > 0.0) {
            if (mode) {
                a[i] = (a[i] * 2.0 + 3.0) * 1.25 + 4.0;
            } else {
                a[i] = (a[i] * 3.0 - 2.0) * 0.75 - 1.0;
            }
        }
    }
    let mut sum: f64 = 0.0;
    for (i in 0..n) { sum = sum + a[i]; }
    free(a);
    print_f64(sum, 2); println();
}
