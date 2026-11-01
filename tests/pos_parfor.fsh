fn main() {
    let n: i64 = 8000000;
    let mut s: i64 = 0;
    parfor (i in 0..n) [reduction(+:s)] { s = s + 1; }
    print_i64(s); println();
}
