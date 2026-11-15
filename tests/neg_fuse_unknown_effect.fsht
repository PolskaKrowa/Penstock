// optimisation-audit §4.6 (P1), regression per §10.1:
// fusion must be REJECTED when unknown effects may conflict.  The second
// loop writes through a CALL-PRODUCED pointer bound inside its own body —
// wild in the loop-local alias map, so the write cannot be placed and
// loop 2 carries unknownWrite while loop 1 writes `b`: an output
// dependence the historical test missed (it only checked
// unknown-write vs READ).
fn id_ptr(p: *i64) -> *i64 { return p; }

fn main() {
    let n: usize = 64;
    let b = malloc(i64, n);
    let a = malloc(i64, n);
    // loop 1: writes b, reads nothing
    for (i in 0..n) { b[0] = 5; }
    // loop 2: writes b AND an un-placeable write through the wild pointer
    for (i in 0..n) {
        let mut w: *i64 = id_ptr(a);
        b[0] = 5;
        w[0] = 3;
    }
    print_i64(b[0] + a[0]); println();
    free(b);
    free(a);
}
