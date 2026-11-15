// Phase 3c (report §12/§14): break/continue inside parfor (lane-local).
fn main() {
    let n: usize = 1000;
    let d: *i64 = malloc(i64, n);
    parfor (i in 0..n) {
        if (i % 2 == 0) { continue; }
        if (i > 500) { break; }
        d[i] = i as i64;
    }
    let mut s: i64 = 0;
    for (i in 0..n) { s = s + d[i]; }
    print_i64(s); println();   // odds <= 499: 250^2 = 62500
    free(d);
}
