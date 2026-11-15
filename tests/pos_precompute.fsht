// Small finite-domain pure integer function used by the -O3 lookup rewrite.
// A complete u8 input map is only 256 entries; the result checksum exercises
// every possible input and catches table packing/order mistakes.
@pure fn scramble(x: u8) -> u64 {
    return ((((((((x as u64 + 13) * 7) ^ 91) + 53) * 3) ^ 17) + 9) * 11) ^ 77;
}
fn main() {
    let mut sum: u64 = 0;
    for (i in 0..256) {
        sum = sum + scramble(i as u8);
    }
    print_u64(sum);
    println();
}
