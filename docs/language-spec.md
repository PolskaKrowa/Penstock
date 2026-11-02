# Freshet Language Specification — Penstock 0.1.0-INDEV

Freshet is a strictly-typed, strictly-checked HPC language. C++/Rust-like
surface syntax, C-level control over data layout, and a compiler that
treats predictable performance bugs as compile errors.

```
penstock [options] file.fsh
  -o <file>   -S (emit .ll)  -c (emit .o)  -O<0-4>  -march=<t>
  --run  --report  --profile  --use-profile=<f>  --keep-ir
  --threads=<n>  --soa=warn|auto|aosoa  -I<dir>  --no-loop-opts
```

## 1. Lexical structure

Line comments `//`, block comments `/* */`. Identifiers are C-like.
Numeric literals carry optional suffixes (`42u8`, `1.5f32`, `0x1F`). The
preprocessor supports `#include "file"`, `#include <file>` (searches `-I`
dirs), `#define NAME body`, object and function-like macros, `#link m`,
`#pragma once`, `#ifdef/#ifndef/#else/#endif`. String literals exist for
`print`.

## 2. Types

| scalar | bits |
|---|---|
| `u8 u16 u32 u64 usize` | 8/16/32/64 unsigned |
| `i8 i16 i32 i64` | signed |
| `f32 f64` | IEEE binary32/64 |
| `bool` | 1 byte |
| `*T` | pointer |
| `T` (struct name) | struct value |
| `soa(S)` | SoA descriptor handle (planes of S's fields) |
| `[T; N]` | fixed-size array — the length is part of the type |
| `Span<T>` | borrowed `(ptr, len)` view over contiguous `T` |
| `str` | immutable UTF-8 string view (byte-indexed) |
| enum name / `Option<T>` / `Result<T, E>` | tagged unions (§4c) |

`usize` and `u64` are distinct spellings with identical representation.
Implicit conversions: literal type adoption, widening int→int of same
signedness, f32→f64, literal → annotated type. Everything else requires an
explicit `expr as T` cast; narrowing casts warn (`F0211`); ptr↔number
casts are forbidden (`F0201`).

## 3. Declarations

```
let x: f64 = 1.0;             // immutable binding
let mut acc: i64 = 0;         // mutable binding
fn name(a: f64, b: *f64) -> f64 { ... }
fn clamp<T: Numeric>(x: T, low: T, high: T) -> T { ... }   // generic (§4d)
export fn name(...)           // extern linkage
@pure fn f(...)               // no side effects: CSE/licm enabled
@strictfp fn f(...)           // force strict IEEE (the default is strict)
@fastmath fn f(...)           // opt into relaxed FP in this fn (§8.1)
@vectorize for (i in 0..n)    // ask the backend to vectorize this loop
@inline fn f(...)             // strong alwaysinline hint (Stage A1)
@noinline fn f(...)           // forbid inlining (Stage A1)
@hpc_strict fn f(...)         // perf diagnostics are errors for this fn (§9)
@unsafe fn f(...)             // whole body is an unsafe scope (§6)
@alias(a, b) fn f(a: *f64, b: *f64)  // declare no-overlap for vectorization
fn take(owned data: *u8) { ... }      // ownership transfers IN (§6)
fn view(borrowed s: Span<f64>) -> f64 { ... }  // read-only view (§6)
struct Name { field: T, ... }
struct Name align(64) { ... } // alignment override
struct Pair<T, U> { a: T, b: U }      // generic struct (§4d)
enum Color { Red, Green, Blue }       // enums + tagged unions (§4c)
enum Shape { Circle(f64), Rect(f64, f64), Point }
enum Result<T, E> { Ok(T), Err(E) }   // generic enums (prelude: §4f)
```

`fn main()` is the entry point (returns `i32` to the C ABI; `return 0`
implicit).

### 3.1 Function attributes — semantics and compiler contract

The Freshet function attributes are **language-level contracts**, not
optimiser hints: violating one is a compile error (or, for `@alias`,
a `F0306` warning plus suppression of the corresponding LLVM
attribute).

| attribute    | promise                                            | what the compiler does with it |
|--------------|----------------------------------------------------|--------------------------------|
| `@pure`      | function reads no memory and writes no memory      | **validated at compile time (F0304)**: the effect analysis (src/effects.cpp) checks the body and every callee against the contract; a violating `@pure` is a hard error, never a bypass.  Functions whose PROVEN summary is read-none + proven-returning get LLVM group `#52` (`nounwind memory(none) willreturn`); read-only pointer consumers get `memory(argmem: read)` (or `memory(read)` when globals/heap are touched) — `memory(none)` is only emitted when NO memory is accessed at all.  Writes-only and mixed summaries get the corresponding `memory(argmem: write)` / `memory(readwrite)` groups.  `willreturn` requires a separate termination PROOF (no `while` / arbitrary-condition `for` / `parfor` / `assert` / recursion) and is never inferred from effect-freedom alone. |
| `@strictfp`  | this function follows IEEE-754 strictly (no fast-math reassociation, no contraction, no NaN/signed-zero relaxation) | selects group `#51` (`nounwind strictfp`); `IrGen::fflag()` returns `""` rather than `"fast"` for every `fadd/fmul/fdiv` in this function.  Consequential: NaN/infinity/signed-zero/reassociation behaviour matches the IEEE reference, NOT the rest of the program. |
| `@inline`    | mark for aggressive inlining                       | selects group `#61` (`alwaysinline nounwind`).  Cost/legality still apply (recursion, very large bodies may be rejected by LLVM). |
| `@noinline`  | forbid inlining                                     | selects group `#60` (`noinline nounwind`).  Useful for differential benchmarking and for keeping hot helpers out of the inliner's expansion budget. |
| `@alias(a,b)`| every caller passes pointer args `a` and `b` that DO NOT overlap | when sound (no `F0306` in the compilation unit), emits LLVM `noalias` parameter attribute on both `a` and `b`.  When violated: warning + attribute suppressed for THIS pair only. |

#### `@alias(a, b)` — precise semantics (Stage B4 / B5)

`@alias(a, b)` is a **caller promise** that the memory regions pointed to
by parameters `a` and `b` do not overlap for the duration of the call.
This is *not* a permit to pass overlapping memory and *not* a tautology
that the compiler should always assume — it is a contract the caller
must keep.

The strict checker enforces it as follows:

- Two pointer arguments at a call site that provably alias (same root
  variable, same SoA plane) and whose parameter pair is **NOT**
  declared `@alias` → `F0305` (compile error).
- Two pointer arguments at a call site that provably alias and whose
  parameter pair **IS** declared `@alias` → `F0306` (warning) and
  the `noalias` LLVM attribute is suppressed for this pair on this
  function.  The program still compiles, but the optimiser is told the
  truth ("these arguments may alias").
- Arguments that cannot be resolved to a root variable (function-return
  values, arithmetic, `freshet.*` builtins) → no diagnostic, but the
  compiler emits **no** `noalias` fact for this pair either.

Historical note (pre-Stage B4): the checker used `aliasPairs` to *skip*
the error for same-root arguments, which is the opposite of the spec.
This was a bug; the new behaviour matches the spec exactly.

The compiler lowers `@alias` to LLVM only when **every** caller in the
current compilation unit can be verified to keep the promise.  Across
translation units or for indirect calls, `@alias` is currently *not*
lowered — this is the report's "lower only sound facts" rule.  A future
LTO-aware lowering could extend the analysis to all callers visible at
link time.


## 4. Structs

Structs use C layout: fields in declaration order, natural alignment,
struct alignment = max field alignment unless `align(N)` is given.

```
struct V3 { x: f64, y: f64, z: f64 }
struct Cell align(64) { p: V3, v: V3, m: f64 }   // one cache line

let c = Cell { p: V3 { x: 1.0, y: 2.0, z: 3.0 },
               v: V3 { x: 0.5, y: 0.5, z: 0.5 }, m: 2.0 };
c.p.x                        // nested field access
fn f(c: *Cell) { ... c.m }   // auto-deref: p.f on *Struct
let r = f(&c);               // &x takes an address of any lvalue
```

- Struct literals must initialize **all** fields (`F0201` otherwise —
  no partial init in strict mode).
- Structs are memory values: assignment copies, parameters pass by
  address (callee sees a copy), field access lowers to `getelementptr`.
  (This contract is now actually honoured: the historical definition
  emitted `%struct.X %param` while every call site passed `ptr`,
  silently miscompiling by-value struct parameters — e.g. a
  `sum2(P{1.5, 2.5})` that returned `1.5`.)
- `sizeof(T)` / `alignof(T)` are compile-time constants usable in `let`
  (scalars, structs, enums, arrays, `soa(S)`).

## 5. Statements

`let`, assignment (`= += -= *= /= %= &= |= ^= <<= >>=`), `return`,
`defer <call>`, `if/else`, `while`, `for`, `parfor`, `match`, `break`,
`continue`, `unsafe { ... }`, blocks, expression statements. `defer` runs
at scope exit, on every `return` **and** before every `break`/`continue`
that leaves the scope — the ownership analysis requires every `malloc` to
be matched by a visible `free`, a `defer free`, or a `give` transfer.

- `break;` / `continue;` bind to the innermost enclosing `for`/`while`
  loop (`F0213` outside a loop). Inside `parfor` they are not yet
  supported (`F0106`) because the body is split into per-chunk worker
  functions.
- The C-style `for (i = a; i < b; i += S)` step may be `i += E`,
  `i -= E`, or `i = i ± E`; range loops accept `for (i in a..b by E)`
  with `by -1` iterating downward. Non-unit strides are an `F0405`
  performance warning (an error under `--strict-performance`) — the
  lowering executes them correctly.
- `match (e) { Variant => {...}, Variant(a, b) => {...}, _ => {...} }`
  dispatches on an enum's tag; payload bindings are positional and
  immutable. Exhaustiveness is checked: every variant must be covered or
  a `_` arm must be present (`F0204`).
- Every non-void function must return a value on every path
  (`F0209`; `while (true)` counts as non-terminating). `return;` without
  a value in a non-void function is `F0209` as well; `main` keeps the
  documented implicit `return 0`.

## 6. Memory

```
let a: *f64 = malloc(f64, n);    // aligned, uninitialized
free(a);
give(a);                          // transfer ownership into a callee param
memcpy(d, s, bytes); memset(d, 0, bytes);
unsafe { ... }                    // explicit unchecked scope (see below)
```

- `malloc` results are *owned*: writable even through an immutable
  binding, tracked by the ownership state machine (Alive/MaybeFreed/
  Freed/Moved). Leak, double free, use-after-free, use-after-give and `give` of a
  non-owned value are all `F0301`.
- `freshet.prefetch_r(p)`, `freshet.prefetch_w(p)`, `freshet.clflush(p)`,
  `freshet.pause()` map to machine instructions.

### 6.1 Parameter ownership modes (report §5.3)

The `give(p)` call-site marker says *what* is transferred; the parameter
declaration says *why*:

```
fn consume(owned data: *u8) { free(data); }          // ownership moves IN
fn inspect(borrowed s: Span<f64>) -> f64 { ... }     // read-only view
```

- `owned` — every caller must pass `give(...)` (`F0303` otherwise);
  the callee may `free` it.
- `borrowed` — no `free`, no `give`, no writes through the parameter;
  callers may not give it away either.
- unannotated — the historical contract (ownership analysis as before).

### 6.2 `unsafe` scopes (report §5.1)

`unsafe { ... }` (or `@unsafe fn`) marks code whose correctness the
compiler cannot prove: Span/array bounds checks are omitted inside it and
the ownership diagnostics that fire within it are downgraded to warnings.
The safe path stays the default; the escape hatch is explicit and
greppable.

## 7. SoA arrays

```
struct Body { x: f64, y: f64, ..., flags: u64 }

let b: soa(Body) = soa_new(Body, n);   // nf planes, 64B-aligned each
defer soa_free(b);
b[i].x = 1.0;                          // plane x [i]
b[i].vy = 2.5;                         // plane vy [i]
soa_len(b);
```

A `soa(S)` value is a descriptor handle: `{ i32 nf, i64 n, ptr planes[nf] }`.
`s[i].f` lowers to `planes[f_idx][i]` — unit-stride loads that vectorize
into `<2 x double>` wide loads (verified in the optimized IR). Nested
field paths (`s[i].f.g`) resolve the inner struct's plane.


## 7a. Fixed-size arrays `[T; N]`

```
let a: [i64; 4] = [1, 2, 3, 4];
a[2] = 9;                       // bounds are part of the type
a.len                           // compile-time constant 4
let m: [[i64; 2]; 2] = [[1, 2], [3, 4]];
let ps: [P; 2] = [P { x: 1.0, y: 2.0 }, P { x: 3.0, y: 4.0 }];
```

- The length is part of the type; `sizeof([T; N]) = N * sizeof(T)`.
- Arrays are values: assignment and parameter passing copy (`memcpy`).
- Constant indices outside `0..N-1` are `F0212` at compile time;
  non-constant indices are bounds-checked in checked builds
  (`--checked` / `-O0` / `-O1`, `--no-bounds-check` to disable, traps
  with `frt_bound_trap` otherwise).
- An array literal adopts its annotation (`let f: [i32; 3] = [7, 8, 9]`
  builds `[3 x i32]`); a literal without an annotation is inferred from
  its elements.

## 7b. Slices / spans `Span<T>`

```
let s: Span<f64> = span(ptr, len);   // view over raw memory
let v: Span<i64> = span(arr);        // view over a fixed array
let sub: Span<i64> = v[1..4];        // sub-view, no copy
v[i]                                 // checked indexing
v.len                                // carried length
v.ptr                                // raw pointer out (FFI)
fn sum(xs: Span<f64>) -> f64 { ... } // cheap by-value (ptr, len) pair
```

- A span is a borrowed `(ptr, len)` pair passed by value — the function
  signature carries the valid range instead of a separate count argument.
- Indexing is bounds-checked in checked builds (`--checked` / `-O0` /
  `-O1`); a proven-safe index is never checked.
- Slicing `v[a..b]` validates `0 <= a <= b <= len` in checked builds.
- Mutation through a span writes the underlying storage; `borrowed`
  parameters (§6.1) make view read-onlyness a checked contract.

## 7c. Enums, tagged unions and `match`

```
enum Color { Red, Green, Blue }
enum Shape { Circle(f64), Rect(f64, f64), Point }

let s: Shape = Circle(2.0);
match (s) {
    Circle(r) => { ... }        // r binds the payload (immutable)
    Rect(w, h) => { ... }
    Point => { ... }
}
```

- Representation: `{ i64 tag, payload }` — the payload is the largest
  variant's field set, laid out per-variant like a mini-struct.
- Construction: contextual (`let c: Color = Red;`), qualified
  (`Color.Green`), or with payload (`Ok(v)`); variants resolve against
  the expected type.
- `match` lowers to tag comparisons; arms are blocks; payload bindings
  are positional copies. Exhaustiveness is enforced (`F0204`); `_` is
  the wildcard arm.
- Enums are values: assignment/parameters copy. Equality is not defined
  for enum values — use `match` (payload comparison lives in the arms).

## 7d. Generics (monomorphisation)

```
fn clamp<T: Numeric>(x: T, low: T, high: T) -> T { ... }
struct Pair<T, U> { a: T, b: U }
enum ReadResult<T> { Value(T), EndOfFile, Error(i32) }

fn first<T, U>(p: Pair<T, U>) -> T { return p.a; }
```

- Generic functions/structs/enums are TEMPLATES: they are type-checked
  per instantiation and never emitted themselves. Each distinct argument
  set creates a concrete instance with a mangled name
  (`clamp$i64`, `Pair$f64$i64`).
- Call-site inference: a `T`-typed parameter, `*T`, `Span<T>` or
  `Container<T, ...>` argument determines `T`. No runtime dispatch.
- Constraints: `Numeric`, `Int`, `Float`, `Signed` (checked at
  instantiation, `F0203`).
- Generic struct literals adopt the annotated instantiation:
  `let p: Pair<f64, i64> = Pair { a: 1.5, b: 7 };`.

## 7e. Strings

```
let s: str = "hello";            // UTF-8 view (ptr, len)
s.len                            // 5 (bytes)
s[0]                             // 104 (u8 — BYTE indexing)
let w: str = s[1..4];            // "ell" view
str_eq(a, b)                     // -> bool
let owned: String = str_cat(a, b);       // allocates
string_append(&owned, more);     // grows in place
str_of(&owned)                   // -> str view over the buffer
fmt_i64(42) / fmt_f64(3.14, 2)   // -> String
str_to_cstr(s)                   // -> *u8 (NUL-terminated copy; caller frees)
```

- `str` is an immutable, non-owning UTF-8 view. **Indexing is
  byte-based** (`s[i]` yields the i-th byte as `u8`); UTF-8-aware
  iteration/decoding is explicit user code. This is the documented rule,
  not an accident.
- `String` is the owned growable buffer `{ data: *u8, len, cap }`;
  `string_free(&s)` releases it (the ownership state machine does not
  track String allocations yet — free them explicitly or via `defer`).
- String literals are NUL-terminated in the constant pool for C interop.
- `print` accepts any `str` value (views and slices included), not just
  literals.

## 7f. `Option<T>`, `Result<T, E>` and `?`

```
fn safe_div(a: i64, b: i64) -> Result<i64, i64> {
    if (b == 0) { return Err(1001); }
    return Ok(a / b);
}
fn work(a: i64, b: i64) -> Result<i64, i64> {
    let q: i64 = safe_div(a, b)?;    // early-return on Err
    return Ok(q * 2);
}
```

- `Option<T> = enum { Some(T), None }` and
  `Result<T, E> = enum { Ok(T), Err(E) }` are prelude generic enums,
- `expr?` propagates `None`/`Err` out of the enclosing function (which
  must return the same family; `Result` error payloads must match).
  `assert` remains for violated invariants.

## 8. Parallelism: parfor

```
parfor (i in lo..hi) [clauses] { body }

clauses:
  reduction(+:acc)   reduction(-:acc)  reduction(*:acc)
  reduction(min:acc) reduction(max:acc)
  threads:8          chunk:4096
  affinity:close     affinity:spread
  collapse:2
```

Semantics:

- The iteration space `[lo, hi)` is split into chunks executed by a
  persistent worker pool (size `FRT_THREADS`, default = all cores); the
  calling thread participates. Step is ±1 (`by -1` iterates descending
  internally).
- `reduction` makes the accumulator a per-chunk register value; chunks
  combine with the operator after the region. Float reductions use
  fast-math so the vectorizer may reorder them (disable per-fn with
  `@strictfp`). `+`/`-` accumulators should init to 0 (`F0308`),
  `*` must init to 1 (`F0309`).
- Writes to captured scalars/structs race → `F0307`. Writes through
  pointers (`a[i] = ...`, `a[i].f = ...`) and through `soa` handles are
  the intended data-parallel pattern and are allowed (stride quality is
  `F0403`'s business).
- **Nesting:**
  - *rectangular* nesting (inner bounds independent of the outer var,
    no conflicting reductions): the compiler **collapses** — the inner
    loop is serialized inside the outer chunk function, so the whole
    nested iteration space runs in one parallel region and the inner
    loop vectorizes (`F0907` note).
  - *dynamic* nesting: the chunk function itself dispatches `frt_parfor`;
    inner chunks are stealable tasks in the global pool. The caller
    helps until its region drains — no thread ever blocks on a region,
    so nesting cannot deadlock (`F0906` note).
- `affinity:close` gives T large contiguous ranges (cache-friendly);
  `affinity:spread` gives ~8T small interleaved ranges (spreads hot
  lines across cores; combined with `FRT_AFFINITY=1`, workers are pinned
  to distinct cores best-effort).
- Intrinsics: `freshet.threads()` reports the pool size.
- **break/continue inside parfor** (report §12/§14; lane-local
  semantics): `continue;` skips the rest of the current iteration; `break;`
  stops the CURRENT CHUNK's remaining iterations — other chunks are
  unaffected. Neither terminates the whole region: iterations already
  dispatched elsewhere still run. Reduction accumulators observed by a
  chunk that broke early still combine. Use a flag + `if` when global
  termination is intended. Nested `for`/`while` loops inside the body
  bind `break`/`continue` to themselves as usual.
- **`@reproducible`** (report §12/Phase 5): written before `parfor` —
  ```
  @reproducible parfor (i in 0..n) [reduction(+: acc)] { ... }
  ```
  requests a DETERMINISTIC parallel reduction: the range is partitioned
  into fixed blocks (the `chunk:` clause, else 8192) independent of the
  worker count, every partial is computed in strict IEEE-754 order (the
  accumulator chain is never reassociated or vectorized), and partials
  combine in ascending block order with strict-FP adds. The result is
  bitwise-identical for any `FRT_THREADS` / affinity / scheduler on the
  same binary. `min`/`max` are included. Without the flag, reductions
  may use fast-math reassociation and thread-count-derived chunking —
  faster, but the float result can differ between runs.

### 8.1 Floating-point contract (report §8, Option A)

Freshet's default floating-point semantics are **strict, IEEE-754-like
per-operation**: no reassociation, no contraction, NaN/signed-zero
preserved.  Fast-math is an explicit opt-in for the HPC throughput
workflow (`@fastmath` on a function, or `--fast-math` for the whole
build).  This is a *language-level contract*, not an optimiser hint —
it affects program correctness, not just performance.

| mode        | spelling                            | effect                                                                                                                              |
|-------------|-------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|
| strict      | (default, no annotation)            | `IrGen::fflag()` emits no fast-math flag; every operation matches the IEEE-754 reference for its type.  `sin/cos/log2/log10` call libm (errno-visible memory is not hidden behind an intrinsic).  Reductions are bit-reproducible. |
| strict-FP   | `@strictfp` on the fn               | Same per-operation semantics plus the LLVM `strictfp` rounding-environment attribute (group `#51`). |
| fast-math   | `@fastmath` on the fn, or `--fast-math` | `fflag()` emits `fast` on every `fadd`/`fsub`/`fmul`/`fdiv`; reassociation, contraction and NaN/signed-zero relaxation are permitted.  `sin/cos/log2/log10` lower to LLVM intrinsics (vectorisable; `--veclib=libmvec` replaces them with vector routines).  Reduction order may vary run to run. |

**Consequential rules:**

- The mode is **per-function**; one program may mix strict and fast-math
  functions.  `@strictfp` always wins over `@fastmath` on the same
  function.
- A `@reproducible` mode (planned, not in this integration) will give
  deterministic reduction trees without the full IEEE-754 cost.
- FMA contraction happens only under fast-math; explicit `fsh_math_fma_*`
  calls are always available.

The IR generator's `fflag()` is the single source of truth for this
contract — every floating-point operation in the program flows through
it, and it consults the current function's `fastmath`/`strictfp` flags
and the `--fast-math` global.

## 9. Strict diagnostics

### 9.1 Correctness vs performance (report §7)

Correctness diagnostics are always errors.  Performance diagnostics are
**warnings by default** — a slow-but-correct program compiles; "your
program is incorrect" and "your program may be slow" are different
messages.  `--strict-performance` (or `@hpc_strict` on a function)
restores the historical hard errors for the HPC workflow:

| diagnostic | default | `--strict-performance` / `@hpc_strict` |
|---|---|---|
| type, cast, arity, mutability errors (`F0201`, `F0202`, `F0208`) | error | error |
| ownership errors: leak / UAF / double free / bad give (`F0301`) | error | error |
| `F0212`, `F0204`, `F0209`, `F0203`, `F0303` | error | error |
| division by the literal zero (`F0214`) | error (correctness) | error |
| integer division by a non-power-of-2 constant (`F0401`) or a runtime value (`F0402`) | warning | error |
| strided access burning whole cache lines (`F0403`) | warning | error (a stride under one cache line stays a warning) |
| hot field straddling a cache line (`F0404`) | warning | error |
| non-unit loop stride (`F0405`) / induction variable modified in body (`F0406`) | warning | error |
| AoS→SoA advice, layout padding, narrowing casts (`F0408`, `F0407`, `F0211`) | warning | warning |

### Diagnostic IDs

Every diagnostic has a stable ID of the form `Fnnnn`, printed in the
terminal as `error[F0402]` / `warning[F0402]`.  The ID names the *rule*,
not its severity: `F0402` is the same diagnostic whether this run
reports it as a warning or (under `--strict-performance`) an error.
IDs are never reused or renumbered; a diagnostic that is split later
gets new IDs.

- `penstock --explain F0402` (or its name, `--explain arith-div-runtime`)
  prints the registry entry and, when one exists, the long-form page
  from [`docs/errors/`](errors/) (`docs/errors/F0402.md`).  The same pages
  are the documentation site's per-diagnostic URLs.
- The registry is `src/diag_codes.h`; call sites reference it by name
  (`Code::ArithDivRuntime`), so a misspelt diagnostic does not compile.
- The IDs are also what LSP clients and `--report` output carry.
- Several IDs still cover a family of closely related failures
  (`F0201` type, `F0202` arguments, `F0301` ownership, …); these are
  split into finer IDs as their documentation pages are written.

Index (generated from the registry):

**Driver and project**

| ID | name | meaning |
|---|---|---|
| `F0001` | `io` | file could not be read or written |
| `F0002` | `proj` | project manifest error |
| `F0003` | `mod` | module declaration error |
| `F0004` | `import` | import could not be resolved or is not visible |
| `F0005` | `test` | invalid @test function |
| `F0006` | `fmt` | formatter refused to rewrite the file |
| `F0007` | `pgo` | profile data could not be used |
| `F0008` | `ir` | internal error while generating IR |

**Syntax**

| ID | name | meaning |
|---|---|---|
| `F0101` | `lex` | unexpected character |
| `F0102` | `parse` | syntax error |
| `F0103` | `preproc` | preprocessor error |
| `F0104` | `preproc-directive` | unknown preprocessor directive |
| `F0105` | `attr` | unknown or conflicting attribute |
| `F0106` | `parfor` | invalid parfor syntax or clause |

**Semantics**

| ID | name | meaning |
|---|---|---|
| `F0201` | `type` | type error |
| `F0202` | `args` | wrong arguments to a call or builtin |
| `F0203` | `generic` | generic instantiation error |
| `F0204` | `match` | invalid match expression |
| `F0205` | `dup` | duplicate definition |
| `F0206` | `main` | no entry point |
| `F0207` | `global` | global initializer must be constant |
| `F0208` | `mut` | assignment through an immutable binding |
| `F0209` | `ret` | missing or invalid return |
| `F0210` | `ret-main-implicit` | main returns implicitly |
| `F0211` | `cast-narrowing` | narrowing numeric cast |
| `F0212` | `bound` | constant index out of bounds |
| `F0213` | `loop-break-outside` | break or continue outside a loop |
| `F0214` | `arith-div-zero` | division by zero |
| `F0215` | `arith-neg-unsigned` | negation of an unsigned value |

**Safety (ownership, aliasing, purity, parfor)**

| ID | name | meaning |
|---|---|---|
| `F0301` | `own` | ownership violation |
| `F0302` | `own-suspicious` | suspicious ownership operation |
| `F0303` | `own-call` | argument passing mode mismatch |
| `F0304` | `pure` | @pure function has side effects |
| `F0305` | `alias-violation` | aliased arguments passed to noalias parameters |
| `F0306` | `alias-lie` | call breaks a function's @alias no-overlap promise |
| `F0307` | `thread` | unsafe construct inside a parfor body |
| `F0308` | `red-add-init` | reduction(+) not initialized to 0 |
| `F0309` | `red-mul-init` | reduction(*) not initialized to 1 |

**Performance**

| ID | name | meaning |
|---|---|---|
| `F0401` | `arith-div-const` | division by a non-power-of-2 constant |
| `F0402` | `arith-div-runtime` | division by a runtime value |
| `F0403` | `cache-stride` | strided access wastes cache lines |
| `F0404` | `cache-line-straddle` | hot field straddles a cache line |
| `F0405` | `loop-stride` | for-loop stride is not constant 1 |
| `F0406` | `loop-iv-written` | loop induction variable modified in the body |
| `F0407` | `layout-pad` | struct layout wastes bytes on padding |
| `F0408` | `soa` | array-of-structs access would benefit from SoA layout |

**Optimisation remarks (notes)**

| ID | name | meaning |
|---|---|---|
| `F0901` | `lane` | cross-lane optimisation applied |
| `F0902` | `lane-skip` | cross-lane optimisation skipped |
| `F0903` | `soa-auto` | automatic AoS-to-SoA conversion |
| `F0904` | `soa-aosoa` | AoSoA layout cost report |
| `F0905` | `par-advise` | loop appears safe to parallelise |
| `F0906` | `nested-ws` | nested parfor inner chunks become stealable tasks |
| `F0907` | `collapse` | rectangular nested parfor collapsed |
| `F0908` | `fuse` | adjacent loops fused |
| `F0909` | `unroll` | constant-trip loop fully unrolled |
| `F0910` | `partial-unroll` | loop partially unrolled |
| `F0911` | `peel` | first loop iteration peeled |
| `F0912` | `licm` | loop-invariant binding hoisted |
| `F0913` | `interchange` | loop nest interchanged |
| `F0914` | `tile` | loop nest tiled |
| `F0915` | `distribute` | loop distributed |
| `F0916` | `unroll-jam` | loop nest unrolled and jammed |
| `F0917` | `const-fold` | constants folded at exact source width |



## 9a. Primitive operations and numeric semantics (normative)

These rules are the contract the backend must honour; the IR generator
conforms to them by construction.

- **Integer overflow wraps.** `add`, `sub`, `mul` on signed and unsigned
  operands are two's-complement modular arithmetic (no `nsw`/`nuw` flags
  are emitted).  This is deliberate: wrapping is the specified
  behaviour, not undefined behaviour.
- **Unsigned overflow** likewise wraps (mod 2^N).
- **Signed division truncates toward zero**; the remainder takes the
  dividend's sign (`-7 / 2 == -3`, `-7 % 8 == -7`).  Constant
  power-of-two divisors are strength-reduced with a rounding-correction
  sequence that preserves exactly these semantics; the unsigned forms
  keep the plain shift/mask.
- **Division by zero and `INT_MIN / -1`** trap at runtime (the LLVM
  `sdiv`/`udiv` poison rules surface as a hardware trap on the
  supported targets).  Division by the literal `0` is the compile-time
  `F0214`.
- **Shift counts** wider than or equal to the operand width produce
  poison (avoided, not defined); `<<` is a logical shift, `>>` is
  arithmetic for signed operands and logical for unsigned.
- **Mixed-width arithmetic** promotes both operands to the common type
  first (the historical code left the lhs register at its narrow width,
  emitting malformed IR).  A bare INT literal adopts the other operand's
  integer type — signedness AND width — so `i32var / 4` is 32-bit signed
  arithmetic.
- **Comparisons** on pointers are defined for `==`/`!=` only (including
  against `nil`); ordering comparisons on pointers are a type error.
  Aggregate values (structs, enums, arrays, spans, str) are not
  comparable with `==` — str uses `str_eq`.
- **`nil`** is a `*void` null constant; it converts to any pointer type
  for equality tests.
- **Bool parameters** use the i8 C ABI at call boundaries; bool values
  are i1 registers (the zext at the boundary is generated).
- **Operator families** (checked in sema, before code generation):
  `+ - * /` numeric; `% & | ^ << >>` integers only; `== != < <= > >=`
  numeric/pointer-equality; `&& || !` bool only; `ptr ± int` pointer
  arithmetic; `ptr - ptr` (same pointee) element distance as `i64`.

## 10. Cache-layout analysis

For every loop touching `root[i].field` where `root: *S`:

- **Efficiency model**: bytes consumed per element = Σ sizes of used
  fields; bytes fetched = `sizeof(S)`. If efficiency < 50%, `F0408`
  fires with the concrete numbers and a soa(S) rewrite hint.
- **Layout**: `F0407` simulates a largest-first field reorder
  and reports savings per element.
- **Straddle**: `F0404` fires when a hot field's
  `(offset mod 64) + size > 64` and the struct is not `align(64)` —
  every access touches two lines; fix with `align(64)` or field order.

## 11. Math / I/O

`sqrt fabs floor ceil round fmin fmax pow exp log` map to LLVM
intrinsics (f32/f64); `sin cos tan log2 log10` call libm under the
strict default and lower to LLVM intrinsics under fast-math (§8.1).
`print(s)` accepts any `str` value. `print_f64(x, digits)`, `print_i64`,
`print_u64`, `println`, `assert(cond)`. `freshet.time_ns()`,
`freshet.cycles()`.  String/span/array builtins: §7a–§7f.

Integer `imin(a, b)` / `imax(a, b)` (optimisation-audit §6.7): exact,
memory-free minimum/maximum lowering to `icmp` + `select` with the
signedness of the common operand type.  The typing is sign-sensitive:
when either operand is syntactically negated (`-5`), the SIGNED
interpretation of the common width is chosen, so a negative literal can
never be silently reinterpreted as a huge unsigned value.  Used by the
loop tiler for exact boundary-tile bounds and generally useful for
clamped index arithmetic.

## 12. Automatic AoS→SoA: `--soa=auto`

With `penstock --soa=auto`, arrays that the F0408 analysis would flag
are rewritten to soa planes automatically — same transformation the
advice describes, applied by the compiler:

```
let b: *Body = malloc(Body, n);   --soa=auto-->  let b: soa(Body) = soa_new(Body, n);
b[i].x = ...;                                     b[i].x = ...;      // unchanged source
free(b);                                          soa_free(b);
```

The transform runs between parsing and semantic analysis, so all later
phases (sema, race checks, ownership, codegen) see exactly the program
the user would have written by hand.

A candidate `let v [: *S] = malloc(S, n)` is transformed only when it is
**provably safe and profitable**:

- every use of `v` is a plane-style access `v[e].f...` or `free(v)`;
  whole-element uses (`let e = v[i]`), `&v[...]`, passing `v` to calls,
  rebinding, or pointer arithmetic veto the transform (F0903
  explains);
- some loop over `v` fetches < 50% of `sizeof(S)` per element (the
  F0408 threshold) and **no** loop fetches ≥ 60% (planes would hurt
  the wide loop);
- `sizeof(S) ≥ 32` and S has ≥ 2 fields;
- `v` is not shadowed by another `let` of the same name in the function;
- the binding is not inside a `parfor` body.

Vetoed candidates keep the F0408 advice — `--soa=auto` never changes
output or semantics, it only converts wins the analysis can prove.
On the sweep benchmark the auto transform measures 85 ms →
12 ms (~7×), matching the hand-written SoA version.

## 13. Embedded Freshet superoptimisation (`-O3` / `-O4`)

At `-O3` and `-O4`, Penstock runs a bounded instruction superoptimiser whose
control loop is written in Freshet and linked into the compiler. CMake builds a
non-superoptimising bootstrap compiler, compiles `src/superopt.fsh` with it,
then links that object into the final compiler alongside static Z3 and
in-process Alive2 libraries. Runtime execution does not require Souper,
`alive-tv`, or the `z3` command-line tool.

The current candidate generator handles a deliberately conservative set of
integer strength reductions (multiplication by zero, one or a power of two;
unsigned division by one or a power of two; and unsigned remainder by a power
of two). Z3 must show that the fixed-width bit-vector miter is unsatisfiable;
Alive2 must then prove LLVM refinement; and the cost objective must improve.
Only verified candidates are retained. Unsupported instructions, failed or
inconclusive proofs, budget exhaustion and output failures leave the current
IR unchanged. It does not yet search arbitrary expression DAGs or synthesize
new algorithms from input/output samples.

`--superopt-budget=<sec>` controls the solver/verification budget (default 2 s
at `-O3`, 10 s at `-O4`); `--superopt-objective=latency|size|balanced` selects
the cost rule; and `--no-superopt` disables the stage. CMake options and
candidate scope are documented in [`docs/superopt-guide.md`](superopt-guide.md).

This is instruction-level optimisation, not unrestricted synthesis of a new
algorithm from an inverse input/output query. The finite-domain precomputation
stage remains separate and also uses the embedded Alive2 verifier. The legacy
genetic runtime tuner remains separately opt-in via `--evo-runtime-tune`.

## 14. Source-level loop optimisation

At `-O2` and above Penstock runs two semantics-preserving AST transform
passes after all checks and before IR generation. They optimise the
shapes a *source-level* compiler can prove and LLVM's generic loop passes
cannot see through (calls, batch slices, language-level ownership).
`--no-loop-opts` disables them; `--profile` builds skip them so
instrumented profiles measure the same structure the guided build
optimises. Cross-lane analysis also searches below a loop's immediate body
for pure, loop-invariant predicates nested under data-dependent guards. When
its memory-effect proof and code-growth gate pass, it duplicates the complete
outer loop and removes the nested test from each specialisation, so the
condition is evaluated once per loop nest rather than once per inner
iteration. Conditions that depend on any enclosing induction variable, or
loops with nested `parfor` regions, are conservatively excluded from this
wider rewrite.

### 14.1 Loop fusion + full unrolling (`F0908`, `F0909`)

Adjacent `for (iv in lo..hi)` loops over the **identical range** are
jammed into one sweep:

```
for (k in 0..nb) { fwd(&re[k*len], &im[k*len], len); }   // phase 1
for (k in 0..nb) { pw(&re[k*len], &im[k*len], &br[k*len], len); }
for (k in 0..nb) { inv(&re[k*len], &im[k*len], &out[k*len], len); }
```

fuses to a single batch loop running all three phases per batch
element — forward transform, pointwise arithmetic and inverse transform
back-to-back on slices that stay hot in cache, **batching retained**.
Legality is proven per pair of fused bodies, all pairs `i<j`:

- veto: any body may `return`, print/assert, dispatch a `parfor`, take
  a `give` argument, or hold unknown memory effects;
- tier 1: no flow/anti conflicts at all;
- tier 2: conflicting arrays touched only through direct `a[<affine
  in-iv, literal coefficient>]` indexing with **identical canonical
  index expressions** on both sides — the address function is
  injective in the iteration variable, so a match means *same
  address, same iteration*;
- tier 3 (call slices): `&r[iv*C]` passed to a function whose
  interprocedural footprint proves its writes/reads are confined to
  `[base, base+C)` (every write is `p[<loop var of for (j in 0..C)>]`,
  `C` a parameter) **and** the call-site count argument is exactly the
  stride `C` — slices tile the array, so every cross-phase access is
  same-iteration. Reads at `iv*C + j` with `j` an inner loop bounded
  by `C` stay inside the slice. The mut-rebind write idiom
  (`let mut w = p; w[j] = ...`) is resolved through an alias map;
- later loops' bounds must not read anything an earlier body writes,
  and bounds must be side-effect-free (`soa_len`/`@pure`/math allowed).

Constant-trip loops (`for (i in A..B)`, literal bounds, trip ≤ 16,
body·trip cost-capped, ivar never assigned) are **fully unrolled**: the
induction variable folds into literal indices and constant-offset
addressing reaches the backend (`F0909`).

### 14.2 Cross-lane analysis for the vectoriser (`F0901`)

LLVM's loop vectoriser (enabled at `-O2`+, with no forced metadata —
§14.3) turns simple loops into SIMD lanes where its cost model allows.
Two patterns waste that throughput:

- **Uniform branches.** An `if` whose condition does not mention the
  induction variable and reads nothing the loop writes has the same
  value in every lane, every iteration — it forces masked execution
  (both paths computed for every lane) or scalarisation.  The transform
  hoists the branch out and specialises the loop per path: the
  condition is evaluated **once, scalar** (one lane reserved for the
  decision) and every lane of the specialised, branch-free loops does
  independent work.  **Cost model** (explicit, reported via
  `F0902`): lane work ≥ 6 vectorisable ops (≥ 10 when an `else`
  doubles the body), specialised bodies ≤ ~600 nodes, constant trip
  counts ≤ 4 skipped, ≤ 3 unswitches per function — vector-light loops
  are never duplicated for less than the branch costs.
- **Recomputed predicates.** Adjacent `if`s with canonically identical
  conditions make the vectoriser emit the same vector compare + branch
  twice per iteration.  When neither arm writes anything the condition
  reads, they merge — the predicate is computed once and **reused**
  instead of recomputed.

Both transforms are pure AST restructures (statements shared, no new
bindings); outputs are bit-identical by construction and verified
against `--no-loop-opts` builds in the test suite. On the
batch-pipeline benchmark (`tests/bench_fuse.fsh`) fusion alone measures
**95 ms → 53 ms (~1.8×)** on the 2-vCPU sandbox; the cross-lane hoist is
measured at parity there (LLVM's own unswitcher handles the small case,
but its complexity gates can bail on fat bodies — the source-level
transform guarantees the specialisation).


### 14.3 Backend loop hints (`@vectorize`, `--force-vectorize`)

IR generation attaches **no forced vectorisation metadata by default**.
LLVM's loop vectoriser is enabled at `-O2`+ and its cost model decides
legality and profitability; the historical blanket
`llvm.loop.vectorize.enable` + `interleave.count 4` on every simple loop
could override the profitability choice and hurt small loops, register
pressure, code size and non-AVX2 targets (audit §5).

Two opt-ins exist:

- `@vectorize for (...)` / `@vectorize parfor (...)` — a per-loop
  request. Emits a plain `vectorize.enable` hint **without** an
  interleave count, so the width/unroll choice stays with the cost
  model. Fusion propagates the hint to the fused loop. The emitted LLVM
  LoopID is self-referential, and the enable hint is represented by its
  name string alone (without a legacy `i1 true` operand).
- `--force-vectorize` — restores the historical blanket hint
  (`vectorize.enable` + `interleave.count 4`) for differential
  benchmarking against the old behaviour.

The performance report distinguishes "loops w/ vectorise annotation"
(**[static]** estimate — what the front end attached) from "loops
vectorised" / "vectorisation missed" (**[backend]** results — what the
backend actually did, parsed from `--opt-remarks` records).  An
annotation is a request, not proof of vectorisation.

## 15. Toolchain support

Penstock emits textual LLVM IR and drives `clang` as its backend; it
builds and generates code for **LLVM 19 through 23**.  This matrix is
the single source of truth — the README, the CMake probe, and this
document all agree on the 19–23 range.  The CMake configure step
smoke-tests the resolved backend before building, so a broken
toolchain fails immediately with a targeted hint instead of mid-build.

- **Selection order.** `$LLVM_ROOT` (env) wins if set; otherwise the
  compiler and the CMake configure step probe, newest first,
  `tools/llvmNN/usr/lib/llvm-NN` beside the project and then system
  prefixes `/usr/lib/llvm-NN`, for NN in 23, 22, 21, 20, 19.
  `cmake -B build -DLLVM_ROOT=<prefix>` pins the toolchain for the build
  *and* for the `check`/`bench` targets (the resolved root is exported
  to the test and benchmark scripts); `--report` prints the resolved
  backend in its header (`backend : LLVM 23.x.y (…)`).
- **Runtime cache.** The clang-compiled runtime IR is cached per LLVM
  major AND runtime optimisation level
  (`build/frt_rt_<N>_o<level>.ll`, default `-O2` — audit §3) so a
  runtime produced by one LLVM version or optimisation level is never
  reused after switching toolchains.
- **IR dialect.** The generated IR uses the opaque-pointer dialect
  (`ptr`, `llvm.memset.p0.i64`, …) that LLVM 19–23 parse natively.
  User functions carry attribute groups `#50`–`#53` and (Stage A1)
  `#60`/`#61` for explicit `@noinline` / `@inline`
  (`nounwind` / `+strictfp` / `+memory(none) willreturn` for `@pure` /
  `+alwaysinline` / `+noinline`); these sit far above the
  machine-generated groups clang emits for the runtime module
  (`#0`–`#9` on LLVM 19–23), which matters because the runtime is merged
  by *textual concatenation*: a redefined group number would silently
  override the user's attributes. (`#5`-era groups were silently
  overridden — dropping `noinline` benignly, losing `@strictfp`, and
  turning every `export fn` into `cold noreturn`; `#50+` makes the
  attributes take effect exactly as emitted on all supported versions.)

## 16. Modules and multi-file compilation (report §6.1/§6.2)

```
module geometry;          // optional: file-based module identity (must
                          // be the first declaration in the file;
                          // default: sanitised file stem)

import geometry;          // pulls in geometry's PUBLIC declarations

pub fn distance(a: Point, b: Point) -> f64 { ... }
pub struct Point { x: f64, y: f64 }
fn helper() { ... }       // private: invisible outside the module
```

- **Importing is not textual inclusion.** No duplicate definitions,
  no macro leakage: `#include`/`#define` remain preprocessor features
  for local text splicing; `import` is a language-level dependency.
- **Visibility.** From a module you can see: your own declarations (pub
  or private), `pub` declarations of directly imported modules, and the
  prelude (`String`, `Option`, `Result`). Everything else is private —
  using it is `F0004` ("private to module 'm'"). Two modules may
  define the same name (private decls coexist); an ambiguous unqualified
  reference to two visible `pub` decls is `F0004` (qualify it).
- **Qualified names.** `geometry::distance(a, b)` restricts resolution
  to that module (`mod::item` in expressions, `mod::Type` in type
  position). Qualifying a non-imported module is `F0004`.
- **Resolution.** `import geometry;` searches the entry file's
  directory, then `-I<dir>` directories, then the builtin modules
  directory (where the shipped `fsh` module lives), then `.` — for
  `geometry.fsh` or `geometry/mod.fsh`. Missing module → `F0004`
  with the searched paths. **Circular imports are `F0004`** with the
  full cycle chain.
- **Multi-file compilation.** `penstock main.fsh util.fsh` — every
  `.fsh` argument is a module of ONE program (the first is the entry).
  Imports resolve the rest. Internal symbols disambiguate to
  `u_<module>$<name>` in the IR only when two modules define the same
  name; `main`, `export fn` and `extern fn` keep their exact C-ABI
  names.
- **Project manifest** (`penstock build [dir]`, report §6.2):
  ```toml
  [project]
  name = "demo"
  [target]
  type = "bin"            # bin | static-lib | shared-lib
  src  = ["src/main.fsh"] # entry modules (imports pull the rest)
  [build]
  opt = 3
  threads = 0
  [link]
  libs = ["m"]
  [modules]
  path = ["lib"]          # extra import search dirs
  ```
  The output lands beside the manifest as `<project name>` (`lib<name>.a`
  / `<name>.so` for library targets) unless `-o` overrides. Whole-program
  dependency tracking: the build cache hashes the CONTENT of every module
  in the import closure plus the flags — unchanged sources and flags skip
  recompilation ("up to date"; `--force` rebuilds).

## 17. Function pointers and C callbacks (report §6.3)

```
fn add(a: i64, b: i64) -> i64 { return a + b; }

let f: fn(i64, i64) -> i64 = add;      // fn value coerces to the ptr type
let mut g: fn(i64, i64) -> i64 = add;
g = mul;                                // reassign
print_i64(f(2, 3));                     // INDIRECT call through the slot

fn apply_twice(f: fn(i64, i64) -> i64, x: i64) -> i64 {
    return f(f(x, x), x);               // fn-pointer PARAMETERS
}

extern fn qsort(base: *u8, n: usize, sz: usize,
                cmp: fn(*u8, *u8) -> i32);   // C callback ABI
fn cmp_desc(a: *u8, b: *u8) -> i32 { ... }
qsort(d as *u8, n, sizeof(i64), cmp_desc);   // Freshet fn as callback
```

- `fn(P1, P2) -> R` is a first-class, pointer-sized value type
  (structurally interned; `void` return may be omitted via `-> void`).
- Referencing a function by name (or `&name`) yields its pointer —
  `export fn` keeps its exact C symbol so C code can call back in.
- Indirect calls type-check arguments against the pointer's signature.
  Their effects are unknown → an indirect call inside `parfor` is
  `F0307`, and `@pure` bodies may not contain them.
- Calling conventions: the C ABI (the default for `extern fn`).
  Variadic C functions remain out of scope (report §6.3 defers them).

## 18. Tooling: test runner, formatter, linter, debug info (report §10)

- **`penstock test <file.fsh>`** — every zero-arg void `@test fn` runs
  through a generated harness main: per-test `test name ... ok|FAIL`
  lines, a `N/M tests passed` summary, and exit code 1 on any failure.
  An `assert` inside a test aborts into a SIGABRT long-jump ISOLATED to
  that test — the suite continues. `@test` fns must be non-generic,
  parameterless and return void (`F0005` otherwise).
- **`penstock fmt [--check] <file.fsh>`** — compiler-aware, token-level
  formatter (4-space indent, 78-column short-block collapse,
  house-style signatures, comments preserved). SAFETY GATE: the output
  is only written when its token stream is IDENTICAL to the input's —
  the formatter can never change what the program means. `--check`
  exits 1 when a file is not already formatted.
- **`penstock lint <file.fsh>`** — front-end only; prints every E-/W-/
  N- diagnostic and exits non-zero on errors (report §10.2: the natural
  home for optional performance-policy checks).
- **`penstock -g` / `--debug`** — DWARF source-level debug info: line
  tables + per-function symbols for every module (dynamically numbered
  `DISubprogram`/`DIFile` metadata; calls, returns and branches carry
  `!dbg`). `gdb <out>` / `addr2line` resolve Freshet source lines;
  combine with `-O0` for the richest tables.
- **Language server** (`freshet-language-server`): position-aware
  hover (inferred type / full signature), **goto-definition**,
  **find-references** and **rename** over stdio JSON-RPC (report
  §10.1). Resolution goes through the sema-annotated AST, so rename
  edits exactly the resolved symbol's occurrences — declaration
  included.

## 19. The `fsh` standard library module (report §6.3)

`import fsh;` — found through the builtin modules directory, no `-I`
needed. Freshet-facing surface over the runtime (args / environment /
file I/O without a single manual `extern fn`):

```
pub fn args_count() -> i64
pub fn args_get(i: i64) -> str            // borrowed view of argv[i]
pub fn arg_or(i: i64, d: str) -> str
pub fn env_get(name: str) -> str          // "" when unset
pub fn env_or(name: str, d: str) -> str
pub fn file_size(path: str) -> i64        // -1 when missing
pub fn file_read(path: str) -> str        // fresh NUL-terminated buffer
pub fn file_write(path: str, data: str) -> i64
pub fn assert_eq_i64(got: i64, want: i64, what: str) -> i64
pub fn assert_eq_f64(got: f64, want: f64, what: str) -> i64
```

The C-level `libfsh` (include/fsh/*.h, `libfsh.a`) remains available
through `extern fn` + `#link fsh` for everything not wrapped here.
