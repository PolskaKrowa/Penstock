fn double_it(x: i64) -> i64 { return x * 2; }
fn main() {
    let n: usize = 100;
    let d: *i64 = malloc(i64, n);
    let g: fn(i64) -> i64 = double_it;
    parfor (i in 0..n) {
        d[i] = g(i as i64);
    }
    free(d);
}
