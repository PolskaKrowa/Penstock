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
3. The final `penstock` executable links the Freshet object, the native LLVM
   bridge, Alive2's static libraries, and static Z3.

The final executable therefore contains the Freshet optimiser, Alive2 verifier
and Z3 solver. The build also installs the corresponding static archives,
headers and licence files for downstream CMake consumers. The compiler still
uses the selected Clang/LLVM toolchain as its normal code-generation backend;
that is separate from the embedded superoptimiser.

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
one function at a time and does not attempt interprocedural rewrites.

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
| `PENSTOCK_Z3_GIT_TAG` | `z3-4.15.3` | Z3 source revision, statically linked |
| `PENSTOCK_ALIVE2_GIT_TAG` | `master` | Alive2 source revision fetched and linked into Penstock |
| `PENSTOCK_ALIVE2_LLVM_GIT_TAG` | `main` | LLVM revision paired with Alive2 master |
| `PENSTOCK_FETCH_LLVM` | `ON` | Fetch a matching LLVM development SDK when necessary |
| `PENSTOCK_BUILD_RTTI_LLVM` | `ON` | Build a private LLVM revision for Alive2 with RTTI/EH, even when the backend SDK already has them |
| `PENSTOCK_RTTI_LLVM_SOURCE_DIR` | empty | Use an existing `llvm-project` checkout matching `PENSTOCK_ALIVE2_LLVM_GIT_TAG` |
| `PENSTOCK_ALIVE2_LLVM_BUILD_DIR` | `build/_deps/penstock-alive2-llvm-build` | Private LLVM build directory used by the verifier |
| `PENSTOCK_RTTI_LLVM_TARGETS_TO_BUILD` | `Native` | LLVM code-generation backends built in the private SDK |

The build requires a matching LLVM development SDK for Penstock's selected
Clang backend. Alive2's LLVM adapter requires RTTI and exception handling, and
its development branch tracks LLVM `main` rather than necessarily the latest
numbered LLVM release. By default CMake therefore fetches
`PENSTOCK_ALIVE2_LLVM_GIT_TAG` (default `main`) and builds it privately with
both RTTI and exceptions enabled, even when the backend SDK already has those
features. The private build supplies the LLVM headers/libraries for the
in-process verifier and the matching `opt` host when the optional O4 plugin is
enabled. Penstock's normal `LLVM_ROOT` and Clang backend remain unchanged.

Z3 and Alive2 are fetched and built as static libraries. Z3's internal
`opt` component is namespaced in the fetched build tree because LLVM already
exports an imported executable target with that name; its API component
reference is updated at the same time. Alive2's `smt` and `util` targets are
likewise namespaced as `penstock_alive2_smt` and `penstock_alive2_util`, since
Z3 already owns the unqualified target names in the shared CMake build. The
Alive2 dependency list and Penstock link/install rules are updated with those
names. Alive2 is pointed at the in-tree `z3::libz3` target through a small
config package, avoiding a configure-time probe for an archive that has not
been built yet. The private verifier LLVM's shared library
and (when enabled) matching `opt` host are installed under
`lib/penstock/embedded`, with relative runtime search paths. Use that installed
`opt` with Penstock's plugin; do not load the plugin into the unrelated system
`opt`. The build-time `re2c` tool is not a runtime dependency.

```sh
cmake -S . -B build -DPENSTOCK_BUILD_EMBEDDED_SUPEROPT=ON
cmake --build build --parallel
cmake --install build
```

To use a local LLVM source checkout instead of having CMake fetch the
configured Alive2-compatible LLVM revision (by default `main`):

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
