module geometry;

pub struct Point { x: f64, y: f64 }

pub fn distance(a: Point, b: Point) -> f64 {
    let dx: f64 = a.x - b.x;
    let dy: f64 = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

pub fn scale(p: Point, k: f64) -> Point {
    return Point { x: p.x * k, y: p.y * k };
}

fn hidden(p: Point) -> f64 {
    return p.x * p.y;
}

pub fn visible(p: Point) -> f64 {
    return hidden(p);
}
