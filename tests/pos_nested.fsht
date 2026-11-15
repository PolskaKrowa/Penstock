struct V3 { x: f64, y: f64, z: f64 }
struct Cell align(64) { p: V3, v: V3, m: f64 }

fn energy(c: *Cell) -> f64 {
    return c.p.x * c.v.x + c.p.y * c.v.y + c.p.z * c.v.z + c.m;
}
fn main() {
    let c: Cell = Cell { p: V3 { x: 1.0, y: 2.0, z: 3.0 },
                         v: V3 { x: 0.5, y: 0.5, z: 0.5 }, m: 2.0 };
    print_f64(energy(&c), 2); println();
    let n: usize = 8;
    let cells: *Cell = malloc(Cell, n);
    parfor (i in 0..n) {
        cells[i].p.x = 1.0;
        cells[i].v.z = 2.0;
    }
    let mut t: f64 = 0.0;
    for (i in 0..n) { t = t + cells[i].p.x + cells[i].v.z; }
    print_f64(t, 2); println();
    free(cells);
}
