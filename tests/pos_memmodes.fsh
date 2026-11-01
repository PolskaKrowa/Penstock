// Phase MEM (report §5.1/§5.3): owned/borrowed parameter modes + unsafe.
fn consume(owned data: *u8) {
    // ownership transferred IN: the callee may free it
    free(data);
}
fn inspect(borrowed data: Span<f64>) -> f64 {
    // a borrowed view: read-only, no free, no give
    let mut s: f64 = 0.0;
    for (i in 0..data.len) {
        s = s + data[i];
    }
    return s;
}
fn main() {
    // owned transfer at the call site
    let b: *u8 = malloc(u8, 16);
    consume(give(b));

    // borrowed spans carry no ownership duties
    let a: *f64 = malloc(f64, 3);
    a[0] = 1.0; a[1] = 2.0; a[2] = 3.0;
    print_f64(inspect(span(a, 3)), 1); println();     // 6.0
    // the caller still owns a and frees it normally
    free(a);

    // unsafe { }: an explicit scope for operations the compiler cannot
    // prove — here, indexing without checks in a checked build
    let arr: [i64; 2] = [5, 6];
    let v: Span<i64> = span(arr);
    unsafe {
        print_i64(v[0]); print(" "); print_i64(v[1]); println();   // 5 6
    }
    // @unsafe fn applies to the whole body (see unsafe_helper below)
    print_i64(unsafe_helper(span(arr))); println();   // 11
}
@unsafe fn unsafe_helper(v: Span<i64>) -> i64 {
    // whole-body unsafe scope: bounds checks are skipped here
    return v[0] + v[1];
}
