// optimisation-audit §4.3 (P0), regression per §10.1:
// full unrolling must veto a body containing `return`.  A constant-trip
// loop with an early return is not a copyable straight-line body; the
// clones after the first would follow a terminating block.
fn find(a: *i64, want: i64) -> i64 {
    for (i in 0..4) {                 // trip 4: full-unroll target
        if (a[i] == want) { return i as i64; }
    }
    return -1;
}

fn main() -> i32 {
    let a = malloc(i64, 4);
    a[0] = 5; a[1] = 7; a[2] = 9; a[3] = 11;
    print_i64(find(a, 9)); println();    // 2  (hit in the 3rd iteration)
    print_i64(find(a, 4)); println();    // -1 (fall-through return)
    free(a);
    return 0;
}
