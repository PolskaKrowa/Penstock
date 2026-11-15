fn inner(v: i64) -> i64 {
    let r: i64 = v * 2;
    assert(r == 42);
    return r;
}
fn outer(v: i64) -> i64 { return inner(v + 39); }
fn main() {
    let x: i64 = outer(3);
    print_i64(x); println();
}
