<div align=center>
  <img width="1086" height="362" alt="penstock logo" src="https://github.com/user-attachments/assets/4ddc97b5-e9bd-42cf-ae5e-9cb996b5b0e0" />
</div>

# Freshet / Penstock

**Freshet** is a strictly-typed, strictly-checked HPC language with C++/Rust-like
syntax and C-level control over data layout.  Source files use the `.fsht`
extension (renamed from `.fsh`, which clashed with GLSL fragment shaders). **Penstock** is its official compiler:
a source-level loop-optimising, superoptimising pipeline that lowers Freshet to
LLVM IR, drives Clang as its backend, and treats *predictable* performance bugs
as compile-time diagnostics.

```
penstock [options] input.fsht [more.fsht ...]
```

## Highlights

- **Performance bugs are diagnostics.** Div/rem shapes, cache-line straddles,
  layout padding and loop shapes warn by default and become hard errors under
  `--strict-performance` (`F0401`–`F0406`; every code has a long-form page via
  `penstock --explain F0404`).
- **C-level layout control.** Explicit AoS/SoA (`soa(Body)`, automatic
  conversion advice via `--soa=warn|auto|aosoa`), fixed-size arrays, borrowed
  spans, `align(64)` fields, layout tables in `--report`.
- **`parfor` with deterministic reductions.** Work-stealing ring-buffer worker
  pool; `[reduction(+:acc)]` regions are order-independent by construction;
  `@reproducible` builds are bit-identical across thread counts.
- **Embedded Freshet superoptimisation.** CMake bootstraps a no-superopt
  compiler, uses it to compile the Freshet optimisation loop, then links that
  loop with static Z3 and in-process Alive2 into the final executable. No
  Souper, `alive-tv`, or `z3` runtime executable is required — see
  [docs/superopt-guide.md](docs/superopt-guide.md).
- **Observability.** `--report` merges [static]/[backend]/[runtime] layers;
  `--opt-remarks`, `--emit-final-ir` and `--save-temps` expose exactly what
  the backend did.
- **Tooling.** `penstock build/test/fmt/lint/run` subcommands and a
  `freshet-language-server` speaking LSP 3.18 over stdio (hover,
  documentSymbol, goto-definition, find-references, rename).
- **Editor support.** VS Code and Vim integrations are provided under
  [`editors/`](editors/README.md), with syntax highlighting and optional
  LSP-backed diagnostics/navigation.

## Quick start

Linux, **CMake ≥ 3.16**. On the first configure with no local LLVM 19–23
toolchain, the build downloads the official LLVM 23.1.4 SDK automatically
(SHA-256-verified, cached under `build/_deps`). To build every dependency
from your own pinned sources instead, check out the vendored submodules
first — the build then compiles Z3, Alive2 and the whole clang backend from
`third_party/` and never touches the network:

```sh
git submodule update --init          # third_party/{llvm,alive2,z3}
cmake -B build                        # configure (submodules > local > fetch)
cmake --build build -j                # bin/penstock + LSP server + libfsh
cmake --build build --target check    # regression suite (or: ctest --test-dir build)

cat > hello.fsht <<'EOF'
struct Body { x: f64, y: f64, m: f64 }
fn main() {
    let n: usize = 1000000;
    let b: soa(Body) = soa_new(Body, n);
    defer soa_free(b);
    parfor (i in 0..n) {
        b[i].x = (i as f64) * 0.5;
        b[i].m = 1.0;
    }
    let mut mass: f64 = 0.0;
    parfor (i in 0..n) [reduction(+:mass)] { mass = mass + b[i].m; }
    print_f64(mass, 2); println();     // 1000000.00
}
EOF
bin/penstock --run hello.fsht
```

Daily drivers:

```sh
bin/penstock program.fsht -o app              # build (-O3 default)
bin/penstock --report program.fsht -o app     # + performance report
bin/penstock -O4 --superopt-budget=10 program.fsht -o app   # verified superopt
```

Backend pinning (`-DLLVM_ROOT=/path/to/llvm-20`), offline builds, installation
(`cmake --install build`) and CMake package usage are covered in
[docs/install.md](docs/install.md); the full option surface lives in the
[language spec](docs/language-spec.md) and `penstock --help`.

### Troubleshooting: `penstock: cannot compile runtime library`

The runtime support library (`runtime/frt_rt.c`, plain C with platform
layers) is compiled by the driver with the selected clang and cached in
`build/` (or `~/.cache/penstock` when installed). This error means that
one compile failed — and the driver reports clang's own diagnostic and
the exact failing command underneath the message, so read the first
`error:` line there. The usual causes:

- **Incomplete clang** — `'stddef.h' file not found` (or `stdint.h`,
  `stdarg.h`, …): the selected toolchain is missing its *builtin
  headers* from `<llvm>/lib/clang/<major>/include`. Debian/Ubuntu:
  `apt install libclang-common-<major>-dev`; source builds: use an LLVM
  install prefix created by a full `ninja install`. `LLVM_ROOT` selects
  which clang is used.
- **Unsupported `--target`** — `unknown target triple`: this clang build
  has no backend for the requested triple.
- **Unwritable cache** — `unable to open output file`: set
  `PENSTOCK_CACHE_DIR` to a writable path.

The cache key covers the LLVM major, `-O` level and target triple, so a
toolchain switch recompiles the runtime automatically; deleting
`build/frt_rt_*.ll` (or `~/.cache/penstock/frt_rt_*.ll` when installed)
forces a rebuild.

- **Incomplete installed runtime** — `'platforms/linux/frt_platform.c'
  file not found` (or `common/…`): an installed Penstock whose
  `share/penstock/runtime/` holds only `frt_rt.c` (install rules from
  before the runtime was decomposed into `common/` + `platforms/`).
  Re-run `cmake --install build` with the fixed install rules, or copy
  the whole `runtime/` directory from a checkout over the installed one.

## Documentation

| Guide | Contents |
|---|---|
| [language-spec.md](docs/language-spec.md) | Language reference — types, attributes, structs, memory model, `parfor`, diagnostics index, loop passes, modules, FFI, `fsh` stdlib |
| [superopt-guide.md](docs/superopt-guide.md) | `-O3`/`-O4` embedded Freshet/Z3/Alive2 superoptimisation: bootstrap, scope, budgets and cost-vector acceptance |
| [pgo-guide.md](docs/pgo-guide.md) | Profile-guided optimisation (custom counters + LLVM IR PGO) |
| [proof-guide.md](docs/proof-guide.md) | Verification layers: Lean kernel, `--proof-log` premises, Alive2 |
| [evo-guide.md](docs/evo-guide.md) | Legacy `-O4` runtime tuner (opt-in `--evo-runtime-tune`) |
| [precompute-guide.md](docs/precompute-guide.md) | Finite-domain algorithm precomputation |
| [perf-report-guide.md](docs/perf-report-guide.md) | Reading `--report` output |
| [libfsh-guide.md](docs/libfsh-guide.md) | The libfsh C API (embed Freshet's runtime) |
| [opt-plugin.md](docs/opt-plugin.md) | Penstock's O4 pipeline as an LLVM `opt` plugin |
| [install.md](docs/install.md) | Installation, `PATH`, CMake package (`Penstock::fsh`) |
| [optimization-integration.md](docs/optimization-integration.md) · [report-integration.md](docs/report-integration.md) | Change reports mapping the optimisation and language audits to the implementation |
| [checklist-audit.md](docs/checklist-audit.md) | The 28-item roadmap checklist audited item-by-item against both repositories, with the latent defects the audit fixed |
| [errors/](docs/errors/) | Long-form diagnostic pages behind `penstock --explain <ID>` |

## Repository layout

```
src/            compiler (C++17): lexer → preprocessor → parser → sema →
                strict checks → ownership → loop passes → IRgen → PGO;
                libfsh.c (public C API), lsp/server.cpp (LSP)
include/fsh/    public libfsh C API headers
runtime/        libfreshet runtime (frt_rt.c): memory, SoA planes,
                worker pool, parfor dispatch, PGO counters, timing
modules/        the `fsh` standard-library module
plugins/opt/    optional LLVM opt pass plugin (PenstockOptPlugin)
lean/           Lean proof-kernel sources (PenstockProofs.lean)
cmake/          build modules — LlvmBootstrap, PenstockTargets,
                PenstockOptPlugin, PenstockTests, PenstockInstall —
                plus the installed package files
tests/          run.sh regression suite (pos_/neg_/warn_), bench.sh
                benchmarks (AoS/SoA, fusion, cross-lane), tests/proj
                manifest build; opt-in difftest + property tester
scripts/        cmake-free fallback build (build.sh), Lean check,
                differential and property-based testers
docs/           the guides above + long-form error pages
```

Runtime environment variables: `FRT_THREADS` (worker-pool size),
`FRT_AFFINITY=1` (pin workers), `FRT_CHUNK` (default parfor chunk),
`FRT_SERIAL_CUTOFF` (run small parfor regions inline; default 256),
`FRT_TIMING=1` (write `<exe>.frtiming`), `FRT_PROF_OUT` (PGO profile path),
`FRT_SMT_INTERLEAVE=1` (SMT memory/compute lanes: route memory- and
compute-class parfor chunks to sibling hardware threads of one physical
core, paired by `--smt-interleave`).

## Freshet-lang: the standard, the stress suite, the benchmarks

The language is specified and stress-tested by the standards repository
[Freshet-lang](https://github.com/OWNER/Freshet-lang) (current standard:
`fsht-03:03`). This repository consumes it in two places:

- **`.github/workflows/freshet-conformance.yml`** — every PR builds
  Penstock and runs the Freshet-lang conformance suite (corpus goldens,
  negative tests, property checks, differential fuzz) against it.
- **`.github/workflows/freshet-benchmarks.yml`** — a self-hosted,
  perf-capable runner benchmarks Penstock against **idiomatic C++,
  hand-tuned C++ and safe Rust** on the 12 HPC kernels of the standards
  repository, then annotates, in detail, every kernel where another
  language won: the final optimised LLVM IR of the losing function from
  both implementations, source-annotated disassembly, a measured
  instruction census, and `perf` cycle attribution for both binaries.
  Hosted runners have no hardware performance counters, so this workflow
  deliberately runs only on `[self-hosted, perf]` runners — it skips
  rather than fabricating cycle numbers.

The same comparison runs machine-locally with one command after cloning
this repository:

```sh
tools/lang-bench/run.sh                 # builds penstock if needed,
                                         # clones Freshet-lang if needed,
                                         # runs bench.py + annotate.py
tools/lang-bench/run.sh --perf off      # static annotation only
tools/lang-bench/run.sh --kernels dot,matmul
```

Set `FRESHET_LANG_REPO` (or the workflow input / `--freshet-lang` flag)
to your `owner/Freshet-lang` checkout before the first run — the
shipped default is the placeholder `OWNER/Freshet-lang`.

## Status

Version `0.1.0-INDEV`. The integration reports document what is
implemented and what is deliberately deferred (variadic `extern fn`, and
the registry-tracked deferrals: custom allocators, bulk-copy container
APIs, associated types, dynamic dispatch, the package manager):
[docs/report-integration.md](docs/report-integration.md),
[docs/optimization-integration.md](docs/optimization-integration.md) and
[docs/checklist-audit.md](docs/checklist-audit.md) (the 28-item roadmap
audit mapping every checklist item to both repositories).

### fsht-02:01 — dynamic collections (ratified 2026-11-19)

The first increment of the fsht-02:01 roadmap
(registry `fsht-02:01-02.09.01` / `fsht-02:01-02.09.02`) is in the tree:
`Vec<T>` — a prelude generic struct `{ data: *T, len: u64, cap: u64 }`
monomorphised per element type — with the full container surface
(`vec_new` / `vec_from_span` / `vec_from_array` / `vec_clone`,
`&mut` mutators, checked indexing `v[i]`, `Option<T>` access, and
borrowed `Span<T>` views via `v[lo..hi]` / `vec_slice`), plus the
ownership model the registry demands: by-value transfers move (the
source binding is invalidated — F0301 on later use), every owned
binding drops at scope exit in reverse declaration order (after the
scope's `defer`s, flushed on `break`/`continue`/return), manual
`vec_free` pre-empts the drop, and rebinding while owning is an error.
Conformance: `tests/pos_vec_*.fsht` and `tests/neg_vec_*.fsht` in the
regression suite; the spec chapter is
`spec/20-dynamic-collections.md` in the Freshet-lang repository
(normative since fsht-02:01).

Deferred with the registry entries: containers nested in user
aggregates, owning element types (`Vec<String>`), custom allocators,
and the `String` migration onto the drop model (its `str_of` views
routinely escape; that waits for the fsht-03:02 lifetime analysis,
which is also what will lift the restriction on returning views of
local containers). The `Slice`/`SliceMut` naming question was closed
at ratification: container views are `Span<T>`, no separate slice
types exist in the standard.

### fsht-02:02/03 — traits and const generics (ratified 2026-11-19)

The next two roadmap increments are in the tree (registry
`fsht-02:02-06.02.01` / `fsht-02:03-06.03.01`):

**User-defined traits, static dispatch** — `trait Numeric { fn
add(a: Self, b: Self) -> Self; ... }` declarations (pure signature
sets, `Self` the implementing-type placeholder) and `impl Trait for
Type` blocks whose members become ordinary functions under mangled
names (`Numeric$i64$add`) — so effects, checks, ownership, IRgen and
LLVM all see plain direct calls: no vtables, no trait objects, the
HPC philosophy preserved. Calls resolve three ways:
`Trait::method(args)` inside generic fns (rewritten per instantiation
against the bound tparam), by first-argument type in concrete code
(F0216 when undetermined), `Type::method` qualified calls, and
`Self::method` inside impls. Bounds compose with `+`
(`T: Numeric + Scalable`); a missing impl is F0218, a signature
mismatch after Self substitution is F0217.

**Const generics + const fn** — `const N: usize` parameters on
structs/enums/fns (`struct Vector<T, const N: usize> { data: [T; N] }`,
`Matrix<T, const R: usize, const C: usize> { data: [T; R * C] }`);
const arguments accept literals, const expressions and const fn calls
(`Vector<f64, square(3)>`), distinct values are distinct interned
types; array lengths accept const expressions; const parameters
are inferred from array-typed call arguments and fold to literals
inside instance bodies (constant-trip loops unroll). `const fn`
runs at compile time (literals, arithmetic, comparisons, casts, let,
if, while, for-in, assignment, calls to other const fns,
sizeof/alignof values; budgeted at 16384 steps / 32 recursion levels)
while staying an ordinary runtime function; `const_assert(expr)`
asserts at compile time (F0219 non-const, F0220 false).

Conformance: `tests/pos_traits.fsht`, `tests/pos_constgen.fsht`,
`tests/neg_traits_*.fsht`, `tests/neg_constgen_*.fsht` in the
regression suite; the spec chapters are `spec/21-traits.md` and
`spec/22-const-generics.md` in the Freshet-lang repository (normative
since fsht-02:02/fsht-02:03). New diagnostics F0216–F0220 are
documented in `docs/errors/`.

Deferred with the registry entries: blanket impls over template
parameters, generic traits, associated types, dynamic dispatch;
const parameters beyond usize, const inference from container-typed
arguments, `sizeof(T)` inside const fn bodies.

### fsht-03:01–fsht-03:03 — stdlib, borrows & lifetimes, portable
runtime (ratified 2026-11-19)

The three fsht-03 increments are in the tree (registry
`fsht-03:01-05.04.01/05.05.01/05.06.01/08.01.04`,
`fsht-03:02-03.02.09/03.02.10`, `fsht-03:03-10.03.02`):

- **fsht-03:01 expanded standard library** — `parse_i64/u64/f64` over
  `Result<T, ParseError>` (with the MUST-level round-trip law),
  buffered streaming I/O (`Reader`/`Writer`, `IoError`, ownership-
  tracked handles), the checked/saturating/wrapping variant family
  with `checked_cast`/`saturating_cast`, and the ten-module stdlib
  decomposition (`core/alloc/collections/string/math/io/numeric/time/
  bits/random`, `fsh` façade kept).
- **fsht-03:02 borrows & lifetimes** — `&mut`/`&` borrow expressions
  with `F0310` exclusivity (one writer or many readers), `F0311`
  escape analysis (borrows/views must not outlive their storage),
  the `String` migration onto the ownership-tracked container model
  (auto-drop, move invalidation, pre-emptive `string_free`), drop
  elaboration through user shapes (`Vec<String>`, `Vec<Vec<T>>`, struct
  fields, arrays), and `usize` resolved as the fixed 64-bit integer on
  every target.
- **fsht-03:03 portable runtime** — the runtime decomposes into
  `common/` + `platforms/{linux,windows,wasm,baremetal}`;
  same-target runtime compilation (the `--target` triple in the
  runtime cache key), platform-conditional link lines, serialised
  `parfor` on wasm with `@reproducible` output unchanged; the
  five-target matrix is gated by the standards repository's
  `targets.yml` workflow (tested-before-declared).

Conformance: `tests/pos_parse.fsht`, `pos_checked_{variants,cast}.fsht`,
`pos_io_stream.fsht`, `pos_modules_stdlib.fsht`,
`pos_string_migration.fsht`, `pos_shapes.fsht`, `pos_borrow_mut.fsht`,
the `neg_borrow_*`/`neg_string_*` families and the fsht-03:03 target
checks in `tests/run.sh`; the standards-side corpus 20/21/22,
negatives n26–n31 and props p08/p09. Spec chapters 23–25 are
normative since fsht-03:01/02/03.

### fsht-04 working set (checklist audit 2026-11-22)

The maintainer's 28-item roadmap checklist was audited item-by-item
against both repositories — [docs/checklist-audit.md](docs/checklist-audit.md)
has the full mapping. Two fsht-04 items landed early as a
pre-ratification preview (registry 07.04.01 in development,
10.04.01's tooling increment):

- **automatic C header generation** — `penstock --emit-c-header[=<f>]`
  writes an ABI-mirroring header for the `export fn` surface
  (prototypes, struct bodies, tagged-union enums, `fsh_span_T`
  typedefs, by-address aggregate parameters); the generated header is
  itself valid C (consumer compile in `tests/run.sh`).
- **LSP completions and inlay hints** — `textDocument/completion`
  (scope-aware, prefix-filtered) and `textDocument/inlayHint`
  (parameter-name hints at call sites, let-type hints) in
  `freshet-language-server`, conformance
  `scripts/lsp_completion_inlay_check.py`.

The audit also fixed five latent defects the fsht-03 increments had
left in the tree (fusion's induction-variable identity bug — a
wrong-code bug producing uninitialised offsets — the PGO metadata-id
collision in the combined module, a stale `pos_strings` shape, the
builtin/generic `clamp` shadowing that silently degraded
`pos_generics`' coverage, and the SMT unit test's include path),
taking the regression suite from 220/16 to **237 passed, 0 failed**
and the standards conformance suite to **63 passed, 0 failed**.
