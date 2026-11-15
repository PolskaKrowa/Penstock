// optimisation-audit §6.1 TypedConstFold: exact-width folding, cast
// truncation, short-circuit without evaluating the unreachable operand,
// division never folded with a zero divisor.
fn boom(d: i64) -> i64 { return 1 / d; }

fn main() -> i32 {
    // typed wrapping arithmetic folded at source width
    let a: i64 = 2 * 3 + 4 - 1;            // 9
    print_i64(a); println();
    // narrowing cast truncates BEFORE folding: (300 as u8) == 44
    let b: u8 = (300 as u8);
    print_i64(b as i64); println();
    // short-circuit: `false && boom()` never evaluates the trap
    let mut c: i64 = 5;
    if (false && c > boom(0)) { c = c + (1); }
    print_i64(c); println();               // 5
    return 0;
}
