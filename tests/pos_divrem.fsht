// Phase 0a regression (report §3.1): signed power-of-two division and
// remainder must truncate toward zero. The historical strength reduction
// lowered `x / 2^k` to `lshr` and `x % 2^k` to `and`, which produced a
// huge positive quotient and a wrong-sign remainder for negative
// dividends. Every value printed here is checked against C semantics
// (trunc(x/d), sign of remainder = sign of dividend).
fn main() {
    // i64: negative dividends, divisors 1, 2, 4, 8, 16
    let a: i64 = 0 - 7;
    print_i64(a / 2); print(" "); print_i64(a % 8); println();        // -3 -7
    let b: i64 = 0 - 9;
    print_i64(b / 8); print(" "); print_i64(b % 8); println();        // -1 -1
    let c: i64 = 0 - 16;
    print_i64(c / 16); print(" "); print_i64(c % 16); println();      // -1 0
    print_i64(a / 1); print(" "); print_i64(a % 1); println();        // -7 0
    // exact negatives of powers of two
    let d: i64 = 0 - 8;
    print_i64(d / 4); print(" "); print_i64(d % 4); println();        // -2 0
    // positive controls
    print_i64(7 / 2); print(" "); print_i64(7 % 8); println();        // 3 7

    // i32 width: the bias sequence must use the OPERAND width (W-1 = 31)
    let e: i32 = 0 - 100;
    print_i64((e / 4) as i64); print(" "); print_i64((e % 32) as i64); println();  // -25 -4

    // i16 / i8 narrow widths
    let f: i16 = 0 - 300;
    print_i64((f / 8) as i64); print(" "); print_i64((f % 256) as i64); println(); // -37 -44
    let g: i8 = 0 - 100;
    print_i64((g / 2) as i64); print(" "); print_i64((g % 64) as i64); println();  // -50 -36

    // unsigned stays logical-shift / mask semantics
    let u: u64 = 18446744073709551615;    // 2^64 - 1
    print_u64(u / 2); println();                           // 9223372036854775807
    print_u64(u % 16); println();                          // 15
    let u8v: u8 = 250;
    print_i64((u8v / 2) as i64); print(" "); print_i64((u8v % 4) as i64); println(); // 125 2

    // consistency inside expressions (not just statement level)
    let mut acc: i64 = 0;
    for (i in 0..8) {
        let v: i64 = i as i64 * 2 - 9;             // -9,-7,-5,-3,-1,1,3,5
        acc = acc + v / 2 * 4 + v % 4;      // division/remainder composed
    }
    print_i64(acc); println();              // C reference: -32

    // constant-folded negatives (unary minus on literal dividends)
    let n: i64 = -7;
    print_i64(n / 2); print(" "); print_i64(n % 8); println();        // -3 -7
}
