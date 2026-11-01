// audit §2.4 — reads through a REBOUND / aliased local pointer
// (`let mut q = p; q[j] = ...`), which the historical purity walk missed
// (it only tracked writes through direct parameter chains).

@noinline fn fill(p: *i64, n: i64) -> i64 {
    let mut q: *i64 = p;      // rebound pointer
    for (i in 0..n) {
        q[i] = i as i64;
    }
    return q[n - 1];
}

@noinline fn read_via_alias(p: *i64) -> i64 {
    let q: *i64 = p;          // read-only alias of the parameter
    return q[0];
}

fn main() {
    let p: *i64 = malloc(i64, 8);
    let last: i64 = fill(p, 8);
    let x: i64 = read_via_alias(p);
    p[0] = 100;
    let y: i64 = read_via_alias(p);
    print_i64(last); print_i64(x); print_i64(y);
    println();
    free(p);
}
