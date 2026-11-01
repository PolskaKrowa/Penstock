struct Body {
    x: f64, y: f64, z: f64,
    vx: f64, vy: f64, vz: f64,
    id: u64, flags: u64,
}
fn main() {
    let n: usize = 1000;
    let b: soa(Body) = soa_new(Body, n);
    defer soa_free(b);
    parfor (i in 0..n) {
        b[i].x = 1.5;
        b[i].vy = 2.5;
    }
    let mut t: f64 = 0.0;
    for (i in 0..n) { t = t + b[i].x + b[i].vy; }
    print_f64(t, 2); println();
}
