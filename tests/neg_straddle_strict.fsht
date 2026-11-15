// report §7: the same straddle under --strict-performance is an error (F0404).
struct T { a: u32, b: u32, c: u32 }
struct S { t1: T, t2: T, t3: T, t4: T, t5: T, t6: T, t7: T, t8: T, t9: T, t10: T, t11: T, t12: T }
fn main() {
    let n: usize = 256;
    let arr: *S = malloc(S, n);
    parfor (i in 0..n) {
        arr[i].t6.a = 1;
    }
    free(arr);
}
