<div align=center>
  <img width="1086" height="362" alt="penstock logo" src="https://github.com/user-attachments/assets/cd0eb0a9-2abe-4bb2-b899-6f7a06ad9434" />
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
[Freshet-lang](https://github.com/PolskaKrowa/Freshet-lang) (current standard:
`fsht-01:01`). This repository consumes it in two places:

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
shipped default is the placeholder `PolskaKrowa/Freshet-lang`.

## Status

Version `0.1.0-INDEV`. The two integration reports document what is
implemented and what is deliberately deferred (variadic `extern fn`, numeric
text parsing, LSP completions/inlay hints):
[docs/report-integration.md](docs/report-integration.md) and
[docs/optimization-integration.md](docs/optimization-integration.md).
