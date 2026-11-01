// audit §2.4 — @pure contract violation: the function writes through a
// pointer (F0304).  Per spec §3.1 "@pure: reads no memory and writes
// no memory" — a violated contract is a compile error, not a bypass.

@pure fn set_it(p: *i64, v: i64) -> i64 {
    p[0] = v;
    return 0;
}

fn main() {
    let p: *i64 = malloc(i64, 1);
    p[0] = 1;
    let r: i64 = set_it(p, 5);
    print_i64(r);
    println();
    free(p);
}
