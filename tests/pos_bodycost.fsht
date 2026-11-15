// optimisation-audit §6.12 body-cost-aware parfor cutoff: the same 200
// iterations make DIFFERENT dispatch decisions based on the estimated
// per-iteration body work (cheap region runs inline, call-heavy region
// dispatches); results are identical either way.
fn heavy(x: i64) -> i64 { return x * 3 + 7 - 2 + 5; }

fn main() -> i32 {
    let mut s: i64 = 0;
    parfor (i in 0..200) [reduction(+:s)] { s = s + i; }
    print_i64(s); println();                          // 19900
    let mut t: i64 = 0;
    parfor (i in 0..200) [reduction(+:t)] { t = t + heavy(i as i64) + heavy((i+1) as i64) + heavy((i+2) as i64); }
    print_i64(t); println();                          // 186900
    return 0;
}
