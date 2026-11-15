struct Pt { x: f64, y: f64 }
fn main() {
    let n: usize = 80;
    let pts: *Pt = malloc(Pt, n);
    parfor (i in 0..n) {
        pts[i].x = 1.0;
        pts[i].y = 1.0;
    }
    let mut t: f64 = 0.0;
    for (i in 0..n) { t = t + pts[i].x + pts[i].y; }
    print_f64(t, 2); println();
    free(pts);
}
