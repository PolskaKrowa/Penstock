// optimisation-audit §6.4 FreshetLICM: the invariant pure call is
// computed once, the loop-variant use still updates per iteration.
fn pure_helper(x: i64) -> i64 { return x * 3 - 1; }

fn main() -> i32 {
    let mut s: i64 = 0;
    for (i in 0..30) {
        let t: i64 = pure_helper(7);
        let u: i64 = t + i;
        s = s + (u);
    }
    print_i64(s); println();    // 30*20 + sum(0..29) = 1035
    return 0;
}
