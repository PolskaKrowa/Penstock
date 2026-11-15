// Stage A1 test: @inline and @noinline attributes are accepted, the
// resulting program runs correctly, and the IR carries the new attribute
// groups #60 (noinline) and #61 (alwaysinline).

@inline fn small_helper(x: i64) -> i64 {
  return x * 2;
}

@noinline fn big_computation(x: i64) -> i64 {
  let mut acc: i64 = 0;
  for (i in 0..x) {
    acc = acc + i;
  }
  return acc;
}

fn main() {
  let a: i64 = small_helper(7);   // 14
  let b: i64 = big_computation(10);  // 0+1+...+9 = 45
  // total = 14 + 45 = 59, but we want a clean 42 → subtract 17
  let r: i64 = a + b - 17;
  print_i64(r); println();
}
