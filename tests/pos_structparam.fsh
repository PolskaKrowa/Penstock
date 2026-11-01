// report §3.2 follow-on: by-value struct parameters. The historical
// definition/call type mismatch silently miscompiled these (sum2(P{1.5,
// 2.5}) returned 1.5 instead of 4.0).
struct P { x: f64, y: f64 }
fn sum2(p: P) -> f64 {
    return p.x + p.y;
}
fn modify(p: P) -> P {
    // the callee sees its own COPY: the caller's value is untouched
    let mut q: P = p;
    q.x = 100.0;
    return q;
}
fn main() {
    let a = P { x: 1.5, y: 2.5 };
    print_f64(sum2(a), 2); println();                 // 4.00
    let b = modify(a);
    print_f64(b.x, 1); print(" ");                    // 100.0
    print_f64(a.x, 1); println();                     // 1.5 (unchanged)
    // struct-returning functions
    let c: P = modify(a);
    print_f64(c.y, 1); println();                     // 2.5
}
