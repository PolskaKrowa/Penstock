// R3: -O4 evolution smoke — parfor reduction workload, ~tens of ms/run,
// plenty of tuning surface for the stochastic search (threads / pin /
// chunk / opt level / march).
fn main() {
    let n: usize = 400000;
    let a: *f64 = malloc(f64, n);
    parfor (i in 0..n) { a[i] = (i as f64) * 0.5; }
    let mut s: f64 = 0.0;
    let reps: i64 = 40;
    for (r in 0..reps) {
        parfor (i in 0..n) [reduction(+:s)] { s = s + a[i]; }
    }
    free(a);
    print_f64(s, 2); println();
}
