// Stage R1+R2 test: exercise the ring-buffer scheduler under nested
// parfor + concurrent timing.  Output must be deterministic (sum is
// order-independent) so any race-free scheduler produces the same value.
// (Syntax refreshed for the current `[reduction(+:acc)]` clause form —
// the file predates it and had stopped parsing.)

fn main() {
  let n: i64 = 1000;
  let outer: i64 = 100;
  let mut acc: f64 = 0.0;
  let a: *f64 = malloc(f64, n);

  // initialise
  parfor (i in 0..n) {
    a[i] = 1.0;
  }

  // outer sequential, inner parallel: 100 nested parfor regions, each
  // touching 1000 elements.  This is exactly the workload the ring
  // buffer + per-worker-local timing must handle without racing.
  for (k in 0..outer) {
    parfor (i in 0..n) [reduction(+:acc)] {
      acc = acc + a[i];
    }
  }

  free(a);
  // acc = 100 * 1000 * 1.0 = 100000
  print_f64(acc, 2); println();
}
