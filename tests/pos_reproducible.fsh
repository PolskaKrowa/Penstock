// Phase 5 (report §12): @reproducible — bitwise-identical for any thread
// count (the grouping-sensitive data makes unflagged parfor diverge).
fn main() {
    let n: usize = 100000;
    let d: *f64 = malloc(f64, n);
    parfor (i in 0..n) {
        if (i < 50000) { d[i] = 1000000000.0; }
        else { d[i] = 0.0000001; }
    }
    let mut acc: f64 = 0.0;
    @reproducible parfor (i in 0..n) [reduction(+: acc)] {
        acc = acc + d[i];
    }
    print_f64(acc, 15); println();
    free(d);
}
