// audit §2.4 — @pure contract violation: the function calls an impure
// callee (F0304).  Summaries propagate interprocedurally.

fn noisy(p: *i64) -> i64 {
    print_i64(p[0]);
    return p[0];
}

@pure fn wrap(p: *i64) -> i64 {
    return noisy(p);
}

fn main() {
    let p: *i64 = malloc(i64, 1);
    p[0] = 4;
    print_i64(wrap(p));
    println();
    free(p);
}
