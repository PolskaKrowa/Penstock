struct Body {
    x: f64, y: f64, z: f64,
    vx: f64, vy: f64, vz: f64,
    id: u64, flags: u64,
}
fn main() {
    let n: usize = 1024;
    let b: *Body = malloc(Body, n);
    parfor (i in 0..n) {        // touches 8B of 64B per element -> F0408
        b[i].x = 1.0;
        b[i].vy = b[i].x + b[i].x;
    }
    free(b);
}
