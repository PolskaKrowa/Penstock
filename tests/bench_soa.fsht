// AoS vs SoA stress: 8-field struct, touch 2 fields hot.
// F0408 should flag the AoA loop; SoA version should win.
struct P8 {
    x: f64, y: f64, z: f64, w: f64,
    vx: f64, vy: f64, vz: f64, vw: f64,
}
fn main() {
    let n: usize = 4000000;
    let reps: i64 = 10;

    // ---- AoS
    let a: *P8 = malloc(P8, n);
    parfor (i in 0..n) {
        a[i].x = 1.5;
        a[i].vy = 2.5;
    }
    let t0: u64 = freshet.time_ns();
    let mut sa: f64 = 0.0;
    for (r in 0..reps) {
        parfor (i in 0..n) [reduction(+:sa)] {
            sa = sa + a[i].x + a[i].vy;
        }
    }
    let t1: u64 = freshet.time_ns();
    free(a);

    // ---- SoA
    let s: soa(P8) = soa_new(P8, n);
    parfor (i in 0..n) {
        s[i].x = 1.5;
        s[i].vy = 2.5;
    }
    let t2: u64 = freshet.time_ns();
    let mut sb: f64 = 0.0;
    for (r in 0..reps) {
        parfor (i in 0..n) [reduction(+:sb)] {
            sb = sb + s[i].x + s[i].vy;
        }
    }
    let t3: u64 = freshet.time_ns();
    defer soa_free(s);

    print("AoS  sum="); print_f64(sa, 1);
    print("  ~"); print_i64(((t1 - t0) >> 20) as i64); print(" ms");
    print(" | SoA sum="); print_f64(sb, 1);
    print("  ~"); print_i64(((t3 - t2) >> 20) as i64); print(" ms");
    print(" | speedup ");
    print_f64((t1 - t0) as f64 / (t3 - t2) as f64, 2);
    println();
}
