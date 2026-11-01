// R4: cross-lane analysis — a uniform branch inside a vector loop. The
// condition `flag` is loop-invariant: every lane takes the same path, so
// the branch is hoisted out and the loop specialised per path (F0901):
// one scalar evaluation decides for all lanes, the specialised loops are
// branch-free.  The init loop and the compute loop also fuse (same
// range, tier-2 same-index producer/consumer) before the hoist.
fn main() {
    let n: i64 = 4096;
    let a: *f64 = malloc(f64, n);
    let b: *f64 = malloc(f64, n);
    for (i in 0..n) {
        a[i] = (i as f64) * 0.25;
        b[i] = 0.5;
    }
    let flag: bool = true;
    let mut s: f64 = 0.0;
    for (i in 0..n) {
        let mut x: f64 = a[i];
        if (flag) { x = (x * 2.0 + b[i]) * 1.5; }
        else      { x = (x * 3.0 - b[i]) * 0.5; }
        b[i] = x;
        s = s + x;
    }
    free(a);
    free(b);
    print_f64(s, 2); println();
}
