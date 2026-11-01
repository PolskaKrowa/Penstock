// Phase 0b: valueless `return;` in a non-void function is a hard error
// (the historical warning let IRgen emit an ill-typed `ret void`).
fn f(x: i64) -> i64 {
    if (x > 0) { return 1; }
    return;
}
fn main() { print_i64(f(1)); println(); }
