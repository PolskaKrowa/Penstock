// audit §2.4 — helper reads a MUTABLE GLOBAL between two writes; the
// effect summary must classify the reader as touching "other" memory
// (memory(read)), never memory(none), or -O3 could reuse a stale value.

let mut g_counter: i64 = 0;

@noinline fn bump() -> i64 {
    g_counter = g_counter + 1;
    return g_counter;
}

@noinline fn read_counter() -> i64 {
    return g_counter;
}

fn main() {
    let a: i64 = read_counter();
    bump();
    let b: i64 = read_counter();
    bump();
    let c: i64 = read_counter();
    print_i64(a); print_i64(b); print_i64(c);
    println();
}
