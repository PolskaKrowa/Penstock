// Phase 2c (report §4.6): Option<T> / Result<T, E> + `?` propagation.
fn safe_div(a: i64, b: i64) -> Result<i64, i64> {
    if (b == 0) {
        return Err(1001);
    }
    return Ok(a / b);
}

fn find_first(v: Span<i64>, want: i64) -> Option<i64> {
    for (i in 0..v.len) {
        if (v[i] == want) {
            return Some(i);
        }
    }
    return None;
}

fn checked_computation(a: i64, b: i64) -> Result<i64, i64> {
    // `?` propagates the error early; the value flows on
    let q: i64 = safe_div(a, b)?;
    let d: i64 = safe_div(q, 2)?;
    return Ok(d + 1);
}

fn maybe_double(o: Option<i64>) -> Option<i64> {
    let v: i64 = o?;     // None propagates as None
    return Some(v * 2);
}

fn main() {
    // Ok / Err with payload extraction via match
    let r: Result<i64, i64> = safe_div(10, 2);
    match (r) {
        Ok(v) => { print_i64(v); println(); }          // 5
        Err(e) => { print_i64(0 - e); println(); }
    }
    let r2: Result<i64, i64> = safe_div(1, 0);
    match (r2) {
        Ok(v) => { print_i64(v); println(); }
        Err(e) => { print_i64(e); println(); }         // 1001
    }

    // Option
    let arr: [i64; 4] = [10, 20, 30, 40];
    let found: Option<i64> = find_first(span(arr), 30);
    match (found) {
        Some(idx) => { print_i64(idx); println(); }    // 2
        None => { print("none"); println(); }
    }
    let missing: Option<i64> = find_first(span(arr), 99);
    match (missing) {
        Some(idx) => { print_i64(idx); println(); }
        None => { print("none"); println(); }          // none
    }

    // ? chains: 100 / 2 / 2 + 1
    match (checked_computation(100, 2)) {
        Ok(v) => { print_i64(v); println(); }          // 26
        Err(e) => { print_i64(e); println(); }
    }
    match (checked_computation(100, 0)) {
        Ok(v) => { print_i64(v); println(); }
        Err(e) => { print_i64(e); println(); }         // 1001
    }

    // Option propagation
    match (maybe_double(Some(21))) {
        Some(v) => { print_i64(v); println(); }        // 42
        None => { print("none"); println(); }
    }
    match (maybe_double(None)) {
        Some(v) => { print_i64(v); println(); }
        None => { print("none"); println(); }          // none
    }

    // qualified construction and Option<i64> in a variable
    let o: Option<i64> = Option.Some(7);
    match (o) {
        Some(v) => { print_i64(v); println(); }        // 7
        None => { print("none"); println(); }
    }
}
