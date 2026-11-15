// Phase 3 (report §6.3): the shipped Freshet-facing fsh module —
// args / env / file I/O without a single manual extern declaration.
import fsh;

fn main() {
    // program arguments (argv[0] = binary path)
    print("args: "); print_i64(fsh::args_count()); println();
    print("arg0 len: "); print_i64(fsh::args_get(0).len); println();
    print("arg9-or: "); print(fsh::arg_or(9, "<none>")); println();
    // environment
    print("missing-or: "); print(fsh::env_or("FRESHET_NOPE", "fallback")); println();
    // file I/O
    let wrote: i64 = fsh::file_write("/tmp/fsh_mod_io.txt", "hello freshet io");
    print("wrote: "); print_i64(wrote); println();
    let sz: i64 = fsh::file_size("/tmp/fsh_mod_io.txt");
    print("size: "); print_i64(sz); println();
    let data: str = fsh::file_read("/tmp/fsh_mod_io.txt");
    print("read: "); print(data); println();
    print("nosize: "); print_i64(fsh::file_size("/tmp/no_such_file_xyz")); println();
}
