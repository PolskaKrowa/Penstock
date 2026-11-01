# Finite-domain algorithm precomputation

Penstock's finite-domain precomputation stage is a verified first step towards
algorithm rewriting based on precomputed input/output maps. It is enabled by
default at `-O3` and `-O4`; use `--no-precompute` to disable it.

## What it does

The stage scans the combined LLVM module, canonicalises scalar integer code,
and identifies pure functions whose complete input domain is small. It
executes a separate evaluator over every possible input tuple and serialises
the resulting outputs into a constant table. A candidate body then computes a
packed key from its arguments and loads the corresponding entry. Functions
that search for an input matching a requested value can benefit too: when the
search is expressed as an eligible finite-domain integer function, its map is
`requested value -> matching input`, removing the repeated search at runtime.

The map is exhaustive, not sampled. No table is installed unless Alive2 proves
the rewritten whole module equivalent/refining with respect to the original
LLVM IR. Timeouts, unsupported operations, incomplete evaluations, missing
tools, or failed proofs preserve the original function.

## Scope and costs

| Limit | `-O3` | `-O4` |
|---|---:|---:|
| Total input bits per function | 8 | 16 |
| Maximum entries per function | 256 | 65,536 |
| Aggregate evaluated input tuples | 1,024 | 65,536 |
| Candidate functions per build | 4 | 8 |
| Mapping data budget per function | 16 KiB | 512 KiB |

The pass ranks larger eligible bodies first and has stage-level and per-tool
timeouts. It canonicalises with `opt` when available, otherwise it uses
Clang's LLVM optimisation pipeline with a non-executed volatile probe to keep
integer functions live and promote stack locals. The evaluator itself is
compiled and run separately from the program being built.

Eligible functions must have integer arguments and an integer return value,
with a small complete input domain. After canonicalisation they may use integer
arithmetic, comparisons, selects, casts, PHIs, branches, and direct calls to
other functions that satisfy the same safety rules. Loads/stores, global data,
I/O, indirect/external calls, recursion, and potentially trapping integer
division are excluded. Loops can be considered if the pure SSA body is eligible;
non-terminating or expensive cases are bounded by the evaluator timeout.

## Important boundary

This is exhaustive finite-domain rewriting, not general SAT-guided synthesis of
an entirely new algorithm. It can turn an existing finite search function into
a lookup, but it does not yet infer arbitrary inverse functions from a separate
forward function, optimise floating-point/pointer/memory algorithms, or prove
unbounded algebraic loop transformations. A future SAT/SMT algorithm-synthesis
stage would need to derive a symbolic relation, constrain the desired output,
synthesise a replacement, and then discharge equivalence with Alive2.

Required tools are `alive-tv` from Alive2 and `timeout`. The standalone LLVM
`opt` binary is optional because Clang provides the conservative
canonicalisation fallback. Tool paths can be configured with
`PENSTOCK_ALIVE_TV`, `PENSTOCK_TIMEOUT`, and `PENSTOCK_OPT`.

---

## Profitability gate (optimisation-audit §7.4)

Verification and profitability are now separate gates.  A mapping that
Alive2 PROVES correct is installed only when it is expected to pay for
itself:

- the original function body must carry real work — at least **8
  instructions** (replacing three ops with a load is noise);
- the table must fit the cache level the scope targets — **4 KiB at
  -O3** (comfortably inside L1 next to the program's other hot data),
  **64 KiB at -O4** (L2 territory).

A rejection under this gate is reported as

```
precompute: 'fn' verified but NOT profitable (table N bytes vs M-instruction body; ...) — keeping the original function
```

— deliberately never conflated with "not verified".  Without profile
data the call frequency is unknown, so this is a conservative static
gate; PGO-weighted hotness is the natural refinement.
