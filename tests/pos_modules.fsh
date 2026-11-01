// Phase 3a (report §6.1/§6.2): modules, imports, pub visibility.
import geometry;
import util;

fn main() {
    let a: Point = Point { x: 0.0, y: 0.0 };
    let b: Point = geometry::scale(Point { x: 3.0, y: 4.0 }, 2.0);
    print_f64(distance(a, b), 2); println();            // 10
    print_f64(util::far(), 2); println();               // 7
    print_f64(geometry::visible(a), 2); println();      // 0
}
