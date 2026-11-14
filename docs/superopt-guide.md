# Penstock's embedded Freshet superoptimiser

Penstock's instruction optimiser is built into the compiler. It does not load
Souper, invoke `opt -souper`, launch `alive-tv`, or find a `z3` executable.
The `-O3` and `-O4` pipelines call a Freshet control loop linked into the
compiler; native library bindings provide LLVM IR operations, Z3 bit-vector
queries, and Alive2 translation validation.

```sh
bin/penstock -O3 program.fsh -o app
bin/penstock -O4 --superopt-budget=10 program.fsh -o app
```

## Build stages

CMake deliberately uses a three-stage bootstrap:

1. `penstock-bootstrap` is built from the normal compiler sources with
   superoptimisation disabled.
2. That bootstrap compiler compiles `src/superopt.fsh` to a native object.
3. The final `penstock` AND `freshet-language-server` executables link the
   Freshet object, the native LLVM bridge, and the shared Z3/Alive2/LLVM
   dynamic libraries compiled from the `third_party/` submodules.

Both binaries therefore run the Freshet optimiser, Alive2 verifier and Z3
solver against one shared dynamic-library ABI. The build also installs those
shared libraries (plus the bridge archive, headers and licence files) for
downstream CMake consumers. The compiler still uses the selected Clang/LLVM
toolchain as its normal code-generation backend; that is separate from the
embedded superoptimiser.

## What the current search covers

The current bounded candidate generator focuses on integer strength-reduction
idioms in LLVM IR: multiplication by zero, one or a power of two; unsigned
division by one or a power of two; and unsigned remainder by a power of two.
It does not rewrite overflow-flagged or exact operations. The candidate set is
intentionally conservative and is capped at 500 candidates for `-O3` and
4,000 for `-O4`. Widths above 64 bits are skipped.

For each proposed rewrite:

1. Z3 checks the corresponding fixed-width bit-vector identity by asking
   whether a counterexample exists. Only `unsat` is accepted.
2. Penstock materialises the candidate in a cloned LLVM module and runs the
   LLVM verifier.
3. Alive2 checks the changed function's refinement against the current
   accepted module.
4. The configured cost objective must improve before the rewrite is retained.

Each accepted rewrite becomes the incumbent for the next candidate. The
compiler writes the final module atomically; any parse, SMT, validation, budget
or write failure leaves the previous IR intact. This is a bounded local
superoptimisation engine, not yet a general-purpose Souper-equivalent
synthesis search over arbitrary expression DAGs.

Alive2 supports function-local transformations; the engine therefore changes
one function at a time and does not attempt interprocedural rewrites. Its LLVM
utility and SMT initialisers are held in one process-wide session; the SMT
context is reset between proofs and finalised only when Penstock exits. Proof
calls are serialised because Alive2's state is process-global.

## Budgets and objectives

`--superopt-budget=<sec>` sets the per-compilation budget (default 2 seconds at
`-O3`, 10 seconds at `-O4`). `-O3` is capped at 256 KiB of input IR and 500
candidates; `-O4` allows 4 MiB and 4,000 candidates.

`--superopt-objective=` selects the acceptance rule:

| Objective | Acceptance rule |
|---|---|
| `latency` | Lower weighted latency, with no more than 25% instruction growth |
| `size` | Fewer instructions and no latency regression |
| `balanced` | Either of the two improvements above |

`--no-superopt` disables the Freshet stage. `--keep-ir` preserves the normal
compiler's intermediate IR and diagnostic artefacts.

## CMake options and dependencies

| Option | Default | Purpose |
|---|---|---|
| `PENSTOCK_BUILD_EMBEDDED_SUPEROPT` | `ON` | Build the Freshet engine and embedded solver/verifier |
| `PENSTOCK_USE_SUBMODULES` | `ON` | Prefer the `third_party/{z3,alive2,llvm}` submodule checkouts when present; FetchContent is the fallback |
| `PENSTOCK_Z3_GIT_TAG` | `z3-4.15.3` | Z3 source revision used by the FetchContent fallback (the submodule gitlink pins the actual build) |
| `PENSTOCK_ALIVE2_GIT_TAG` | `master` | Alive2 source revision used by the FetchContent fallback (the submodule gitlink pins the actual build) |
| `PENSTOCK_ALIVE2_LLVM_GIT_TAG` | `main` | LLVM branch tracked by the `third_party/llvm` submodule (and the FetchContent fallback) |
| `PENSTOCK_FETCH_LLVM` | `ON` | Fetch a matching LLVM development SDK when necessary |
| `PENSTOCK_BUILD_RTTI_LLVM` | `ON` | Build a private LLVM revision for Alive2 with RTTI/EH, even when the backend SDK already has them |
| `PENSTOCK_RTTI_LLVM_SOURCE_DIR` | empty | Use an existing `llvm-project` checkout matching `PENSTOCK_ALIVE2_LLVM_GIT_TAG` |
| `PENSTOCK_ALIVE2_LLVM_BUILD_DIR` | `build/_deps/penstock-alive2-llvm-build` | Private LLVM build directory used by the verifier |
| `PENSTOCK_RTTI_LLVM_TARGETS_TO_BUILD` | `Native` | LLVM code-generation backends built in the private SDK |

The build requires a matching LLVM development SDK for Penstock's selected
Clang backend. Alive2's LLVM adapter requires RTTI and exception handling, and
its development branch tracks LLVM `main` rather than necessarily the latest
numbered LLVM release. By default CMake therefore uses the `third_party/llvm`
submodule (or, without submodules, fetches `PENSTOCK_ALIVE2_LLVM_GIT_TAG`)
and builds it privately with both RTTI and exceptions enabled, even when the
backend SDK already has those features. The private build supplies the LLVM
headers/libraries for the in-process verifier and the matching `opt` host when
the optional O4 plugin is enabled. Penstock's normal `LLVM_ROOT` and Clang
backend remain unchanged.

## Submodule sources

Z3, Alive2 and the verifier LLVM are git submodules of this repository:

```sh
git clone --recurse-submodules <penstock-url>   # or, in an existing checkout:
git submodule update --init                     # (add --depth 1 for the
                                                #  shallow third_party/llvm)
```

The pinned gitlink revisions — not the `PENSTOCK_*_GIT_TAG` cache strings —
select what is compiled from `third_party/z3`, `third_party/alive2` and
`third_party/llvm`. Move a pin with:

```sh
git -C third_party/alive2 checkout <revision>   # e.g. origin/master
git add third_party/alive2
```

`third_party/llvm` is marked `shallow = true` in `.gitmodules` so a fresh
`submodule update` fetches a depth-1 clone of the monorepo; unshallow it with
`git -C third_party/llvm fetch --unshallow` when history is needed. Setting
`-DPENSTOCK_USE_SUBMODULES=OFF` (or an empty `third_party/`, as in the release
tarball) makes the configure fetch the same revisions via FetchContent
instead.

## Dynamic libraries

Z3 and Alive2 are compiled as SHARED libraries — `libz3.so`, `libir.so`,
`libpenstock_alive2_smt.so`, `libtools.so`, `libpenstock_alive2_util.so` and
`libllvm_util.so` — and the verifier LLVM is built as the monolithic
`libLLVM.so` with RTTI/EH. `penstock`, `freshet-language-server` and the O4
`opt` plugin all link this one set of dynamic libraries, so a single solver
and LLVM instance serve every Penstock process. The binaries carry
`$ORIGIN`-relative RPATH entries resolving to the installed
`lib/penstock/embedded/lib` directory, which contains `libLLVM.so`,
`libz3.so` and the Alive2 shared libraries next to the optional `opt` host.
Use that installed `opt` with Penstock's plugin; do not load the plugin into
the unrelated system `opt`. The bundled Z3 carries the upstream
`libz3.so.4.15` SONAME (major.minor), so it never collides with a system
`libz3.so.4`; still, the RPATH guarantees the bundled build is preferred.

Z3's internal `opt` component is namespaced in the build tree because LLVM
already exports an imported executable target with that name; its API
component reference is updated at the same time. Alive2's `smt` and `util`
targets are likewise namespaced as `penstock_alive2_smt` and
`penstock_alive2_util`, since Z3 already owns the unqualified target names in
the shared CMake build; the same configure pass rewrites their `STATIC`
library kinds (and those of `ir`, `tools` and `llvm_util`) to `SHARED`.
Alive2 is pointed at the in-tree `z3::libz3` target through a small config
package, avoiding a configure-time probe for a library that has not been
built yet. These adjustments patch the submodule checkouts in place and are
idempotent; restore a checkout with `git submodule update --checkout` and
the next configure simply reapplies them. The build-time `re2c` tool is not
a runtime dependency.

```sh
git submodule update --init
cmake -S . -B build -DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=ON
cmake --build build --parallel
cmake --install build
```

`third_party/llvm` supplies the verifier LLVM by default. To point at
another `llvm-project` checkout instead (matching the selected Alive2
revision and having RTTI/EH enabled):

```sh
cmake -S . -B build \
  -DPENSTOCK_RTTI_LLVM_SOURCE_DIR=/path/to/llvm-project
```

The private LLVM build is substantial: it builds the LLVM shared library and,
when the O4 plugin is enabled, the matching `opt` executable. Disable it with
`-DPENSTOCK_BUILD_RTTI_LLVM=OFF` only when `LLVM_DIR` points at a checkout
whose C++ API revision matches the selected Alive2 revision as well as having
RTTI and exception handling enabled. The default `main` plus `master` pairing
should normally be left together.

No `PENSTOCK_SOUPER_PASS`, `PENSTOCK_Z3`, or `PENSTOCK_ALIVE_TV` environment
variables are used. The bounded finite-domain precomputation stage also calls
the embedded Alive2 API; its evaluator still uses the selected Clang backend
and the system `timeout` utility to limit evaluator compilation/execution.
