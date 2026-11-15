// R3: --soa=auto end-to-end.
//  - b: only sparse loops (x, vy hot; 2 of 8 fields) -> auto-transforms
//       to soa(P8) planes
//  - w: one sparse loop AND one full-width loop -> vetoed (planes would
//       hurt the 8/8-field loop); stays AoS, F0408 still advises
// Output must be identical with and without --soa=auto.
struct P8 {
    x: f64, y: f64, z: f64, w: f64,
    vx: f64, vy: f64, vz: f64, vw: f64,
}
fn main() {
    let n: usize = 50000;

    // ---- b: sparse everywhere -> transforms
    let b: *P8 = malloc(P8, n);
    parfor (i in 0..n) { b[i].x = 1.5; b[i].vy = 2.5; }
    let mut sb: f64 = 0.0;
    parfor (i in 0..n) [reduction(+:sb)] { sb = sb + b[i].x + b[i].vy; }
    free(b);

    // ---- w: sparse init loop + full-width reduce loop -> vetoed
    let w: *P8 = malloc(P8, n);
    parfor (i in 0..n) { w[i].x = 1.5; }
    let mut sw: f64 = 0.0;
    parfor (i in 0..n) [reduction(+:sw)] {
        sw = sw + w[i].x + w[i].y + w[i].z + w[i].w
                  + w[i].vx + w[i].vy + w[i].vz + w[i].vw;
    }
    free(w);

    print_f64(sb, 2); print(" "); print_f64(sw, 2); println();
}
