// audit §2.4 — a PURE-BUT-POSSIBLY-NONTERMINATING function: side-effect
// free, but the `while` loop has no bound.  IR inspection test: this
// function must NOT receive `willreturn` (a side-effect-free loop can
// still run forever — willreturn needs a termination PROOF, never an
// inference from effect freedom).

fn spin(n: i64) -> i64 {
    let mut i: i64 = 0;
    while (i < n) {
        i = i + 2;
        if (i > 1000000) { i = 0; }
    }
    return i;
}

fn main() {
    print_i64(spin(4));
    println();
}
