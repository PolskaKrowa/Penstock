// audit §5 — @vectorize loop annotation: an explicit per-loop request.
// IR check: vectorize.enable metadata present on exactly the annotated
// loop, WITHOUT an interleave count (the cost model picks width/unroll).
// Runtime check: identical output to the unannotated build.

fn main() {
    let n: i64 = 2000;
    let a: *f64 = malloc(f64, n);
    for (i in 0..n) {
        let fi: f64 = i as f64;
        a[i] = fi;
    }
    @vectorize for (i in 0..n) {
        a[i] = a[i] * 3.0 + 1.0;
    }
    // untouched loop: no annotation, no metadata
    for (i in 0..n) {
        a[i] = a[i] - 1.0;
    }
    let mut s: f64 = 0.0;
    for (i in 0..n) { s = s + a[i]; }
    print_f64(s, 2);
    println();
    free(a);
}
