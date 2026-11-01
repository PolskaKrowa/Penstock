struct P { x: f64, y: f64 }
fn main() {
    let p: P = P { x: 1.0 };   // missing field y: F0201
    print_f64(p.x, 2); println();
}
