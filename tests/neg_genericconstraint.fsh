// Phase 2a: constraint violation at instantiation.
fn clamp<T: Numeric>(x: T, low: T, high: T) -> T {
    if (x < low) { return low; }
    if (x > high) { return high; }
    return x;
}
fn main() {
    // bool is not Numeric — F0203 at the call site
    let r: bool = clamp(true, false, true);
    print_i64(r as i64); println();
}
