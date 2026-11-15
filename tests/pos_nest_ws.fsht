fn main() {
    let n: i64 = 100;
    let mut acc: i64 = 0;
    parfor (i in 0..n) [reduction(+:acc)] {
        let inner: usize = i + 1;
        let mut local: i64 = 0;
        parfor (j in 0..inner) [reduction(+:local)] { local = local + 1; }
        acc = acc + local;
    }
    print_i64(acc); println();
}
