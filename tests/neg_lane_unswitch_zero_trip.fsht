// optimisation-audit §4.2 (P0), regression per §10.1:
// loop unswitching must not evaluate a condition on a ZERO-TRIP loop.
// The range is empty (n = 0), so the source never evaluates `1 / d`;
// an unsound hoist would divide by zero and abort.  The arms carry
// enough lane work that the cost model alone would NOT veto the hoist —
// only the zero-trip legality gate rejects it.
fn main() {
    let n: usize = 0;
    let d: i64 = 0;
    let mut s: i64 = 0;
    for (i in 0..n) {
        if (1 / d > 0) {
            s = s + (1); s = s + (2); s = s + (3); s = s + (4); s = s + (5); s = s + (6);
        } else {
            s = s + (7); s = s + (8); s = s + (9); s = s + (10); s = s + (11); s = s + (12);
        }
    }
    print_i64(s); println();   // 0 — the loop body never ran, no trap
}
