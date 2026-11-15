// audit §2.4 — @pure contract violation: the function performs I/O.

@pure fn hello() -> i64 {
    print("side effect!");
    println();
    return 0;
}

fn main() {
    let r: i64 = hello();
    print_i64(r);
    println();
}
