// Phase 3b (report §6.3): fn pointers + C callbacks (libc qsort).
fn add(a: i64, b: i64) -> i64 { return a + b; }
fn mul(a: i64, b: i64) -> i64 { return a * b; }

fn apply_twice(f: fn(i64, i64) -> i64, x: i64) -> i64 {
    return f(f(x, x), x);
}

extern fn qsort(base: *u8, n: usize, sz: usize, cmp: fn(*u8, *u8) -> i32);

fn cmp_desc(a: *u8, b: *u8) -> i32 {
    let pa: *i64 = a as *i64;
    let pb: *i64 = b as *i64;
    let va: i64 = pa[0];
    let vb: i64 = pb[0];
    if (va > vb) { return -1; }
    if (va < vb) { return 1; }
    return 0;
}

fn main() {
    let mut f: fn(i64, i64) -> i64 = add;
    print_i64(f(2, 3)); println();              // 5
    f = mul;
    print_i64(f(4, 5)); println();              // 20
    print_i64(apply_twice(add, 3)); println();  // 9
    print_i64(apply_twice(mul, 2)); println();  // 8
    let n: usize = 6;
    let d: *i64 = malloc(i64, n);
    d[0] = 5; d[1] = 1; d[2] = 9; d[3] = 3; d[4] = 7; d[5] = 2;
    qsort(d as *u8, n, sizeof(i64), cmp_desc);
    for (i in 0..n) { print_i64(d[i]); print(" "); }
    println();                                   // 9 7 5 3 2 1
    free(d);
}
