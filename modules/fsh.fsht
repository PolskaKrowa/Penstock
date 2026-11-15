// fsh — the Freshet standard library module (Phase 3, report §6.3:
// "Publish a usable Freshet-facing libfsh module").
//
//   import fsh;
//
// This module is shipped with the compiler and found through the BUILTIN
// modules directory (no -I needed).  It exposes:
//
//   * program arguments       args_count / args_get / arg_or
//   * environment access      env_get / env_or
//   * file I/O                file_size / file_read / file_write
//   * test helpers            assert_eq_i64 / assert_eq_f64
//
// The C-level libfsh (include/fsh/*.h, built as libfsh.a) stays available
// through `extern fn` + `#link fsh` for everything not wrapped here —
// this module covers the surface programs need before linking anything
// extra.  str values are the { ptr, i64 } UTF-8 view; every path is
// NUL-terminated at the boundary.
module fsh;

// ---------------- runtime bindings (private) ----------------
extern fn frt_args_count() -> i64;
extern fn frt_args_get(i: i64) -> str;
extern fn frt_env_get(name: str) -> str;
extern fn frt_file_size(path: str) -> i64;
extern fn frt_file_read(path: str) -> str;
extern fn frt_file_write(path: str, data: str) -> i64;

// ---------------- program arguments (report §4.7) ----------------
pub fn args_count() -> i64 {
    return frt_args_count();
}

pub fn args_get(i: i64) -> str {
    return frt_args_get(i);
}

// argument i, or `d` when the index is out of range
pub fn arg_or(i: i64, d: str) -> str {
    if (i < 0 || i >= frt_args_count()) { return d; }
    return frt_args_get(i);
}

// ---------------- environment (report §4.7) ----------------
pub fn env_get(name: str) -> str {
    return frt_env_get(name);
}

pub fn env_or(name: str, d: str) -> str {
    let v: str = frt_env_get(name);
    if (v.len == 0) { return d; }
    return v;
}

// ---------------- files (report §4.7) ----------------
pub fn file_size(path: str) -> i64 {
    return frt_file_size(path);
}

// whole file as a str view over a FRESH heap buffer (free it with
// file_read_free when done); empty result means the file was missing
pub fn file_read(path: str) -> str {
    return frt_file_read(path);
}

pub fn file_write(path: str, data: str) -> i64 {
    return frt_file_write(path, data);
}

// ---------------- test helpers (report §10.3) ----------------
pub fn assert_eq_i64(got: i64, want: i64, what: str) -> i64 {
    if (got != want) {
        print("FAIL ");
        print(what);
        print(": expected ");
        print_i64(want);
        print(", got ");
        print_i64(got);
        println();
        return 1;
    }
    return 0;
}

pub fn assert_eq_f64(got: f64, want: f64, what: str) -> i64 {
    if (got != want) {
        print("FAIL ");
        print(what);
        print(": expected ");
        print_f64(want, 6);
        print(", got ");
        print_f64(got, 6);
        println();
        return 1;
    }
    return 0;
}
