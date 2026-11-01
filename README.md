# Freshet / Penstock

**Freshet** is a strictly-typed, strictly-checked HPC language with C++/Rust-like
syntax and C-level control over data layout. **Penstock** is its official compiler:
a source-level loop-optimising, superoptimising pipeline that lowers Freshet to
LLVM IR, drives Clang as its backend, and treats *predictable* performance bugs
as compile-time diagnostics.

```
penstock [options] input.fsh [more.fsh ...]
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
- **Verified superoptimisation.** At `-O3`/`-O4` Penstock asks Souper + Z3 for
  cheaper equivalent IR and installs a rewrite only when Alive2 proves
  refinement — see [docs/superopt-guide.md](docs/superopt-guide.md).
- **Observability.** `--report` merges [static]/[backend]/[runtime] layers;
  `--opt-remarks`, `--emit-final-ir` and `--save-temps` expose exactly what
  the backend did.
- **Tooling.** `penstock build/test/fmt/lint/run` subcommands and a
  `freshet-language-server` speaking LSP 3.18 over stdio (hover,
  documentSymbol, goto-definition, find-references, rename).

## Quick start

Linux, **CMake ≥ 3.16**. On the first configure with no local LLVM 19–23
toolchain, the build downloads the official LLVM 23.1.4 SDK automatically
(SHA-256-verified, cached under `build/_deps`).

```sh
cmake -B build                        # configure (find or fetch the backend)
cmake --build build -j                # bin/penstock + LSP server + libfsh
cmake --build build --target check    # regression suite (or: ctest --test-dir build)

cat > hello.fsh <<'EOF'
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
bin/penstock --run hello.fsh
```

Daily drivers:

```sh
bin/penstock program.fsh -o app              # build (-O3 default)
bin/penstock --report program.fsh -o app     # + performance report
bin/penstock -O4 --superopt-budget=10 program.fsh -o app   # verified superopt
```

Backend pinning (`-DLLVM_ROOT=/path/to/llvm-20`), offline builds, installation
(`cmake --install build`) and CMake package usage are covered in
[docs/install.md](docs/install.md); the full option surface lives in the
[language spec](docs/language-spec.md) and `penstock --help`.

## Documentation

| Guide | Contents |
|---|---|
| [language-spec.md](docs/language-spec.md) | Language reference — types, attributes, structs, memory model, `parfor`, diagnostics index, loop passes, modules, FFI, `fsh` stdlib |
| [superopt-guide.md](docs/superopt-guide.md) | `-O3`/`-O4` SAT/SMT superoptimisation: budgets, tool setup, cost-vector acceptance |
| [pgo-guide.md](docs/pgo-guide.md) | Profile-guided optimisation (custom counters + LLVM IR PGO) |
| [proof-guide.md](docs/proof-guide.md) | Verification layers: Lean kernel, `--proof-log` premises, Alive2 |
| [evo-guide.md](docs/evo-guide.md) | Legacy `-O4` runtime tuner (opt-in `--evo-runtime-tune`) |
| [precompute-guide.md](docs/precompute-guide.md) | Finite-domain algorithm precomputation |
| [perf-report-guide.md](docs/perf-report-guide.md) | Reading `--report` output |
| [libfsh-guide.md](docs/libfsh-guide.md) | The libfsh C API (embed Freshet's runtime) |
| [opt-plugin.md](docs/opt-plugin.md) | Penstock's O4 pipeline as an LLVM `opt` plugin |
| [install.md](docs/install.md) | Installation, `PATH`, CMake package (`Penstock::fsh`) |
| [optimization-integration.md](docs/optimization-integration.md) · [report-integration.md](docs/report-integration.md) | Change reports mapping the optimisation and language audits to the implementation |
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
`FRT_TIMING=1` (write `<exe>.frtiming`), `FRT_PROF_OUT` (PGO profile path).

## Status

Version `0.1.0-INDEV`. The two integration reports document what is
implemented and what is deliberately deferred (variadic `extern fn`, dynamic
arrays beyond `String`, numeric text parsing, LSP completions/inlay hints):
[docs/report-integration.md](docs/report-integration.md) and
[docs/optimization-integration.md](docs/optimization-integration.md).
