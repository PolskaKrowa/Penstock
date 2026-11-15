fn classify(v: f64) -> u64 {
    if v > 0.5 {
        if v > 0.9 { return 2; }
        return 1;
    }
    return 0;
}
fn main() {
    let mut hist: u64 = 0;
    let n: i64 = 4000000;
    let mut seed: u64 = 12345;
    for (i in 0..n) {
        seed = seed * 6364136223846793005 + 1442695040888963407;
        let r: f64 = (seed >> 11) as f64 / 9007199254740992.0;
        hist = hist + classify(r);
    }
    print_u64(hist); println();
}
