// Phase MEM (§5.3): an `owned` parameter must be given by every caller.
fn consume(owned data: *u8) {
    free(data);
}
fn main() {
    let b: *u8 = malloc(u8, 16);
    consume(b);        // F0303: no give()
}
