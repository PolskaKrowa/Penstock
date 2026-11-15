fn main() {
    let n: i64 = 1000000;
    let mut s1: i64 = 0;
    parfor (i in 0..n) [reduction(+:s1), affinity:spread] { s1 = s1 + 1; }
    let mut s2: i64 = 0;
    parfor (i in 0..n) [reduction(+:s2), affinity:close, threads:2] { s2 = s2 + 1; }
    print_i64(s1 + s2); println();
}
