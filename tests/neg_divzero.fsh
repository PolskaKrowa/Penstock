// division by the literal zero is a CORRECTNESS error in every mode
// (report §7: "type mismatch ... vs performance" separation).
fn main() {
    let x: i64 = 100;
    let r: i64 = x / 0;      // F0214 always
    print_i64(r); println();
}
