// Phase 1b: constant out-of-bounds index on a fixed array is F0212.
fn main() {
    let a: [i64; 3] = [1, 2, 3];
    print_i64(a[5]); println();
}
