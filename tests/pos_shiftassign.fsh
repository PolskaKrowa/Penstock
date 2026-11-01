// Phase 0d (report §3.4): <<= and >>= are documented (spec §5) and now
// lexed, parsed, type-checked and lowered end-to-end.
fn main() {
    let mut x: u64 = 1;
    x <<= 4;                      // 16
    print_u64(x); println();
    x >>= 2;                      // 4
    print_u64(x); println();
    let mut m: i64 = 256;
    m <<= 2;                      // 1024
    print_i64(m); println();
    m >>= 3;                      // 128
    print_i64(m); println();
    // signed right shift is arithmetic
    let mut n: i64 = 0 - 64;
    n >>= 2;                      // -16
    print_i64(n); println();
    // all other compound ops still work alongside
    let mut y: i64 = 10;
    y += 5; y -= 3; y *= 2;       // 24
    print_i64(y); println();
    let mut z: i64 = 100;
    z /= 4;                       // 25 (strength-reduced, signed-correct)
    print_i64(z); println();
    let mut w: i64 = 103;
    w %= 32;                      // 7 (signed-correct)
    print_i64(w); println();
}
