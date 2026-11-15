// R4: loop fusion — FFT-multiply-shaped batch pipeline. Three phase
// functions take per-batch slices (`&re[k*len]`) and write through
// mut-rebound views of their params (the language's callee-write
// idiom); the compiler's footprint analysis proves each callee touches
// only its [base, base+len) slice with call-site stride == len, so the
// three batch loops over 0..nb fuse into ONE sweep (F0908): forward
// transform, pointwise arithmetic and inverse transform run per batch
// element while the batching loop is retained.
fn fwd(a: *f64, b: *f64, n: i64) {
    let mut aa: *f64 = a;
    let mut bb: *f64 = b;
    for (j in 0..n) {
        aa[j] = aa[j] + 0.5;
        bb[j] = bb[j] * 0.25;
    }
}
fn pw(a: *f64, b: *f64, c: *f64, n: i64) {
    let mut aa: *f64 = a;
    let mut bb: *f64 = b;
    let mut cc: *f64 = c;
    for (j in 0..n) {
        aa[j] = aa[j] * cc[j];
        bb[j] = bb[j] * cc[j];
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
    let len: i64 = 16;
    let nb: i64 = 64;
    let tot: i64 = len * nb;
    let re: *f64 = malloc(f64, tot);
    let im: *f64 = malloc(f64, tot);
    let br: *f64 = malloc(f64, tot);
    let out: *f64 = malloc(f64, tot);
    for (k in 0..tot) {
        re[k] = (k as f64) * 0.5;
        im[k] = 1.0;
        br[k] = 2.0;
    }
    for (k in 0..nb) { fwd(&re[k*len], &im[k*len], len); }
    for (k in 0..nb) { pw(&re[k*len], &im[k*len], &br[k*len], len); }
    for (k in 0..nb) { inv(&re[k*len], &im[k*len], &out[k*len], len); }
    free(re); free(im); free(br);
    let mut s: f64 = 0.0;
    for (k in 0..tot) { s = s + out[k]; }
    free(out);
    print_f64(s, 2); println();
}
