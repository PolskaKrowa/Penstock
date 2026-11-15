// Phase 1d (report §4.3): enums, tagged unions, exhaustive match.
enum Color { Red, Green, Blue }

enum Shape {
    Circle(f64),
    Rect(f64, f64),
    Point,
}

fn area(s: Shape) -> f64 {
    match (s) {
        Circle(r) => { return 3.14159265358979 * r * r; }
        Rect(w, h) => { return w * h; }
        Point => { return 0.0; }
    }
}

fn name(c: Color) -> i64 {
    match (c) {
        Red => { return 10; }
        Green => { return 20; }
        Blue => { return 30; }
    }
}

fn main() {
    // simple enum: contextual construction + qualified spelling
    let c: Color = Red;
    print_i64(name(c)); println();                     // 10
    let d: Color = Color.Green;
    print_i64(name(d)); println();                     // 20

    // tagged unions with payloads
    let s: Shape = Circle(2.0);
    print_f64(area(s), 2); println();                  // 12.57
    let r: Shape = Rect(3.0, 4.0);
    print_f64(area(r), 1); println();                  // 12.0
    let p: Shape = Point;
    print_f64(area(p), 1); println();                  // 0.0

    // reassignment of enum values
    let mut x: Shape = Point;
    x = Rect(2.0, 5.0);
    print_f64(area(x), 1); println();                  // 10.0

    // match as a statement with wildcard arm
    let y: Shape = Circle(1.0);
    match (y) {
        Circle(_) => { print("circle"); println(); }
        _ => { print("other"); println(); }
    }

    // payload bindings usable in arm bodies
    match (r) {
        Rect(w, h) => {
            print_f64(w, 1); print(" "); print_f64(h, 1); println();  // 3.0 4.0
        }
        _ => { print("no"); println(); }
    }

    // enum in a data structure (by-value copy semantics)
    let mut arr: [Shape; 2] = [Circle(1.0), Point];
    arr[1] = Rect(1.0, 1.0);
    print_f64(area(arr[0]), 2); println();             // 3.14
    print_f64(area(arr[1]), 1); println();             // 1.0

    // enum equality is NOT defined (aggregate): use match
    let n: i64 = name(Blue);
    print_i64(n); println();                           // 30
}
