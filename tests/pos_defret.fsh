// Phase 0b: every accepted definite-return shape.
fn both(x: i64) -> i64 {
    if (x > 0) { return 1; } else { return 2; }
}
fn trailing(x: i64) -> i64 {
    if (x > 0) { return 1; }
    return 2;
}
fn loops_out(x: i64) -> i64 {
    let mut y: i64 = x;
    while (true) {
        y = y + 1;
        if (y > 5) { return y; }
    }
}
fn early(x: i64) -> i64 {
    if (x == 0) { return 0; }
    let y: i64 = x * 2;
    return y;
}
fn main() {
    print_i64(both(1) + both(0 - 1)); println();      // 3
    print_i64(trailing(5) + trailing(0 - 5)); println();  // 3
    print_i64(loops_out(0)); println();               // 6
    print_i64(early(0) + early(3)); println();        // 6
}
