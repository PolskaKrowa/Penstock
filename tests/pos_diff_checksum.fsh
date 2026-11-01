// audit §2.4 — differential execution: -O0 vs -O3 must produce identical
// results for a mixed arithmetic/array kernel with a checksum.  Run by
// tests/run.sh at every supported optimisation level.

fn kernel(a: *f64, b: *f64, n: i64) -> f64 {
    let mut aa: *f64 = a;   // mut rebind (the §2.4 idiom)
    let mut bb: *f64 = b;
    let mut acc: f64 = 0.0;
    for (i in 0..n) {
        aa[i] = aa[i] * 1.5 + 2.0;
        bb[i] = aa[i] - bb[i] * 0.25;
        acc = acc + aa[i] * bb[i];
    }
    return acc;
}

@noinline fn read_at(p: *i64, j: i64) -> i64 {
    return p[j];
}

fn main() {
    let n: i64 = 512;
    let a: *f64 = malloc(f64, n);
    let b: *f64 = malloc(f64, n);
    for (i in 0..n) {
        let fi: f64 = i as f64;
        a[i] = fi + 0.25;
        b[i] = 1.0 / (fi + 1.0);
    }
    let x: f64 = kernel(a, b, n);
    // interleave a pointer-reading helper with stores, twice
    let m: *i64 = malloc(i64, 4);
    m[0] = 3;
    m[1] = read_at(m, 0);
    m[2] = 7;
    m[3] = read_at(m, 2);
    print_f64(x, 6);
    print_i64(m[1]); print_i64(m[3]);
    println();
    free(a); free(b); free(m);
}
