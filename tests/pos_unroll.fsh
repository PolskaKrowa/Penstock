// R4: full unrolling — constant-trip loops (trip 8) unroll into straight
// line code with literal indices (F0909); both loops disappear from
// the IR (no vectorize metadata left for them).
fn main() {
    let a: *f64 = malloc(f64, 8);
    for (i in 0..8) { a[i] = (i as f64) * 2.0; }
    let mut s: f64 = 0.0;
    for (i in 0..8) { s = s + a[i]; }
    free(a);
    print_f64(s, 2); println();
}
