// optimisation-audit §6.7: imin/imax integer builtins (icmp + select,
// signedness-aware).
fn main() -> i32 {
    print_i64(imin(3, 7)); print(" ");
    print_i64(imax(3, 7)); print(" ");
    print_i64(imin(-5, 2)); print(" ");
    print_i64(imax(-5, 2)); println();     // 3 7 -5 2
    print_u64(imin(9, 4)); print(" ");
    print_u64(imax(9, 4)); println();      // 4 9
    return 0;
}
