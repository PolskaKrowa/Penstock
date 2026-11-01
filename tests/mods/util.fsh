module util;

// same NAME as geometry::distance — private here, no collision
fn distance(a: f64, b: f64) -> f64 {
    if (a < b) { return b - a; }
    return a - b;
}

pub fn far() -> f64 {
    return distance(3.0, 10.0);
}
