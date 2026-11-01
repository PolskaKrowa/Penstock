// Phase 1b (report §4.1): fixed-size arrays [T; N].
struct P { x: f64, y: f64 }

fn sum_arr(a: [i64; 4]) -> i64 {
    // by-value parameter: the callee sees its own copy
    let mut s: i64 = 0;
    for (i in 0..a.len) {
        s = s + a[i];
    }
    return s;
}
fn main() {
    let a: [i64; 4] = [1, 2, 3, 4];
    print_i64(a[0]); print(" "); print_i64(a[3]); println();     // 1 4
    print_i64(a.len); println();                                  // 4
    // mutation
    let mut b: [i64; 4] = [10, 20, 30, 40];
    b[1] = 99;
    print_i64(b[1]); println();                                   // 99
    // copies are values: modifying the callee's copy leaves ours alone
    let c: [i64; 4] = [5, 6, 7, 8];
    print_i64(sum_arr(c)); println();                             // 26
    print_i64(c[0]); println();                                   // 5
    // sizeof reflects N * sizeof(T)
    let sz: i64 = sizeof([f64; 8]);
    print_i64(sz); println();                                     // 64
    // assignment copies
    let mut d: [i64; 2] = [1, 2];
    let mut e: [i64; 2] = [0, 0];
    e = d;
    d[0] = 100;
    print_i64(e[0]); print(" "); print_i64(d[0]); println();      // 1 100
    // nested: array of arrays
    let m: [[i64; 2]; 2] = [[1, 2], [3, 4]];
    print_i64(m[1][0]); println();                                // 3
    // array of structs
    let ps: [P; 2] = [P { x: 1.0, y: 2.0 }, P { x: 3.0, y: 4.0 }];
    print_f64(ps[1].y, 1); println();                             // 4.0
    // inferred element adoption against the annotation
    let f: [i32; 3] = [7, 8, 9];
    print_i64(f[2] as i64); println();                            // 9
}
