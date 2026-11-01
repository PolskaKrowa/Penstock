// Phase 1a (report §4.4): break/continue with correct defer semantics.
fn main() {
    // break leaves the loop early
    let mut s: i64 = 0;
    for (i in 0..100) {
        if (i == 5) { break; }
        s = s + i;
    }
    print_i64(s); println();                       // 10

    // continue skips the odd numbers
    let mut t: i64 = 0;
    for (i in 0..10) {
        if (i % 2 == 1) { continue; }
        t = t + i;
    }
    print_i64(t); println();                       // 20

    // break + continue in a while loop
    let mut w: i64 = 0;
    let mut k: i64 = 0;
    while (true) {
        k = k + 1;
        if (k > 10) { break; }
        if (k % 3 == 0) { continue; }
        w = w + k;
    }
    print_i64(w); println();                       // 1+2+4+5+7+8+10 = 37

    // nested loops: break/continue bind to the INNER loop
    let mut acc: i64 = 0;
    for (i in 0..3) {
        for (j in 0..3) {
            if (j == 2) { break; }
            acc = acc + 10 * i + j;
        }
    }
    print_i64(acc); println();                     // 0+1 + 10+11 + 20+21 = 63

    // C-style loop with a real step: i += 3 (F0405 only, still correct)
    let mut c: i64 = 0;
    for (i = 0; i < 10; i += 3) {
        c = c + 1;
    }
    print_i64(c); println();                       // 4 (0,3,6,9)

    // descending range: for (i in 10..0 by -1)
    let mut d: i64 = 0;
    for (i in 10..0 by -1) {
        d = d + i;
    }
    print_i64(d); println();                       // 10+9+...+1 = 55

    // continue in the C-style loop runs the step
    let mut e: i64 = 0;
    for (i = 0; i < 8; i += 1) {
        if (i < 4) { continue; }
        e = e + i;
    }
    print_i64(e); println();                       // 4+5+6+7 = 22
}
