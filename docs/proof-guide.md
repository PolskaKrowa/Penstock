# Proof guide — Penstock's three verification layers

Penstock follows the optimisation-audit's §5.2 boundary: **prove
legality, then estimate profitability, then measure the result** — and
never present one layer's evidence as another's.

```
   Freshet source
        |
        v
  [ Layer 1 ]  Lean semantic kernel        lean/PenstockProofs.lean
               (typed BitVec semantics, rewrite legality contracts,
                bounded minimum-cost extraction)      -- not executed
        |                                              by the compiler
        v
  [ Layer 2 ]  Penstock AST transforms     src/*_analysis.cpp, loop_transforms.cpp
               (effect summaries, dependence proofs, typed const eval)
               + --proof-log premise records
        |
        v
  [ Layer 3 ]  LLVM translation validation  src/superopt.cpp, src/precompute.cpp
               (Alive2 must print "Transformation seems to be correct!")
```

## Layer 1 — the Lean kernel (`lean/PenstockProofs.lean`)

Dependency-light Lean 4 (core only, no Mathlib):

- **§5.3 semantics**: `BitVec w` fixed-width integers with the exact
  semantics the IR has; a typed expression language over one width with a
  deterministic evaluator.
- **§6.1 obligation**: `eval_fold : eval env (fold e) = eval env e` —
  the constant folder the C++ pass implements, as a theorem.
- **§5.4 identities**: `x+0`, `0+x`, `x*1`, `1*x`, `x*0`, `x-x`.
  The unsigned `x / 2^k = x >>> k` identity is stated as an OPEN
  obligation (`sorry`) — deliberately: it is better to carry a visible
  obligation than to claim an unchecked proof.  The signed case is
  documented as the biased form Penstock's IRgen actually emits
  (`(x + bias) >>> k`), because plain `ashr` is NOT equivalent for
  negative operands.
- **§5.5/§7.2 certified candidates**: `CertifiedCandidate` carries a
  semantic-validity proof; `selectMinCost` extracts the minimum-cost
  member of a finite candidate list, with membership and minimum-cost
  theorems.  The C++ superoptimiser's acceptance is the mirror of this
  selection over `{incumbent, Souper candidate}`.
- **§5.2 premise contracts**: `FusionPremises` / `UnswitchPremises`
  name exactly what the C++ passes must establish.

### Status and CI

The authoring environment of the integration had **no Lean toolchain**,
so the theorems are not kernel-checked there — the same honest limitation
the optimisation audit itself reported.  `scripts/check-lean.sh`
elaborates the file and **fails on any `sorry`**, so CI cannot let an
open obligation masquerade as a verified result:

```sh
scripts/check-lean.sh        # needs `lean` on PATH (or LEAN=/path/to/lean)
```

## Layer 2 — AST transforms and the proof log

Every source-level transform runs only after proving its premises from
the effect summaries, dependence analysis and typed constant evaluator.
`--proof-log[=<file>]` (default `<output>.prooflog`) records one JSON
line per APPLIED transform:

```json
{"pass":"partial-unroll","fn":"main","file":"pos_prooflog.fsh","line":5,
 "premises":"constant-trip:20;domain-partition:main[16]+remainder[4];no-return,no-break;factor=8"}
```

Premise vocabulary (mirrors the Lean contract structures):

| Pass | Premises recorded |
|---|---|
| `licm` | P1 immutable+unique-name, P2 no-inner-bindings, P3 referentially-transparent, P4 no-loop-write-of-reads, P5 trip>=1-proven or total-non-trapping |
| `partial-unroll` | constant-trip, exact domain partition main+remainder, no-return/no-break, factor |
| `peel` | constant-trip, guard value at k=0 and k>=1, ivar injectivity |
| `interchange` | R1 perfect-rectangular, R2 bounds-RT+total, R3 iterations-independent, R4 no-observable-effects, R5 bounds-read-nothing-body-writes |
| `tile` | R1–R5 + bounds-in-[0,2^62] proven + exact imin-clamped boundaries + tile sizes |
| `distribute` | D1 no-cross-group-dependence, D2 no-unknown-effects, D3 no-observable-effects, D4 bounds-RT+total |
| `unroll-and-jam` | R1–R5 + J1 writes-affine-single-ivar + J2 constant-trip partition |
| `superopt` | alive2 positive verdict, objective, both cost vectors, min-over-verified-candidate-list |

The log is an AUDITABILITY artefact: it makes pass decisions diffable in
CI.  It is not itself a proof — Lean and Alive2 are the gates.

## Layer 3 — Alive2 translation validation

Unchanged in shape from the audit's description, tightened in
acceptance:

- `src/precompute.cpp` installs a finite-domain lookup table only after
  Alive2 proves the whole-module rewrite, and now additionally reports
  **"verified but NOT profitable"** separately from "not verified"
  (§7.4).
- `src/superopt.cpp` accepts a Souper candidate only when Alive2 proves
  it AND it wins on the selected cost vector
  (`--superopt-objective=latency|size|balanced`, §7.1).  Instruction
  count alone can no longer accept a slower rewrite.

Solver failure, timeout, or a missing tool always means **keep the
incumbent** (fail-closed).

## What this design deliberately does NOT claim

- No "globally optimal machine code" claim: every optimality statement
  is *minimum cost within an explicit, finite candidate list* (§5.1).
- No Lean proof is treated as proof that the C++ correctly applied the
  rewrite: the `--proof-log` premises plus Alive2 on the real LLVM
  modules are the executable checks.
- No speculative LLVM attributes (`noalias`, `nsw`, `nuw`, `nonnull`)
  are emitted from heuristics (§12's exclusion list).
