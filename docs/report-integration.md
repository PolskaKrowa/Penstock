# Freshet / Penstock — Language Gap Report: Integration Change Report

**Project:** Freshet (language) / Penstock (compiler)
**Integration version:** `0.1.0-INDEV` (was `0.0.1-INDEV`)
**Scope agreed with the maintainer:** initially report §3–§5 (Phases 0–2)
plus the §7 diagnostics policy and the §8 floating-point policy
(Option A); §5 memory modes; byte-based string indexing — then extended
to **the whole report**: Phases 3–5 (modules, projects, FFI, standard
library, tooling, deterministic reductions), together with the
optimisation-and-proof audit documented in its companion report
(`docs/optimization-integration.md`). Deliverable = updated source tree
+ this report; verification = full build + regression suite.

**Result:** **134/134 regression tests pass** (72 pre-existing — all
preserved in behaviour or explicitly migrated with the new policy
contracts — plus 23 for Phases 0–2, 20 for the optimisation audit and
19 for Phases 3–5). Every entry of the report's §14
"general-purpose Freshet minimum" checklist is now implemented. The
compiler is built and exercised end-to-end against LLVM 19 through both
build systems (CMake and `scripts/build.sh`).

---

## 1. Executive summary

Every P0 item from the report is implemented and regression-tested:

- the signed division/remainder miscompilation is fixed (and the
  strength reduction is now provably truncating-toward-zero);
- definite-return analysis exists; `return;` in a non-void function is a
  hard error; a compiler no longer guesses that a missing value is zero;
- the operator/type matrix is checked in sema before code generation;
- every documented operator (`<<=`, `>>=`, `give(p)`, `sizeof`,
  `alignof`, the C-style `for` step forms) genuinely parses and runs —
  several of them never did;
- arrays `[T; N]`, spans `Span<T>`, enums with payloads, exhaustive
  `match`, monomorphised generics, `str`/`String`, `Option<T>` /
  `Result<T, E>` with `?` propagation, `break`/`continue`,
  `owned`/`borrowed` parameter modes and `unsafe` scopes are all in the
  language, specified, and tested;
- performance diagnostics are warnings by default
  (`--strict-performance` / `@hpc_strict` restore the HPC gates);
- floating point is strict IEEE-like by default (`@fastmath` /
  `--fast-math` opt in).

And, since the extension to the full report, Freshet is no longer a
single-file numerical kernel language:

- **modules and multi-file projects** — `module`/`import`/`pub`,
  qualified names, circular-import detection, `penstock build` with a
  `penstock.toml` manifest and a dependency cache;
- **a real FFI** — first-class function pointers, indirect calls and C
  callbacks (Freshet code runs as a `qsort` comparator);
- **a standard-library module** (`import fsh;`) — argv, environment,
  file I/O, assertions, without a single manual `extern fn`;
- **tooling** — `penstock test`, `penstock fmt`, `penstock lint`,
  `-g` DWARF debug info, and an LSP server with hover,
  goto-definition, find-references and rename;
- **deterministic parallel reductions** (`@reproducible`) and
  `break`/`continue` inside `parfor`.

Along the way, ten additional latent defects were found and fixed
(section 12) — several of them silent wrong-code bugs — and one shipped
build defect (the language server's CMake source list was missing the
new front-end files, which broke its link) is fixed structurally with a
single shared source list (section 11).

---

## 2. Phase 0 — correctness and semantic contract (report §3)

### 2.1 Signed division by a power of two (report §3.1) — FIXED

`src/irgen_expr.cpp` (`emitArithOp`): constant power-of-two division and
remainder are now lowered with the rounding-correction sequence
`q = (a + ((a >> (W-1)) & (d-1))) >> k` (arithmetic shift) for signed
operands and `r = a - q*d` for the remainder; unsigned operands keep the
plain `lshr`/`and` forms. Verified against a C reference across i8/i16/
i32/i64, negative dividends, divisors 1/2/4/8/16, and inside composed
expressions — at -O0, -O2 and -O3 (`tests/pos_divrem.fsh`, expected
values embedded in the test).

Also fixed while there: mixed-width arithmetic used to leave the lhs
register at its narrow width (`add i64 %i32val, %i64val` — malformed
IR); mixed-width comparisons picked the comparison predicate from the
wrong type; and `x / 1` skipped an allocated register number, which
made LLVM reject the whole function.

### 2.2 Missing return values (report §3.2) — FIXED

- `src/sema.cpp`: definite-return analysis (`stmtAlwaysReturns`): every
  path through a non-void function must `return` or terminate in a
  recognised non-returning construct (`while (true)`), else `F0209`.
  `return;` without a value in a non-void function is `F0209` (was a
  warning). The implicit `return 0` rule is kept only for the void
  `main` (the documented entry-point convention).
- `src/irgen_stmt.cpp`: the fall-off-the-end path now emits
  `unreachable` (valid IR) instead of a fabricated zero/null value;
  a `return;` in a non-void function can no longer produce an
  ill-typed `ret void`.
- Tests: `tests/neg_noret.fsh`, `neg_retvoid.fsh`, `neg_oneret.fsh`
  (all `F0209`), `tests/pos_defret.fsh` (both-branch, trailing,
  while(true), early-return shapes).

### 2.3 Operator checking (report §3.3) — DONE

`src/sema_expr.cpp`: explicit operator families — arithmetic numeric
(`+ - * /`), integer-only (`% & | ^ << >>`), comparison (numeric, and
pointer `==`/`!=` including `nil`; ordering on pointers and aggregate
equality are type errors), logical (bool only), pointer arithmetic
(`ptr ± int`, `ptr - ptr` element distance). Invalid combinations are
rejected in sema, before IR generation. `src/irgen_expr.cpp` now has a
single shared arithmetic emitter used by both the binary-expression and
compound-assignment paths, so the two spellings can never disagree about
signedness, strength reduction or shift semantics.

### 2.4 Grammar conformance (report §3.4) — DONE (and then some)

- `<<=` / `>>=` are lexed as single tokens, parsed as assignment
  operators, type-checked and lowered (`shl` / `ashr`/`lshr` by
  signedness) — `tests/pos_shiftassign.fsh`.
- The misleading `i++` parser comment is removed (there is no `++`
  token; the spec never promised one).
- **Additional findings in the same family, all fixed:** `give(p)` as a
  call argument never parsed (`give` is a keyword; the parser only
  matched identifiers); the C-style `for` step never parsed at all
  (`i += 1` / `i = i + 1` are assignment forms fed to an expression
  parser — the F0405 stride analysis was dead code); the induction
  variable was declared after the condition was typed; `sizeof` /
  `alignof` never resolved (documented as compile-time constants in
  spec §4; they now fold in sema for scalars, structs, enums, arrays and
  `soa(S)`).

### 2.5 Numeric semantics (report §3.5) — SPECIFIED

`docs/language-spec.md` §9a (new, normative): integer overflow wraps
(two's-complement modular, deliberately — no `nsw`/`nuw` is emitted and
that is now a language rule rather than an accident of codegen);
unsigned wraps; signed division truncates toward zero with the dividend
sign for the remainder; division by zero and `INT_MIN / -1` trap; shift
count rules; mixed-width promotion and INT-literal adoption; pointer
comparison rules; `nil` compatibility; bool ABI. The backend conforms
by construction, and the regression suite pins the behaviour.

---

## 3. Phase 1 — data and control foundations (report §4.1, §4.4)

### 3.1 Fixed arrays `[T; N]` (report §4.1)

Type, literals, indexing, `.len` (compile-time), `sizeof`, value copies
(assignment and by-address parameter passing with a callee copy), nested
arrays, arrays of structs, annotation adoption for literals, `F0212`
for constant out-of-range indices, checked-build bounds checks.
`tests/pos_arrays.fsh`, `tests/neg_bound.fsh`.

### 3.2 Slices / spans `Span<T>` (report §4.1)

First-class `(ptr, len)` pair aggregate: `span(p, len)`, `span(arr)`,
`s[a..b]` slicing (validated in checked builds), checked indexing,
`.len`, `.ptr` for FFI, by-value parameter passing. Mutation through a
view is supported; `borrowed` parameters make read-onlyness a checked
contract. `tests/pos_spans.fsh`.

### 3.3 `break` / `continue` (report §4.4)

Loop-exit lowering with a loop-context stack; defers registered inside
the loop body run before the branch leaves; statements after a
terminator are no longer emitted (this also fixes the historical
dead-code-after-return shape that produced instructions after `ret`).
`F0213` outside a loop; `F0106` directly inside a `parfor` — until
this integration; see section 9.2 for the new lane-local semantics.
The unroll/fusion/lane passes veto on `hasBreak` so loop transforms
cannot drop early exits. `tests/pos_breakcont.fsh`,
`tests/neg_brkloop.fsh`.

### 3.4 Enums, tagged unions, exhaustive `match` (report §4.3)

`enum` declarations with payload-carrying variants; representation
`{ i64 tag, payload }` with per-variant layout; contextual, qualified
(`Color.Green`) and payload (`Ok(v)`) construction; `match` with
positional payload bindings, wildcard `_`, and exhaustiveness checking
(`F0204`); enums as values (copy semantics), in arrays, as function
parameters and returns. `tests/pos_enums.fsh`,
`tests/neg_match_nonex.fsh`.

---

## 4. Phase 2 — generics, strings, error handling (report §4.2, §4.5, §4.6)

### 4.1 Generics via monomorphisation (report §4.2)

Generic functions (`fn clamp<T: Numeric>(...)`), generic structs
(`struct Pair<T, U>`) and generic enums, with constraints `Numeric`,
`Int`, `Float`, `Signed` (validated at instantiation, `F0203`).
Call-site inference from `T`-typed, `*T`, `Span<T>` and
`Container<T, ...>` parameters; mangled instances (`clamp$i64`,
`Pair$f64$i64`); each instantiation is type-checked as ordinary code
(monomorphise-then-check), so errors point at real code. Templates are
never emitted; every whole-program pass skips them. Nested
instantiations (`Pair<Pair<i64,i64>, f64>`) work; struct literals adopt
the annotated instantiation. `src/generics.cpp` (new),
`tests/pos_generics.fsh`, `tests/neg_genericconstraint.fsh`.

### 4.2 Strings (report §4.5; maintainer decision: byte indexing)

- `str` — immutable UTF-8 view as a `(ptr, len)` pair; literals are
  values (NUL-terminated in the pool for C interop); `s[i]` is the i-th
  **byte** (the documented rule, per the agreed decision); `s[a..b]`
  slicing; `.len`; `print` accepts any `str` value — not just literals.
- `String` — owned growable buffer (`string_new/from/append/free`,
  `str_of`); `str_cat`, `str_eq`, `str_to_cstr`, `fmt_i64`, `fmt_f64`.
- Runtime support added to `runtime/frt_rt.c` (out-param constructors so
  the C ABI stays simple).
`tests/pos_strings.fsh`.

### 4.3 `Option<T>` / `Result<T, E>` and `?` (report §4.6)

Prelude generic enums (lazily registered on first use) with the report's
exact shapes; `expr?` propagates `None`/`Err` out of the enclosing
function (family and error-payload compatibility checked in sema), runs
defers on the early-exit path, and yields the payload on the ok path.
`assert` remains for violated invariants. `tests/pos_optres.fsh`.

---

## 5. Compiler policy (report §7, §8)

### 5.1 Correctness diagnostics vs performance linting (report §7)

Performance diagnostics are **warnings by default** — a slow but correct
program compiles: `F0401` / `F0402` (non-power-of-2 / runtime division), `F0403` (wasteful
strides), `F0404` (field straddling), `F0405` / `F0406` (non-unit stride /
ivar mutation). The IDs are the same in every mode: `--strict-performance`
(or `@hpc_strict` per function) simply raises these diagnostics from
warnings to hard errors for the HPC workflow.
Division by the literal zero stays a hard error in every mode — that is
correctness, not preference. Because non-unit strides now compile, the
`for` lowering actually honours the step expression (`i += E`, `i -= E`,
`i = i ± E`, `by -1` descending ranges). The two historical negative
tests were migrated to the warn-first contract and paired with
strict-mode twins (`neg_arith{,_strict}.fsh`,
`neg_straddle{,_strict}.fsh`, `neg_divzero.fsh`).

### 5.2 Floating-point default — Option A (report §8)

Strict IEEE-754-like per-operation semantics are the **default**
(`IrGen::fflag()` emits no fast flag); `@fastmath` on a function or
`--fast-math` globally opts into the relaxed contract; `@strictfp`
always wins. `sin/cos/log2/log10` keep scalar libm calls under the
strict default and lower to LLVM intrinsics only under fast-math. Spec
§8.1 rewritten for the new contract. The §8 vector-math test now runs
under `@fastmath` and a new check pins the strict default.

---

## 6. Memory safety and data modelling (report §5)

- **Parameter ownership modes (§5.3):** `owned data: *u8` (every caller
  must `give(...)`, `F0303`; the callee may free) and
  `borrowed s: Span<f64>` (no free/give/mutation; callers may not give
  it away) — the contract now lives in the signature, as the report
  asked. `src/parser.cpp` (contextual keywords), `src/sema_expr.cpp`
  (call-site enforcement), `src/sema.cpp` (symbol flags).
- **`unsafe` scopes (§5.1):** `unsafe { ... }` and `@unsafe fn` omit
  bounds checks and downgrade in-scope ownership diagnostics to
  warnings — the escape hatch is explicit and greppable.
- **Nullability (§5.2):** `Option<T>` is the deliberate representation
  for absent values; raw nullable pointers remain for C interop.
- **Layout model (§5.4):** structs and `soa(S)` are untouched; the new
  types are complementary, exactly as recommended.

`tests/pos_memmodes.fsh`, `tests/neg_ownedcall.fsh`.

---

## 7. Phase 3 — modules, projects and FFI (report §6)

### 7.1 A real module system (report §6.1) — DONE

`src/modules.cpp` (new) + spec §16. Importing is language-level
dependency, not textual inclusion: no duplicate definitions, no macro
leakage (`#include`/`#define` stay preprocessor features for local text
splicing).

- **Module identity:** the optional `module geometry;` header (must be
  the first declaration; default = sanitised file stem).
- **Visibility:** private by default; `pub fn` / `pub struct` export.
  Two modules may define the same private name; using another module's
  private declaration, or an ambiguous unqualified reference to two
  visible `pub` decls, is `F0004` (qualify it).
- **Qualified names:** `geometry::distance(a, b)` in expressions and
  `mod::Type` in type position; qualifying a non-imported module is
  `F0004`.
- **Resolution:** entry-file directory, then `-I<dir>`, then the
  builtin modules directory, then `.` — for `geometry.fsh` or
  `geometry/mod.fsh`; a missing module reports every searched path.
  **Circular imports are `F0004`** with the full cycle chain.
- **IR symbol plan:** internal symbols disambiguate to
  `u_<module>$<name>` only when two modules define the same name;
  `main`, `export fn` and `extern fn` keep their exact C-ABI names.

Tests: `pos_modules.fsh` (+ `tests/mods/`), `neg_import_private.fsh`,
`neg_import_missing.fsh`, the circular-import pair.

### 7.2 Multi-file compilation and project builds (report §6.2) — DONE

- `penstock main.fsh util.fsh` — every `.fsh` argument is a module of
  one program (the first is the entry); imports resolve the rest.
- `penstock build [dir]` reads a `penstock.toml` manifest:
  `[project] name`, `[target] type = bin | static-lib | shared-lib` +
  `src`, `[build] opt / threads`, `[link] libs`, `[modules] path`.
  Output lands beside the manifest (`<name>`, `lib<name>.a`,
  `<name>.so`) unless `-o` overrides.
- **Dependency cache:** the build hashes the *content* of every module
  in the import closure plus the flags — unchanged sources print
  "up to date" and skip recompilation; `--force` rebuilds.

Tests: multi-input compilation, project-manifest build + cache hit
(`tests/proj/`).

### 7.3 Function pointers and C callbacks (report §6.3) — DONE

Spec §17. `fn(P1, P2) -> R` is a first-class, pointer-sized value type
(structurally interned). Referencing a function by name yields its
pointer; slots are reassignable; indirect calls type-check arguments
against the pointer's signature; function-pointer parameters work
(`fn apply_twice(f: fn(i64,i64) -> i64, ...)`). `extern fn` with
fn-pointer parameters gives the C callback ABI — the regression test
drives libc `qsort` with a Freshet comparator end-to-end. Indirect
calls have unknown effects, so an indirect call inside `parfor` is
`F0307` and `@pure` bodies may not contain them. Variadic C
functions stay out of scope, as the report itself defers them.

Tests: `pos_fnptr.fsh` (qsort), `neg_fnptr_parfor.fsh`.

### 7.4 The shipped `fsh` module (report §6.3, §4.7) — DONE

`modules/fsh.fsh`, found through the builtin modules directory (no
`-I` needed): `args_count`, `args_get`, `arg_or`, `env_get`, `env_or`,
`file_size`, `file_read`, `file_write`, `assert_eq_i64`,
`assert_eq_f64` — ordinary I/O without a single manual `extern fn`.
The C-level `libfsh` (`include/fsh/*.h`, `libfsh.a`) remains available
through `extern fn` + `#link fsh` for everything not wrapped.

Test: `pos_fsh_module.fsh` (argv, env, file write/read/size, defaults).

---

## 8. Phase 4 — standard library surface and tooling (report §10)

### 8.1 `penstock test` — the @test runner (report §10.3) — DONE

Every zero-arg void `@test fn` runs through a generated harness main
(`synthesizeTestMain` in `src/modules.cpp`): per-test
`test name ... ok|FAIL` lines, an `N/M tests passed` summary, exit code
1 on any failure. An `assert` inside a test aborts into a SIGABRT
long-jump **isolated to that test** — the suite continues past the
failure. `@test` functions must be non-generic, parameterless and
return void (`F0005`).

### 8.2 `penstock fmt` — the formatter (report §10.2) — DONE

`src/fmt.cpp` (new): compiler-aware, token-level reformatting —
4-space indent, 78-column short-block collapse, house-style
signatures, comments preserved. **Safety gate:** the output is written
only when its token stream is *identical* to the input's, so the
formatter can never change what a program means. `--check` exits 1
when a file is not already formatted. The regression test proves
reformat → idempotence → compiled-equivalence in one chain.

### 8.3 `penstock lint` (report §10.2) — DONE

Front-end only: prints every E-/W-/N- diagnostic and exits non-zero on
errors — the natural home the report suggested for optional
performance-policy checks.

### 8.4 `-g` / `--debug` — source-level debug info (report §10.3) — DONE

DWARF line tables + per-function symbols for every module (dynamically
numbered `DISubprogram`/`DIFile` metadata; calls, returns and branches
carry `!dbg`). The regression test verifies with `objdump
--dwarf=decodedline` and resolves a `u_inner` symbol back to its
Freshet source line with `addr2line`. Combine with `-O0` for the
richest tables.

### 8.5 Language server navigation (report §10.1) — DONE

`src/lsp/server.cpp`: position-aware **hover** (inferred type / full
signature), **goto-definition**, **find-references** and **rename**
over stdio JSON-RPC. Resolution goes through the sema-annotated AST, so
a rename edits exactly the resolved symbol's occurrences — declaration
included. The regression test drives the real server process
(`scripts/lsp_nav_check.py`): hover/definition/references/rename in one
session.

---

## 9. Phase 5 — deterministic reductions and parfor control flow (report §12/§14)

### 9.1 `@reproducible` reductions — DONE

`@reproducible parfor (i in ...) [reduction(+: acc)]` compiles the
reduction to a deterministic, thread-count-independent order: the
regression test builds a grouping-sensitive float sum (10^9 + 10^-7
terms) and requires **bitwise-identical output at FRT_THREADS = 1, 3
and 8** — the unflagged `parfor` diverges on exactly this input, the
flagged one cannot.

### 9.2 `break` / `continue` inside `parfor` — DONE

The historical `F0106` limitation is lifted with **lane-local
semantics**: a `break` exits only the executing lane's iteration set
(never the whole `parfor`), `continue` skips to the lane's next
iteration. The unroll/fusion/lane transforms still veto bodies with
breaks, so no whole-program transform can drop an early exit.
`tests/pos_parfor_break.fsh`.

### 9.3 Deliberately deferred (report Phase 5: "choose based on demand")

Richer task-level concurrency, typed atomics/synchronisation, GPU/MPI
integration, compile-time evaluation and hygienic macros,
traits/interfaces, package management. Nothing in the report's §14
minimum requires them.

---

## 10. The report's acceptance bar (§13/§14) — where it stands

Every checklist item of the §14 "general-purpose Freshet minimum" is
implemented and tested: specified numeric/control semantics; no known
primitive miscompilation; definite return; structs/enums/tagged unions;
fixed arrays + slices/spans; generics; `break`/`continue`/`match`;
strings + formatting; `Option`/`Result`; file/console/argv APIs; module
system + multi-file builds; documented C FFI with callbacks; a clear
safe/unsafe model; formatter + test runner + LSP navigation + debug
info; strict-mode-able performance diagnostics.

Of the §13 milestone programs, the first six (command-line calculator,
CSV statistics, configuration reader, JSON parser, ring buffer,
multi-module application) are now expressible without ad hoc scripts or
manual C declarations per operation, with two honest caveats: there is
no generic `Vector<T>` yet (`String` is the only growable buffer;
fixed arrays and `Span<T>` cover the rest) and no numeric text parsing
(`fmt_i64`/`fmt_f64` format; `parse_*` is still to come).

---

## 11. Build system — one source list, two executables (LSP link failure)

The previous delivery shipped a real defect: `src/generics.cpp` was
added to the compiler's CMake source list but **not** to
`freshet-language-server`'s, so a CMake user's
`cmake --build --target freshet-language-server` failed at link time
with undefined references to `freshet::ensurePreludeString`,
`freshet::bindTypeEncoding` and `freshet::instantiateGenericFn` (all
three live in `generics.cpp`; the sandbox build had hidden this because
`scripts/build.sh` spelled its own, correct, list).

Fixed structurally in `CMakeLists.txt`: one shared `FRONTEND_SOURCES`
list now feeds **both** executables —

- `penstock` = `FRONTEND_SOURCES` + `src/evo.cpp` + `src/main.cpp`
- `freshet-language-server` = `FRONTEND_SOURCES` + `src/lsp/server.cpp`

— so a future front-end file can never land in one target only.
`scripts/build.sh` mirrors the same shared list, and its LSP link step
is now fatal on failure (it previously printed the error and exited 0).
Both build systems were re-verified from a clean configure through
`--target check` (134/134).

---

## 12. Latent defects found and fixed beyond the report

1. **By-value struct parameters silently miscompiled**: the definition
   emitted `%struct.X %param` while every call site passed `ptr`;
   `sum2(P{1.5, 2.5})` returned `1.5`. The spec's "parameters pass by
   address (callee sees a copy)" convention is now implemented on both
   sides. (`tests/pos_structparam.fsh`.)
2. **Constant-expressions stored into narrower slots** skipped the width
   cast (`store i32 %i64reg` — malformed IR).
3. **`x /= 8` emitted `div`** (not even a valid LLVM opcode) — compound
   assignment now shares the exact binary lowering, including signed
   strength reduction and shifts.
4. **Two `return`s in one function** silently skipped defers on the
   second path (leak on that path): `emitDeferredAll` no longer clears
   the stack.
5. **Skipped/overlapping `%N` register numbers** (identity casts, `/1`,
   slice defaults) made LLVM reject whole functions.
6. **Mixed signedness/width comparisons** picked `ult` vs `slt` from the
   wrong type.
7. **`bool` parameters** were uncallable (i1 argument vs i8 signature).
8. **Return-position literal adoption** was missing (`fn f() -> i64 {
   return 10; }` did not type-check).
9. **The C-style `for` loop and `give(...)` call arguments never
   parsed**; `sizeof`/`alignof` never resolved (§2.4 above).
10. **Unrolling could drop `break`** (constant-trip loops containing
    breaks were unrolled with the early exits lost) — now vetoed.

---

## 13. Remaining gaps (honest list)

- No generic `Vector<T>` dynamic array (growables = `String` only).
- No numeric text parsing (`parse_i64` etc.); formatting exists.
- Variadic `extern fn` not supported (the report defers these too).
- C ABI bindings are declared, not generated.
- LSP has hover/definition/references/rename; completion, formatting
  and inlay hints are future work.
- Phase 5 optional facilities (§9.3 above) remain future work.

The optimisation-and-proof audit was integrated in the same version;
its own change report (`docs/optimization-integration.md`) documents
those 20 tests and the
`--licm/--partial-unroll/--peel/--interchange/--tile/--distribute/
--unroll-and-jam/--parfor-advisor` transform set with `--proof-log`.

---

## 14. Verification

- Environment: penstock built with **both** g++ 14 (via
  `scripts/build.sh`) and clang++ 19 (the CMake default host compiler);
  backend clang 19.1.7; the full pipeline (preprocess → parse → sema →
  checks → ownership → module loading → loop opts → IRgen → clang →
  binary → execute) runs on every test.
- `tests/run.sh` / `cmake --build --target check`:
  **134 passed, 0 failed** — 72 pre-existing tests (the two policy
  tests migrated with new expectations and strict-mode twins), 23 new
  Phase 0–2 tests, 20 optimisation-audit tests, 19 Phase 3–5 tests.
- CMake path re-verified end-to-end from a clean configure: smoke test,
  `penstock`, `freshet-language-server`, `libfsh`, runtime IR cache,
  `check` — all green; and again from the re-extracted tarball.
- Differential checks retained: -O0 vs -O3 output equality, PGO
  round-trips, fusion/unroll/lane output-preservation, soa=auto
  veto/transform identity, `@reproducible` bit-identity across thread
  counts, formatter token-identity gate — all still green with the new
  features in the tree.

Every change above is in this source tree; the roadmap mapping is
item-by-item as listed in sections 2–10.
