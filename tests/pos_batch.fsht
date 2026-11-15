// mem & vectorisation stage — batch-ops (F0920): two independent
// statement chains are written INTERLEAVED (x0, y0, x1, y1); the
// separable-routine analysis finds the two strands and their batchable
// groups, and --batch-ops reorders to x0, x1, y0, y1 so the backend's
// SLP vectoriser can pack the isomorphic pairs instead of scalarising
// interleaved chains (which vectorise into lane-crossing shuffles).
fn main() {
    let n: i64 = 8;
    let a0: *f64 = malloc(f64, n);
    let a1: *f64 = malloc(f64, n);
    for (i in 0..n) {
        a0[i] = (i as f64) + 1.0;
        a1[i] = (i as f64) + 2.0;
    }
    // interleaved chains:  x0 -> y0  and  x1 -> y1
    let x0: f64 = a0[3] * 2.0 + 1.0;
    let y0: f64 = x0 * x0;
    let x1: f64 = a1[3] * 2.0 + 1.0;
    let y1: f64 = x1 * x1;
    print_f64(y0 + y1, 2); println();
    free(a0);
    free(a1);
}
