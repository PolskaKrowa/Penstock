// R4 bench: FFT-multiply-shaped batch pipeline — three phase loops over
// the batch dimension (forward transform / pointwise multiply / inverse)
// vs the same code with the loop-opts disabled (--no-loop-opts).  The
// fused build runs the three phases back-to-back per batch element, so
// each 64-element slice stays in L1 across phase boundaries instead of
// being re-streamed from DRAM on every phase sweep.
fn fwd(a: *f64, b: *f64, n: i64) {
    let mut aa: *f64 = a;
    let mut bb: *f64 = b;
    for (j in 0..n) {
        aa[j] = aa[j] * 0.5 + 0.25;
        bb[j] = bb[j] * 0.25 + 0.125;
    }
}
fn pw(a: *f64, b: *f64, c: *f64, n: i64) {
    let mut aa: *f64 = a;
    let mut bb: *f64 = b;
    let mut cc: *f64 = c;
    for (j in 0..n) {
        aa[j] = aa[j] * cc[j] - bb[j] * 0.5;
        bb[j] = bb[j] + cc[j] * 2.0;
    }
}
fn inv(a: *f64, b: *f64, o: *f64, n: i64) {
    let mut aa: *f64 = a;
    let mut bb: *f64 = b;
    let mut oo: *f64 = o;
    for (j in 0..n) {
        oo[j] = aa[j] - bb[j];
    }
}
fn main() {
    let len: i64 = 64;
    let nb: i64 = 16384;
    let tot: i64 = len * nb;
    let reps: i64 = 32;
    let re: *f64 = malloc(f64, tot);
    let im: *f64 = malloc(f64, tot);
    let br: *f64 = malloc(f64, tot);
    let out: *f64 = malloc(f64, tot);
    for (k in 0..tot) {
        re[k] = (k as f64) * 0.5;
        im[k] = 1.0;
        br[k] = 2.0;
    }
    for (r in 0..reps) {
        for (k in 0..nb) { fwd(&re[k*len], &im[k*len], len); }
        for (k in 0..nb) { pw(&re[k*len], &im[k*len], &br[k*len], len); }
        for (k in 0..nb) { inv(&re[k*len], &im[k*len], &out[k*len], len); }
    }
    free(re); free(im); free(br);
    let mut s: f64 = 0.0;
    for (k in 0..tot) { s = s + out[k]; }
    free(out);
    print_f64(s, 2); println();
}
