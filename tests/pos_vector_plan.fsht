// mem & vectorisation stage — vector-plan advisory (F0921): the fn has
// two separable routines (the x0-chain and the x1-chain) with batchable
// isomorphic statements.  --vector-plan reports the strands, the batch
// groups and their dataflow; it never rewrites anything.
fn planme(a0: *f64, a1: *f64) -> f64 {
    let x0: f64 = a0[3] * 2.0 + 1.0;
    let x1: f64 = a1[3] * 2.0 + 1.0;
    let y0: f64 = x0 * x0;
    let y1: f64 = x1 * x1;
    return y0 + y1;
}
fn main() {
    let n: i64 = 8;
    let a0: *f64 = malloc(f64, n);
    let a1: *f64 = malloc(f64, n);
    for (i in 0..n) {
        a0[i] = (i as f64) + 1.0;
        a1[i] = (i as f64) + 2.0;
    }
    print_f64(planme(a0, a1), 2); println();
    free(a0);
    free(a1);
}
