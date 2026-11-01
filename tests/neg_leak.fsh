fn main() {
    let a: *i64 = malloc(i64, 10);   // never freed: F0301
    a[0] = 1;
    print_i64(a[0]); println();
}
