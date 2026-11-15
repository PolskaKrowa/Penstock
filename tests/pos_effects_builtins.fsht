// audit §2.4 — runtime builtins inside functions considered for purity:
// freshet.threads() reads runtime state, soa_len / soa_field_ptr read
// the descriptor.  These must keep functions out of memory(none) and out
// of read-none groups; the caller must observe their results freshly.

struct P { x: f64, y: f64 }

@noinline fn nthreads() -> i64 {
    let t: i64 = freshet.threads() as i64;
    return t;
}

@noinline fn plen(a: soa(P)) -> i64 {
    return soa_len(a) as i64;
}

fn main() {
    let a: soa(P) = soa_new(P, 4);
    let n: i64 = plen(a);
    print_i64(n);
    print_i64(nthreads());
    println();
    soa_free(a);
}
