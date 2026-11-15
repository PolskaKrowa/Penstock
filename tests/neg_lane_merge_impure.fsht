// optimisation-audit §4.1 (P0), regression per §10.1:
// predicate merging must NOT remove an observable call.  next_event()
// writes a mutable global; the merged form would evaluate it once per
// iteration instead of twice, halving the observable call count.
let mut g_calls: i64 = 0;

@noinline fn next_event() -> bool {
    g_calls = g_calls + 1;
    return true;
}

fn main() {
    let n: usize = 8;
    let a = malloc(i64, n);
    for (i in 0..n) {
        if (next_event()) { a[i] = 1; }
        if (next_event()) { a[i] = a[i] + 2; }
    }
    // 2 calls per iteration x 8 iterations = 16 (8 would mean the pass
    // illegally merged the impure predicates)
    print_i64(g_calls); println();
    free(a);
}
