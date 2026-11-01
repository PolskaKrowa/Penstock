// optimisation-audit §10.1: a simple small constant-trip loop still
// fully unrolls after the §4.3/§4.4 hardening.
fn main() {
    let mut s: i64 = 0;
    for (i in 0..8) { s = s + (i); }
    print_i64(s); println();    // 28
}
