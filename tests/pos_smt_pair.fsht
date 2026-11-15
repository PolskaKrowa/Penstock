// mem & vectorisation stage — SMT memory/compute interleave (F0922).
// Three adjacent parfor regions:
//   1. init   (writes idx/src/tmp)  — not pairable with A (A reads idx)
//   2. A      gather out[i] = src[idx[i]]  — memory-bound (3 mem / 0 ops)
//   3. B      10-op arithmetic chain on tmp — compute-bound
// A and B are fully independent and complementary, so --smt-interleave
// pairs them into ONE frt_parfor_pair dispatch: the runtime runs A's
// chunks on the memory lane and B's on the compute lane, so on an SMT
// core one hardware thread streams memory while its sibling keeps the
// shared ALU busy.
fn main() {
    let n: usize = 1000000;
    let src: *f64 = malloc(f64, n);
    let idx: *usize = malloc(usize, n);
    let out: *f64 = malloc(f64, n);
    let tmp: *f64 = malloc(f64, n);
    parfor (i in 0..n) {
        idx[i] = (i * 7919) % 1048576;
        src[i] = (i as f64) + 1.0;
        tmp[i] = (i as f64) * 0.5;
    }
    // A: memory-bound gather (reads src + idx, writes out)
    parfor (i in 0..n) {
        out[i] = src[idx[i]];
    }
    // B: compute-bound arithmetic chain (reads/writes tmp)
    parfor (i in 0..n) {
        let mut a: f64 = tmp[i];
        a = a * 1.000001 + 0.1;
        a = a * a + 0.2;
        a = a * a + 0.3;
        a = a * a + 0.4;
        a = a * 1.000001 + 0.5;
        tmp[i] = a;
    }
    print_f64(out[0] + tmp[0], 2); println();
    free(src);
    free(idx);
    free(out);
    free(tmp);
}
