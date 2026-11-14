// mem & vectorisation stage — mem-reuse must NOT fire (quality gate):
// buf[5] is read but never written anywhere in the body, so reuse would
// change which unspecified memory the read observes across iterations
// (fresh-malloc garbage vs last iteration's leftovers).  The read is
// made deterministic with `buf[5] ^ buf[5]` (always 0 for any bits).
fn main() {
    let n: i64 = 64;
    let mut s: i64 = 0;
    let mut k: i64 = 0;
    while (k < 4) {
        let buf: *i64 = malloc(i64, n);
        buf[0] = 7;
        s = s + buf[0];                 // covered: idx 0
        s = s + (buf[5] ^ buf[5]);      // idx 5 NEVER written -> veto
        free(buf);
        k = k + 1;
    }
    print_i64(s); println();
}
