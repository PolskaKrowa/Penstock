# Penstock's LLVM `opt` plugin

Penstock can expose its LLVM-IR-compatible `-O4` stages as a loadable LLVM new
Pass Manager plugin. This lets projects that already emit LLVM IR use the
Penstock pipeline without translating their code through the Penstock/Freshet
source language.

## Build

The build needs the LLVM development SDK matching the major version of the
`opt` executable that will load the plugin; a plugin built against LLVM 20 must
not be loaded by LLVM 19 or LLVM 21 `opt`. CMake now obtains these files
automatically on Linux: it reuses a complete local SDK, or downloads the
matching official LLVM release archive (including `PassPlugin.h`, the LLVM
libraries, `opt`, and Clang) when the SDK is missing.

```sh
cmake -S . -B build -DPENSTOCK_BUILD_OPT_PLUGIN=ON
cmake --build build --target PenstockOptPlugin --parallel
```

To use an existing installation instead, set `LLVM_ROOT` (and optionally
`LLVM_DIR`). To prevent network access, set `-DPENSTOCK_FETCH_LLVM=OFF`; in
that case the matching development SDK must already be installed. Set
`-DPENSTOCK_BUILD_OPT_PLUGIN=OFF` to avoid building the plugin altogether.

The generated plugin is normally:

```text
build/plugins/opt/PenstockOptPlugin.so
```

Install it with the rest of Penstock:

```sh
cmake --install build --prefix "$HOME/.local"
```

The installed path is `${prefix}/lib/penstock/plugins/PenstockOptPlugin.so`.
On systems with a multiarch `CMAKE_INSTALL_LIBDIR`, replace `lib` with that
configured directory.

If the automatic download is disabled or unavailable and CMake cannot find
`LLVMConfig.cmake`, `AddLLVM.cmake`, `opt`, and the LLVM C++ headers, the normal
Penstock compiler remains usable and the plugin target is disabled with a
warning. Install the matching LLVM development package and set `LLVM_DIR` to
its `lib/cmake/llvm` directory, or configure with
`-DPENSTOCK_BUILD_OPT_PLUGIN=OFF` to disable plugin building.

### Troubleshooting SDK/header errors

The plugin must be built with the **development headers for the same LLVM major
version as `opt`**. A Clang binary alone is not sufficient. If compilation
reports that `PassPlugin.h` cannot be found, check that the selected SDK
contains either `include/llvm/Plugins/PassPlugin.h` (LLVM 23 and newer) or
`include/llvm/Passes/PassPlugin.h` (older LLVM releases), and that `LLVM_DIR`
points to the matching toolchain rather than another LLVM install.
For example, on Debian/Ubuntu install the matching versioned development package
(e.g. `llvm-20-dev` for LLVM 20), then configure explicitly:

```sh
cmake -S . -B build \
  -DLLVM_ROOT=/usr/lib/llvm-20 \
  -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm \
  -DPENSTOCK_BUILD_OPT_PLUGIN=ON
```

Penstock's finite-domain rewriter is also compiled into the LLVM plugin, whose
LLVM build flags commonly disable C++ exceptions. Its integer-width parser is
therefore exception-free; no `-fexceptions` override is required.

## Run on LLVM IR

For a complete Penstock-style O4 pipeline (LLVM's target-neutral O3 pipeline,
then Penstock's verified IR rewrites):

```sh
/usr/lib/llvm-20/bin/opt \
  -load-pass-plugin="$HOME/.local/lib/penstock/plugins/PenstockOptPlugin.so" \
  -passes=penstock-o4 -verify-each \
  input.ll -S -o output.ll
```

If the project already runs its preferred LLVM pipeline and wants only the
Penstock rewrite stages, use `-passes=penstock-o4-rewrites` instead. Input and
output can be LLVM textual IR or bitcode as supported by that version of `opt`;
`-S` makes the output textual IR.

### Optional solver tools

The plugin reuses Penstock's bounded precomputation and Souper/Alive2 verified
superoptimisation engines. Stock LLVM optimisation still runs if these optional
external tools are not available; Penstock rewrites simply skip themselves.

For the instruction superoptimizer, configure the version-matched Souper pass
and proof tools using the environment variables recognised by the compiler:

- `PENSTOCK_SOUPER_PASS`: path to the Souper LLVM pass plugin built for this
  LLVM version.
- `PENSTOCK_ALIVE_TV`: path to Alive2's `alive-tv` executable.
- `PENSTOCK_Z3`: path to `z3`.
- `PENSTOCK_TIMEOUT`: path to GNU-compatible `timeout`.
- `PENSTOCK_OPT`: override the `opt` executable used for the Souper sub-pass.

The plugin uses `PENSTOCK_O4_BUDGET` as the solver/precompute verification
budget in seconds (default 10, maximum 300). Set `PENSTOCK_O4_DISABLE_PRECOMPUTE=1`
or `PENSTOCK_O4_DISABLE_SUPEROPT=1` to disable either custom stage. Set
`PENSTOCK_O4_KEEP_IR=1` to preserve intermediate IR and diagnostic logs.

## What this pipeline can and cannot do

The plugin runs a standard LLVM O3 optimisation pipeline first, followed by
Penstock's two stages that can operate on LLVM IR directly:

1. **Finite-domain input/output mapping.** For small safe integer functions,
   Penstock exhaustively evaluates the bounded input domain, proposes a
   constant lookup body (including inverse-search mappings), and installs it
   only when Alive2 verifies it.
2. **SMT-backed instruction superoptimisation.** Souper proposes a shorter
   instruction sequence; Alive2 must verify the candidate before it is
   accepted.

Penstock's AST-level loop fusion, source-level unrolling decisions, and
cross-lane branch specialisation happen before IR generation. Arbitrary LLVM IR
has no original Penstock AST, so those exact source transformations cannot be
reconstructed by this plugin. LLVM's normal O3 loop fusion/unrolling/vectoriser
passes still operate on the IR. Likewise, Penstock's optional evolutionary
runtime tuner is not part of an `opt` pass: it selects runtime/build settings by
building and measuring whole programs, not by transforming an LLVM module.

All custom IR rewrites are conservative: no candidate is applied if its
exhaustive evaluator, time budget, external toolchain, or Alive2 proof fails.
