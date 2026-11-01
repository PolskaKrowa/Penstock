// Phase 2a (report §4.2): generics via monomorphisation.
fn clamp<T: Numeric>(x: T, low: T, high: T) -> T {
    if (x < low) { return low; }
    if (x > high) { return high; }
    return x;
}

struct Pair<T, U> { a: T, b: U }

fn first<T, U>(p: Pair<T, U>) -> T {
    return p.a;
}

fn span_sum<T>(v: Span<T>) -> T {
    // inference from Span<T>: T comes from the element type
    let mut s: T = 0 as T;
    for (i in 0..v.len) {
        s = s + v[i];
    }
    return s;
}

fn pick<T>(cond: bool, a: T, b: T) -> T {
    if (cond) { return a; }
    return b;
}

fn main() {
    // one generic fn, several instantiations
    print_f64(clamp(2.5, 1.0, 2.0), 1); println();      // clamp$f64: 2.0
    print_i64(clamp(9 as i64, 1 as i64, 5 as i64)); println();  // clamp$i64: 5
    print_f64(clamp(0.5f32, 1.0f32, 2.0f32) as f64, 1); println(); // clamp$f32: 1.0

    // generic struct
    let p: Pair<f64, i64> = Pair { a: 1.5, b: 7 };
    print_f64(p.a, 1); print(" "); print_i64(p.b); println();   // 1.5 7
    let q: Pair<i64, f64> = Pair { a: 3, b: 2.5 };
    print_i64(first(q)); print(" ");
    print_f64(first(p), 1); println();                          // 3 1.5

    // generic over a Span's element type
    let arr: [i64; 4] = [1, 2, 3, 4];
    let v: Span<i64> = span(arr);
    print_i64(span_sum(v)); println();                          // 10
    let farr: [f64; 3] = [1.5, 2.5, 3.5];
    print_f64(span_sum(span(farr)), 1); println();              // 7.5

    // generic over struct types
    let ps: Pair<Pair<i64, i64>, f64> = Pair { a: Pair { a: 1, b: 2 }, b: 9.5 };
    print_i64(first(ps.a)); println();                          // 1

    // bool-typed selection
    print_i64(pick(true, 10 as i64, 20 as i64)); println();     // 10
    print_f64(pick(false, 1.5, 2.5), 1); println();             // 2.5
}
