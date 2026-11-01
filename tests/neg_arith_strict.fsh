// report §7: under `--strict-performance` (or @hpc_strict) the perf
// diagnostics are hard errors again — the HPC workflow keeps its gates.
fn main() {
    let d: i64 = 0;
    let x: i64 = 100;
    let r: i64 = x / d;      // runtime divisor: F0402 under strict mode
    print_i64(r); println();
}
