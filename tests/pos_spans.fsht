// Phase 1c (report §4.1): Span<T> — borrowed (ptr, len) views.
struct P { x: f64, y: f64 }

fn sum_span(values: Span<f64>) -> f64 {
    let mut total: f64 = 0.0;
    for (i in 0..values.len) {
        total = total + values[i];
    }
    return total;
}
fn fill(v: Span<i64>) {
    // mutation through a borrowed view
    for (i in 0..v.len) {
        v[i] = i as i64 * 10;
    }
}
fn main() {
    // from a raw pointer + length
    let a: *f64 = malloc(f64, 4);
    a[0] = 1.0; a[1] = 2.0; a[2] = 3.0; a[3] = 4.0;
    let s: Span<f64> = span(a, 4);
    print_f64(sum_span(s), 1); println();              // 10.0
    print_i64(s.len); println();                       // 4

    // from a fixed array (no copy)
    let arr: [i64; 5] = [1, 2, 3, 4, 5];
    let v: Span<i64> = span(arr);
    let mut sum: i64 = 0;
    for (i in 0..v.len) { sum = sum + v[i]; }
    print_i64(sum); println();                         // 15

    // mutation through the view writes the array
    fill(v);
    print_i64(arr[2]); println();                      // 20

    // slicing: v[a..b] is a sub-view, no copy
    let sub: Span<i64> = v[1..4];
    print_i64(sub.len); print(" "); print_i64(sub[0]); print(" ");
    print_i64(sub[2]); println();                      // 3 20 40

    // array slicing
    let tail: Span<i64> = arr[3..5];
    print_i64(tail[0]); print(" "); print_i64(tail[1]); println();   // 40 50

    // .ptr hands the raw pointer to FFI-style code
    let p: *i64 = v.ptr;
    print_i64(p[4]); println();                        // 40

    // a Span of structs
    let ps: *P = malloc(P, 2);
    ps[0] = P { x: 1.5, y: 2.5 };
    ps[1] = P { x: 3.5, y: 4.5 };
    let pv: Span<P> = span(ps, 2);
    print_f64(pv[1].x, 1); println();                  // 3.5

    // spans as function VALUES (by-value pair aggregate)
    print_f64(sum_span(span(a, 2)), 1); println();     // 3.0
    free(a);
    free(ps);
}
