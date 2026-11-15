// optimisation-audit §10.1 (mirror of neg_lane_merge_impure):
// a PURE, WILL-RETURN predicate is referentially transparent, so the
// merge is legal and still fires.
fn pure_helper(x: i64) -> i64 { return x + 1; }

fn main() {
    let n: usize = 32;
    let mut s: i64 = 0;
    for (i in 0..n) {
        if (pure_helper(2) > 0) { s += 1; }
        if (pure_helper(2) > 0) { s += 1; }
    }
    print_i64(s); println();    // 64
}
