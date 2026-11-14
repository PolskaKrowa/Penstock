// mem & vectorisation stage — SMT pairing must NOT fire: the second
// region reads `out`, which the first region writes (a true flow
// dependence between the regions).  Running them concurrently would be
// a data race, so --smt-interleave leaves the two synchronous
// frt_parfor dispatches alone (no F0922).
fn main() {
    let n: usize = 1000000;
    let src: *f64 = malloc(f64, n);
    let idx: *usize = malloc(usize, n);
    let out: *f64 = malloc(f64, n);
    let chk: *f64 = malloc(f64, n);
    parfor (i in 0..n) {
        idx[i] = (i * 7919) % 1048576;
        src[i] = (i as f64) + 1.0;
    }
    // A: memory-bound gather (writes out)
    parfor (i in 0..n) {
        out[i] = src[idx[i]];
    }
    // B: READS out — depends on A, must stay sequential
    parfor (i in 0..n) {
        chk[i] = out[i] * 2.0;
    }
    print_f64(out[0] + chk[0], 2); println();
    free(src);
    free(idx);
    free(out);
    free(chk);
}
