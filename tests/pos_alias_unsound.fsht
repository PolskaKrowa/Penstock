// Stage B4/B5 test: UNSOUND @alias — caller passes provably-aliasing
// memory.  The compiler must emit F0306 and SUPPRESS the noalias
// attribute on the lying parameter pair.

@alias(a, b) fn add_two(a: *f64, b: *f64) -> f64 {
  return a[0] + b[0];
}

fn main() {
  let x: *f64 = malloc(f64, 2);
  x[0] = 21.0;
  // PASSING THE SAME ROOT: this violates the @alias(a, b) no-overlap
  // promise.  The strict checker must emit F0306; IRgen must NOT
  // emit noalias on the add_two signature.
  let r: f64 = add_two(x, x);
  print_f64(r, 2); println();   // 42.00
  free(x);
}
