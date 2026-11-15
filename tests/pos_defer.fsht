fn sum(a: *i64, n: usize) -> i64 {
    let mut t: i64 = 0;
    for (i in 0..n) { t = t + a[i]; }
    return t;
}
fn main() {
    let a: *i64 = malloc(i64, 10);
    defer free(a);
    for (i in 0..10) { a[i] = i as i64; }
    print_i64(sum(a, 10)); println();
}
