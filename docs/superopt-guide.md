# Penstock SAT/SMT-backed superoptimisation

Penstock now has a verified LLVM IR superoptimisation stage at `-O3` and
`-O4`. It is separate from benchmark-driven runtime tuning.

```sh
bin/penstock -O3 program.fsh -o app
bin/penstock -O4 --superopt-budget=20 program.fsh -o app
```

## Candidate generation and correctness gate

The pass uses the Souper LLVM pass to ask an SMT solver for cheaper equivalent
instruction expressions. It then invokes Alive2 on the exact input and
candidate LLVM modules. A candidate is installed only when all of these hold:

1. Souper and `opt` return success and produce parseable IR.
2. The candidate has fewer LLVM instructions than the original module.
3. `alive-tv` explicitly reports `Transformation seems to be correct!`.

A timeout, missing tool, failed parse, non-improving candidate, counterexample,
or inconclusive Alive2 result leaves the original module untouched. Penstock
never treats execution-time measurements or random testing as proof of
semantic equivalence. Alive2 is translation validation for LLVM IR; it does
not prove arbitrary interprocedural algorithm rewrites, so this stage only
accepts the local-equivalence transformations Souper proposes.

## Scope and budgets

| Level | Maximum combined-module size | Maximum instruction count | Default solver budget |
|---|---:|---:|---:|
| `-O3` | 256 KiB | 2,400 | 2 seconds |
| `-O4` | 4 MiB | 24,000 | 10 seconds |

`--superopt-budget=<sec>` overrides the per-invocation timeout. `-O3` is kept
small so solver cost is bounded for normal builds; `-O4` enables the broader
module scope. The solver runs on loop bodies as well as straight-line code, so
it can simplify expressions in hot kernels without instrumenting or timing the
program. The usual LLVM `-O3` vectoriser runs after the verified candidate is
accepted, allowing it to use any newly exposed simplifications.

## Optional toolchain integration

Souper's LLVM pass is optional and must match the selected LLVM toolchain.
Penstock does not silently download or build external tools. Install/build
Souper, Z3 and Alive2, then set paths as needed:

| Variable | Purpose | Default discovery |
|---|---|---|
| `PENSTOCK_OPT` | LLVM `opt` executable | `$LLVM_ROOT/bin/opt`, then `PATH` |
| `PENSTOCK_Z3` | Z3 executable | `PATH` |
| `PENSTOCK_ALIVE_TV` | Alive2 verifier | `PATH` |
| `PENSTOCK_SOUPER_PASS` | Souper LLVM pass plugin (`.so`) | common `lib/` locations |
| `PENSTOCK_TIMEOUT` | process timeout utility | `PATH` (`timeout`) |

A missing or incompatible tool is a safe no-op and emits a `superopt:` notice.
`--no-superopt` disables the stage. `--keep-ir` also keeps diagnostic artefacts
when Souper or Alive2 rejects a candidate.

## Why the general SAT-preimage idea is bounded

Instruction superoptimisation solves a well-defined objective: find a cheaper
sequence with the same semantics. Automatically replacing an arbitrary
algorithm by a precomputed input-to-output map is a different synthesis
problem: it needs a finite input domain or a user-specified output/property,
a cost model for the replacement algorithm, and a sound proof that the map
covers the relevant inputs. Penstock does not invent such a specification.
The current SAT/SMT stage safely delivers equivalent instruction-sequence
rewrites; whole-algorithm inverse synthesis and automatic lookup-table
precomputation remain future work rather than being approximated with unsafe
runtime sampling.

## Legacy runtime tuning

The earlier `-O4` genetic runtime tuner — execute-and-time search over
thread count, chunk size, affinity and codegen flags, opt-in via
`--evo-runtime-tune` — is documented in
[`docs/evo-guide.md`](evo-guide.md) (search space, algorithm, gates). It
is orthogonal to verified instruction superoptimisation, which is
attempted first.

---

## Cost-vector acceptance (optimisation-audit §7.1/§7.2)

A shorter, Alive2-verified instruction sequence is no longer accepted on
length alone — a rewrite that is provably equivalent can still be
SLOWER.  The acceptance metric is now a cost vector:

```
(instructions, weighted latency, memOps, branchOps)
```

with per-opcode latency weights (sdiv/udiv/fdiv 20, srem/urem 26, mul 5,
call 12, load/store 4, gep/icmp/fcmp/select/br 2, other 1) — a documented
static approximation of an out-of-order x86-64 core (Penstock drives an
external Clang, so LLVM's TargetTransformInfo is not linkable here;
TTI/scheduling-model integration is the noted upgrade path).

`--superopt-objective=` selects the decision rule:

- `latency` (default): weighted latency must strictly improve; code
  growth capped at +25% (register pressure / i-cache guard);
- `size`: instruction count must strictly improve and latency must not
  regress (no divide creep for shorter code);
- `balanced`: either of the above.

Acceptance is phrased as **minimum-cost extraction over the verified
candidate list {incumbent, Souper candidate}** — the C++ mirror of the
Lean kernel's `selectMinCost` (see `docs/proof-guide.md` and
`lean/PenstockProofs.lean`).  With `--proof-log` enabled, every accepted
rewrite emits a certificate line carrying both cost vectors and the
Alive2 verdict, and rejections print the vectors that lost.
