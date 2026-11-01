// Stage B4 test: F0305 still fires when two pointer args provably
// alias and the parameter pair is NOT declared @alias.  This preserves
// the historical safety guarantee.

fn add_two(a: *f64, b: *f64) -> f64 {
  return a[0] + b[0];
}

fn main() {
  let x: *f64 = malloc(f64, 2);
  x[0] = 21.0;
  // No @alias declared; passing the same root is an F0305 error.
  let r: f64 = add_two(x, x);
  print_f64(r, 2); println();
  free(x);
}
