// mem & vectorisation stage — mov-minimize (F0918):
//   * `let q: Vec3 = p;`  — whole-aggregate copy (lowers to a memcpy)
//   * `let u: f64 = t;`   — scalar mov (store+load pair)
//   * `let dead: f64 = u;` — zero uses, dead mov
//   * `x = x;`             — self-assignment (load+store back)
// --mov-minimize forwards the copies to their sources and drops the
// dead binding / self-assignment before the backend sees them.
struct Vec3 { x: f64, y: f64, z: f64 }
fn main() {
    let p: Vec3 = Vec3 { x: 1.0, y: 2.0, z: 3.0 };
    let q: Vec3 = p;                 // aggregate mov -> memcpy
    let t: f64 = 2.0;
    let u: f64 = t;                  // scalar mov
    let mut x: f64 = 1.5;
    x = x;                           // self-assignment
    let dead: f64 = u;               // never used
    print_f64(q.x + u + x, 2); println();
    print_f64(q.z, 2); println();
}
