// Phase 4 (report §10.3): @test functions run by `penstock test`.
fn add(a: i64, b: i64) -> i64 { return a + b; }

@test
fn add_works() {
    assert(add(2, 3) == 5);
    assert(add(-1, 1) == 0);
}

@test
fn string_len() {
    let s: str = "hello";
    assert(s.len == 5);
}

@test
fn runs_last() {
    assert(add(10, 10) == 20);
}
