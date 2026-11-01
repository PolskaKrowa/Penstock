// audit §2.2 reproducer — pointer-reading helper called twice around a
// store.  Before the P0 fix, u_read_first was stamped
// `memory(none) willreturn` (read-none) because the boolean purity walk
// only proved "no writes"; at -O3 the backend reused the first call's
// result and printed "00" instead of "12".

@noinline fn read_first(p: *i64) -> i64 {
    return p[0];
}

fn main() {
    let p: *i64 = malloc(i64, 1);
    p[0] = 1;
    let x: i64 = read_first(p);
    p[0] = 2;
    let y: i64 = read_first(p);
    print_i64(x);
    print_i64(y);
    println();
    free(p);
}
