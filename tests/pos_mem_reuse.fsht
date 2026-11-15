// mem & vectorisation stage — mem-reuse loop-churn hoisting (F0919).
// A while loop allocates a fresh buffer per iteration and frees it at
// iteration end; every read (buf[0], buf[n-1]) is covered by the fill
// loop's writes, so --mem-reuse hoists ONE allocation out of the loop.
fn main() {
    let n: i64 = 512;
    let mut total: f64 = 0.0;
    let mut k: i64 = 0;
    while (k < 8) {
        // churn: fresh buffer every iteration, freed at iteration end
        let buf: *f64 = malloc(f64, n);
        for (j in 0..n) { buf[j] = (j as f64) * 0.5; }
        total = total + buf[0] + buf[n - 1];
        free(buf);
        k = k + 1;
    }
    print_f64(total, 2); println();
}
