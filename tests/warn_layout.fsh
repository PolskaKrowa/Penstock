struct Bad { a: u8, x: f64, b: u8, y: f64 }   // 14B padding -> F0407
fn main() {
    let p: Bad = Bad { a: 1, x: 1.0, b: 2, y: 2.0 };
    print_f64(p.x + p.y, 2); println();
}
