// 12B nested struct; 6th instance's field straddles a 64B line.
// report §7: F0404 is a warning by default and an error under
// --strict-performance (neg_straddle_strict.fsh).
struct T { a: u32, b: u32, c: u32 }         // 12B, align 4
struct S { t1: T, t2: T, t3: T, t4: T, t5: T, t6: T, t7: T, t8: T, t9: T, t10: T, t11: T, t12: T }
fn main() {
    let n: usize = 256;
    let arr: *S = malloc(S, n);
    parfor (i in 0..n) {       // t12 at offset 132: (132%64)+12 > 64
        arr[i].t6.a = 1;
    }
    free(arr);
}
