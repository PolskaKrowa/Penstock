// Stage B4/B5 test: SOUND @alias — caller passes two distinct roots.
// The compiler must emit `noalias` parameter attributes on both `a` and
// `b` because every caller in this TU keeps the no-overlap promise.

@alias(a, b) fn add_two(a: *f64, b: *f64) -> f64 {
  return a[0] + b[0];
}

fn main() {
  let x: *f64 = malloc(f64, 2);
  let y: *f64 = malloc(f64, 2);
  x[0] = 10.0;
  y[0] = 32.0;
  let r: f64 = add_two(x, y);
  print_f64(r, 2); println();   // 42.00
  free(x);
  free(y);
}
