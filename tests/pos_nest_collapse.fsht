fn main() {
    let n: i64 = 64;
    let mut acc: i64 = 0;
    parfor (i in 0..n) [reduction(+:acc), collapse:2] {
        parfor (j in 0..n) { acc = acc + 1; }
    }
    print_i64(acc); println();
}
