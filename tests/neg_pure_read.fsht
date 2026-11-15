// audit §2.4 — @pure contract violation: the function READS memory
// through a pointer (F0304).  Reads count too: memory(none) means no
// memory access at all, not "does not write".

@pure fn get_it(p: *i64) -> i64 {
    return p[0];
}

fn main() {
    let p: *i64 = malloc(i64, 1);
    p[0] = 9;
    print_i64(get_it(p));
    println();
    free(p);
}
