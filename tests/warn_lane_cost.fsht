// R4: cross-lane cost model — the branch is uniform but the loop is
// vector-light (3 lane ops < 6 threshold): hoisting would duplicate the
// body for less vector work than the branch costs, so the pass reports
// F0902 and leaves the loop alone.
fn main() {
    let n: i64 = 1024;
    let a: *f64 = malloc(f64, n);
    for (i in 0..n) { a[i] = (i as f64); }
    let flag: bool = true;
    let mut s: f64 = 0.0;
    // 1..n bounds keep this from fusing with the init loop; the point
    // here is the uniform branch inside it
    for (i in 1..n) {
        if (flag) { s = s + a[i]; }
    }
    free(a);
    print_f64(s, 2); println();
}
