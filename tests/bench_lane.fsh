// R4 bench: cross-lane unswitching — a UNIFORM branch inside a wide
// vector loop (condition does not depend on the lane index).  With the
// pass on, the condition is evaluated once, scalar, and the loop
// specialises per path (branch-free bodies for the vectoriser); with
// --no-loop-opts the branch stays inside the loop.
fn main() {
    let n: i64 = 4194304;
    let a: *f64 = malloc(f64, n);
    let b: *f64 = malloc(f64, n);
    for (i in 0..n) {
        a[i] = (i as f64) * 0.125;
        b[i] = 0.5;
    }
    let thr: f64 = 0.25;      // loop-invariant decision
    let mut acc: f64 = 0.0;
    let mut x: f64 = 0.0;
    for (i in 0..n) {
        let mut v: f64 = a[i];
        if (thr > 0.125) {
            v = (v * 1.5 + b[i]) * 2.0 + v;
            x = x + v * 0.5;
        } else {
            v = (v * 2.5 - b[i]) * 1.5 - v;
            x = x - v * 0.5;
        }
        acc = acc + v;
    }
    free(a); free(b);
    print_f64(acc, 2); println();
    print_f64(x, 2); println();
}
