// Phase 2b (report §4.5): str views + owned Strings.
struct Named { name: str, value: i64 }

fn greet(name: str) -> str {
    // slicing + concatenation on views
    let mut s: String = string_from("hello, ");
    string_append(&s, name);
    string_append(&s, "!");
    return str_of(&s);
}
fn main() {
    // str literals are values now, not just print arguments
    let greeting: str = "hello";
    print(greeting); println();                        // hello
    print_i64(greeting.len); println();                // 5

    // byte indexing (documented: s[i] is the i-th BYTE)
    let abc: str = "ABC";
    print_i64(abc[0] as i64); print(" ");
    print_i64(abc[2] as i64); println();               // 65 67

    // slicing produces views
    let s: str = "hello, world";
    let w: str = s[7..12];
    print(w); println();                               // world
    print_i64(w.len); println();                       // 5

    // comparison
    let a: str = "foo";
    let b: str = "foo";
    let c: str = "bar";
    if (str_eq(a, b)) { print("eq"); } else { print("ne"); }
    print(" ");
    if (str_eq(a, c)) { print("eq"); } else { print("ne"); }
    println();                                         // eq ne

    // owned Strings: build, view, free
    let mut out: String = string_new();
    string_append(&out, "abc");
    string_append(&out, "def");
    print(str_of(&out)); println();                    // abcdef
    print_i64(out.len); println();                     // 6

    // concatenation returns an owned String
    let full: String = str_cat("count: ", "42");
    print(str_of(&full)); println();                   // count: 42
    string_free(&full);

    // formatting numbers
    let n: String = fmt_i64(12345);
    print(str_of(&n)); println();                      // 12345
    let f: String = fmt_f64(3.14159, 2);
    print(str_of(&f)); println();                      // 3.14
    string_free(&n);
    string_free(&f);

    // strings through functions (by-value pair aggregates)
    print(greet("freshet")); println();                // hello, freshet!

    // C interop: NUL-terminated copy (caller frees)
    let cz: *u8 = str_to_cstr("abc");
    free(cz);

    // str in a struct field
    let nt: Named = Named { name: "pi", value: 3 };
    print(nt.name); print(" "); print_i64(nt.value); println();   // pi 3
}
