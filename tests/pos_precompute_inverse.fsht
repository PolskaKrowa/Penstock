// Small finite-domain inverse search. The runtime function scans for the
// input whose deterministic u8 transform equals the requested output. At -O3,
// the precomputation pass can replace this search with a complete target->input
// table after the whole-module rewrite is proved by Alive2.
@pure fn scramble(x: u8) -> u8 {
    return ((((((((x as u64 + 13) * 7) ^ 91) + 53) * 3) ^ 17) + 9) * 11 ^ 77) as u8;
}

@pure fn find_input(target: u8) -> u64 {
    let mut x: u64 = 0;
    while (x < 256) {
        if (scramble(x as u8) == target) {
            return x;
        }
        x = x + 1;
    }
    return 256;
}

fn main() {
    // This transform is bijective over u8, so the preimage must be 42.
    print_u64(find_input(scramble(42u8)));
    println();
}
