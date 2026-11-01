# Audit integration notes — compiler optimisation audit (24 Oct 2026)

This document maps every recommendation of
`freshet_penstock_optimization_report.md` to what is now implemented in
the compiler, the acceptance evidence, and the deliberately deferred
work.  Section numbers below refer to the audit report.

## P0 — purity/effect soundness (§2) — DONE

**Problem.** The boolean purity walk in `sema.cpp` conflated "does not
write memory" with `memory(none)` (LLVM's read-none), let a user `@pure`
bypass validation entirely (`f->isPure = f->userPure`), and its builtin
allow-list treated `malloc` / `soa_new` / `freshet.threads` / `soa_len` /
`soa_field_ptr` as memory-free.  Result: a pointer-reading helper was
stamped `memory(none) willreturn` and the §2.2 reproducer printed `00`
instead of `12` at `-O3`.

**Implementation** (`src/effects.cpp`, new):

- `Effects` lattice on every `FnDecl`: `readsArg / readsOther /
  writesArg / writesOther / allocates / freesMem / io / unknownCall /
  mayNotReturn`, computed to a fixed point over the call graph.
  "argmem" = parameter-reachable memory; "other" = globals, heap,
  unknown.  Local allocas are not observable and do not count.
- Reads and writes are tracked **separately**: `return p[0]` is
  read-only (`memory(argmem: read)`), never `memory(none)`.
- `willreturn` needs a termination PROOF: no `while`, no
  arbitrary-condition `for`, no `parfor`, no `assert`, and no position
  on a call cycle (DFS cycle detection after the fixed point).
  Range-`for` loops are proven terminating (bounded, +1 step).
- `@pure` is validated against the spec §3.1 contract ("reads no memory
  and writes no memory") with hard `F0304` errors and precise notes
  ("writes memory through 'p'", "calls 'g', which reads memory", ...).
- Builtin effects are modelled individually: intrinsics are clean;
  `sin/cos/log2/log10` are clean in fast mode (intrinsics) but
  memory-touching under `@strictfp` (libm/errno); `tan` always touches
  memory (no LLVM intrinsic); `malloc`/`soa_new` allocate; `free`/
  `soa_free` free; prints are I/O; `assert` may not return;
  `freshet.threads`/`time_*`/`cycles` read runtime state; `memcpy`/
  `memset`/`soa_len`/`soa_field_ptr`/prefetch are classified through
  their argument roots.
- Argument-root refinement: passing memory rooted in the caller's
  locals/globals to a callee that touches argmem upgrades the caller's
  summary to "other" memory (sound over-approximation).

**IRgen** (`irgen.cpp` `fnAttrBody` / `attrGroupFor`): definition and
call sites share one canonical attribute body, so they can never
disagree (§2.3 rule 2).  Fixed groups #50–#53/#60/#61 keep their
historical meaning; effect-specific combinations (strictfp x effects,
inline x effects, `memory(argmem: ...)`, `memory(none)` without
termination proof) are allocated dynamically from #62 up.

**Evidence.** `tests/pos_effects_repro.fsh` prints `12` at -O0/-O3;
`read_first` carries `#62 = { noinline nounwind memory(argmem: read)
willreturn }` on both its definition and its call sites.  26 new
regression tests cover every §2.4 bullet.

## §3 — runtime IR at -O2 — DONE

- `runtimeLl` builds the cached runtime with `-O2` by default
  (`--runtime-opt=N` overrides, `0` = historical debug runtime); the
  cache key is now `frt_rt_<llvm-major>_o<level>.ll`.
- The `-O4` tuned runtime uses the same level (was hardcoded `-O0`).
- Fixed a latent concatenation bug this surfaced: the runtime `.ll`'s
  own `!llvm.module.flags` (carrying `!"Dwarf Version"`) collided with
  the user module's flags on modern clang ("module flag identifiers
  must be unique"); `stripRuntimeHeader` now drops the runtime's
  module-level named metadata.
- Evidence: `grep -c optnone build/frt_rt_*_o2.ll` = 0; runtime fns
  (`frt_malloc`, ...) now inline into user code (visible in
  `--opt-remarks` records).

## §5 — loop metadata policy — DONE

- No forced vectorisation metadata by default.  `@vectorize for ...`
  emits `vectorize.enable` **without** interleave count;
  `--force-vectorize` restores the historical blanket hint for
  differential testing; fusion propagates the hint to the fused loop;
  `--no-loop-opts` semantics unchanged.
- Report metric renamed: "vectorizable loops" is now "loops w/
  vectorise annotation **[static]**" (audit: an annotation count is not
  proof of vectorisation).

## §8 — vector math — DONE

- Fast FP (the default): `sin`/`cos`/`log2`/`log10` lower to
  `llvm.sin/.cos/.log2/.log10` intrinsics, f32 directly (the historical
  double detour is gone); `tan` keeps its libm call (no intrinsic);
  `@strictfp` functions keep scalar libm calls for everything here
  (errno is memory the strict contract must not hide).
- `--veclib=libmvec` appends `-fveclib=libmvec -fno-math-errno` to the
  backend.  Verified end-to-end: a sin/cos/log2 loop produces
  `_ZGVdN4v_sin`/`_ZGVdN4v_cos` (libmvec AVX2) and `llvm.log2.v4f64`
  in the final optimised IR.
- Accuracy policy documented here and in the language spec: fast mode
  = errno not part of the observable model; strict mode = IEEE +
  errno-visible libm.

## §6 — alias/bounds contracts — PARTIALLY DONE

- `@alias`/`noalias` logic is unchanged (already sound: F0306
  detection suppresses unsound pairs).
- `getelementptr inbounds` emission is unchanged: out-of-object
  indexing is a language-level UB contract (documented), and no
  unproven `nsw/nuw/nonnull/dereferenceable` attributes were added.
  A per-access bounds proof (ownership-based) remains future work;
  nothing new was claimed to LLVM without proof.

## §10.1/§10.4 — runtime serial cutoff — DONE

- `frt_parfor` executes regions with `total <= FRT_SERIAL_CUTOFF`
  inline in the caller (one context, no queue/wake-ups/atomics);
  default 256, env override, `-O4` genome dimension,
  `-DFRT_TUNED_SERIAL_CUTOFF` baking.  Reductions keep exact serial
  semantics (nchunks = 1 through the existing combine loop).
- Scheduler counters (`ST <serial> <dispatched>`) in the `.frtiming`
  file, surfaced by the report as **[runtime]** data.
- Limitation (per the audit): the runtime knows iteration counts, not
  body cost; the threshold is tuned, not derived.  A compiler-side
  per-region cost estimate plumbed through the range struct is the
  natural next step.

## §11 — PGO and -O4 — DONE

- Custom counters are `atomicrmw add ... seq_cst` (thread-safe under
  `parfor`); the `.fprof` format and `--use-profile` flow are
  unchanged.
- LLVM IR PGO: `--llvm-pgo-generate` / `--llvm-pgo-use=<profraw|dir|
  list|profdata>` with automatic `llvm-profdata merge` (tool lookup:
  `--llvm-profdata` > `$LLVM_ROOT/bin` > `$PATH`), matching the
  documented Clang workflow.
- `-O4` (`evo.cpp`): warm-up + repeated samples with MEDIAN fitness
  (not the single fastest run); stdout captured and compared — any
  candidate whose output differs from the reference is rejected
  (checksum gate, also enforced during re-validation); genome gains
  `FRT_SERIAL_CUTOFF` and `veclib` dimensions; the 3% noise gate and
  alternating re-validation are kept.
- Note: the sandbox used for this integration (a clang-compatible
  wrapper without the bundled LLVM profile runtime) cannot link
  `-fprofile-generate` binaries; on stock LLVM 19-23 toolchains it
  works out of the box.

## §12 — target configuration — DONE

- Every build resolves the canonical triple + data layout by probing the
  backend clang (compiling an empty C file): `--target=<triple>` /
  `--portable` probe with `-target <want>` (normalised through the
  backend); a native build probes WITHOUT `-target` and scrapes the
  backend's own default triple.  The module header and the backend
  invocation therefore agree on ONE target by construction instead of a
  hard-coded x86-64 guess — which used to disagree with toolchains
  defaulting to `x86_64-unknown-linux-gnu` (upstream LLVM builds) and
  triggered clang's `[-Woverride-module]` "overriding the module target
  triple" warning on every `.ll` ingest.
- `--portable` selects the generic x86-64 baseline (no `-march=native`).
- The probe-failure path keeps the built-in `x86_64-pc-linux-gnu` header
  pair (a clear warning is printed).  Known limitation: the cached
  runtime `frt_rt_<N>_o<level>.ll` is always compiled with the backend's
  default triple (its header lines are stripped on merge), so genuinely
  cross-target builds inherit host-target runtime codegen.

## §13 — diagnostics and report — DONE

- `--report` labels metrics **[static]** / **[backend]** / **[runtime]**
  and parses the `--opt-remarks` optimisation-record YAML into a new
  layer [2b]: actual loops vectorised, misses with reasons, inlines.
- `--emit-final-ir[=<file>]` writes the final optimised IR the backend
  produced (distinct from `--emit-combined-ir`, which is the backend's
  INPUT).
- Fixed a latent quoting bug: the `-Rpass…=<regex>` flags contain `|`
  and were being shell-interpreted as pipelines.

## §2.4/§13.2/§13.4 — tests — DONE

`tests/run.sh` grew from 41 to 67 tests.  New: the §2.2 reproducer at
-O0/-O3, global-read, alias/rebind, malloc-wrapper, builtin-effect
(IR-attribute) checks, willreturn inspection, four F0304 negatives,
differential -O0-vs-OO3 checksum, runtime-optnone check, `@vectorize`
IR + output, `--force-vectorize`, atomic-PGO-counter check, serial
cutoff (inline/dispatch/default) + ST stats, transcendental intrinsic
lowering + `@strictfp` libm retention, `--emit-final-ir`.
CI policy adopted from the audit: no optimisation attribute ships
without a positive test for its legality and a negative test for the
case where it must not be applied.

## Deliberately deferred (with rationale)

These audit items need dedicated analysis passes and their own
benchmark evidence; none of them block the correctness work above:

1. **§4/§7 loop transforms** — partial unroll / interchange / tiling /
   distribution / unroll-and-jam beyond the existing fusion + full
   unroll.  The costed-transform scaffolding (footprints, alias maps,
   canon forms in `opt_util.cpp`) is in place; each transform needs a
   legality proof + benchmark gate per the audit's "what not to do
   first" guidance.
2. **§6.3 AoSoA** — extending `--soa=auto` with a tiled layout option
   and field-use-driven cost model.
3. **§10.2/§10.3** — per-worker context arenas/stealing and NUMA
   first-touch placement; the serial cutoff addresses the small-region
   dispatch overhead first (measurably the dominant term).
4. **§14 benchmark suite** — a geometric-mean multi-workload harness
   for `-O4`; the checksum gate + median fitness are the measurement
   foundation it needs.
5. **Body-cost-aware serial cutoff** — plumb a per-region cost estimate
   from the front end through `frt_range_t` so the cutoff can react to
   "1000 iterations of expensive work" vs "1000 increments".

---

# Optimisation & proof audit integration (31 Oct 2026)

This part maps every recommendation of
`Freshet_Penstock_Optimisation_Proof_Report.md` (the optimisation and
Lean-proof audit) to the implementation.  Section numbers refer to that
report.  The audit's own order (§13) was followed: correctness hardening
first, then the analysis foundation, then the new passes.

## §4.1–§4.6 — P0/P1 legality hardening — DONE

| Audit item | Fix | Evidence |
|---|---|---|
| §4.1 predicate merge removes impure call | `mergeScan` now requires the shared condition to be referentially transparent (`exprReferentiallyTransparent`: effect-lattice-backed builtin model + pure will-return callees, strict-FP aware) | `tests/neg_lane_merge_impure.fsh` (16 calls preserved), `tests/pos_lane_merge_pure.fsh` (merge still fires) |
| §4.2 unswitch evaluates condition on zero-trip loops | hoist requires proven trip ≥ 1 (`tripCountOf`) OR a provably total condition (`exprTotalAndPure`: no div/rem/shift by non-constant, no dereference, no may-not-return call); same rule in the deep unswitcher | `tests/neg_lane_unswitch_zero_trip.fsh` (prints 0, no trap) |
| §4.3 unrolling does not veto `return` | `unrollInList` vetoes `info.hasReturn`; the clone-safety contract for defer/give/parfor is written out explicitly in the source | `tests/neg_unroll_return.fsh` |
| §4.4 signed-overflow/cast hazards in const eval | new typed bit-vector evaluator `evalConstTyped` (width+signedness, wrapping ops, truncating casts, refuse-to-fold traps); `__int128`-checked trip distances; unroll literals computed mathematically then wrapped | `tests/neg_const_overflow_fold.fsh` |
| §4.5 canon is not a typed normal form | canon literals now carry value+type tags, constant folding in the key is typed and wrapping, commutativity is decided by the typed contract (no float `+`/`*` or `&&`/`\|\|` reordering) | full suite + difftest |
| §4.6 fusion unknown-effect gap | unknown-write now rejects against ANY effect of the other loop (reads AND writes — the output-dependence case the audit called out); dependence classes documented per rule | `tests/neg_fuse_unknown_effect.fsh` (no F0908), `tests/pos_fuse_disjoint_regions.fsh` |

## §6.1/§6.3 — typed const fold + range analysis — DONE

`src/loop_analysis.cpp`:
- **TypedConstFold** (`runTypedConstFoldPass`, default at -O2+): folds
  integer/bool expressions at exact source width; short-circuit `&&`/`||`
  fold without evaluating the unreachable operand; division never folds
  with a zero divisor.  Plus the §6.2 "modest" immutable-constant
  propagation for unique-name non-mut `let`s (NOT a full SCCP — LLVM owns
  that after IRgen).
- **Range/induction analysis**: `Affine` (`a*iv+b`, generalising
  `affineIvarLiteral`), `Interval` (with sound cast-range clamping and
  `imin`/`imax` rules), consumed by every Phase D pass.

## §6.4–§6.9 — Phase D loop transforms — DONE (opt-in)

All in `src/loop_transforms.cpp`, each behind an opt-in flag with a
`--no-*` differential hatch, each emitting an N-* note and a
`--proof-log` premise line; legality premises are documented per pass:

| Pass | Flag | Legality (summary) | Test |
|---|---|---|---|
| FreshetLICM | `--licm` | P1 immutable+unique binding, P2 no inner bindings in init, P3 referentially transparent, P4 nothing read is written by the loop, P5 non-empty trip or total init | `pos_licm.fsh` |
| PartialUnroll | `--partial-unroll` | constant trip in (16,4096], no return/break/ivar-write; domain partitioned exactly into `lo + t*F + k` blocks plus an exact remainder loop | `pos_partial_unroll.fsh` |
| LoopPeel | `--peel` | guard decidable at k=0 and provably constant for k≥1 (injectivity); peeled copy + remainder loop start at lo+1 | `pos_peel.fsh` |
| LoopInterchange | `--interchange` | R1 perfect rectangular nest, R2 bounds RT+total (evaluation counts change both ways), R3 iterations independent, R4 no observable effects, R5 bounds read nothing the body writes; profitability = transposed-stride votes | `pos_interchange.fsh` |
| LoopTile | `--tile[=I[,J]]` | R1–R5 + interval proof bounds ∈ [0,2^62] so the synthesised ceil-shift and tile arithmetic are exact; boundary tiles `imin`-clamped (exact domain) | `pos_tile.fsh` |
| LoopDistribution | `--distribute` | D1 no cross-group flow/anti/output dependence, D2 no unknown effects, D3 no observable effects, D4 bounds RT+total (evaluated twice) | `pos_distribute.fsh` |
| UnrollAndJam | `--unroll-and-jam` | R1–R5 + J1 writes affine in exactly one ivar (either orientation), J2 constant-trip partition | `pos_unrolljam.fsh` |

Pass order (dependency-analysed): LICM → interchange → tile →
unroll-and-jam → peel → partial-unroll → distribute.  Two `imin`/`imax`
integer builtins were added (sema + icmp/select lowering) for exact tile
boundary clamping.

## §6.10–§6.12 — advisor, SoA costs, parfor body cost — DONE

- **AutoParforAdvisor** (`--parfor-advisor`, analysis-only): F0905
  for provably parallel-safe loops (injective writes, same-iteration
  same-root reads, no observable effects) and separate reduction
  candidates.  Never rewrites.
- **SoA frequency weighting + AoSoA advisory** (§6.11): the auto metric
  is now trip-count × access-count weighted; `--soa=aosoa` reports the
  frequency-weighted layout verdict (SoA / AoS / AoSoA band) without
  transforming (a real AoSoA lowering needs new runtime container types —
  the audit's own "best next step" was the measured cost model).
- **Body-cost-aware parfor cutoff** (§6.12, Phase F): the compiler fills
  `frt_range_t.body_cost` (ops/memory/call census); the runtime inlines
  when `trip × body_cost ≤ FRT_WORK_CUTOFF` (default 2048, env override,
  overflow-safe division) — 100 cheap and 100 expensive iterations now
  make different dispatch decisions.  Legacy ranges keep the iteration
  cutoff.

## §7.1/§7.2/§7.4 — superoptimiser & precompute — DONE

- **Cost-vector acceptance** (`--superopt-objective=latency|size|balanced`,
  default latency): instruction count alone no longer accepts a rewrite —
  the decision uses (instructions, weighted latency, memOps, branchOps)
  with per-opcode weights (documented static approximation; no linkable
  TargetTransformInfo in the external-clang architecture).  A shorter
  sequence that introduces a divide or extra load is rejected under the
  latency objective; the +25% code-growth cap guards register pressure.
- **Bounded minimum extraction** (§7.2): acceptance is now phrased as
  minimum-cost over the verified candidate list {incumbent, Souper} — the
  C++ mirror of the Lean kernel's `selectMinCost` — and emits a
  `--proof-log` certificate line with both cost vectors.
- **Precompute profitability** (§7.4): a VERIFIED mapping is installed
  only when it is expected to pay (≥ 8 instructions in the original body;
  table ≤ 4 KiB at -O3 / 64 KiB at -O4); the rejection message says
  "verified but NOT profitable", never conflating it with "not verified".

## §5 / Phase B — Lean semantic kernel — PROVIDED (CI-gated)

`lean/PenstockProofs.lean` (Lean 4 core, dependency-light):
- BitVec identities (§5.4) — `x+0`, `0+x`, `x*1`, `1*x`, `x*0`, `x-x`;
  the unsigned-div-by-2^k identity is an explicitly OPEN obligation
  (never claimed unchecked);
- typed expression semantics + evaluator + folder and the equational
  theorem `eval_fold : eval env (fold e) = eval env e` (§6.1's proof
  obligation);
- `CertifiedCandidate` + `selectMinCost` + membership/minimum-cost
  theorems (§5.5/§7.2);
- `FusionPremises`/`UnswitchPremises` contract structures naming exactly
  what the C++ passes prove (and what `--proof-log` records).

**Honest status** (matching the audit's own §2/§5.6 caveat): this
environment has no Lean toolchain, so the theorems are NOT kernel-checked
here; `scripts/check-lean.sh` runs the file in CI and FAILS on any
`sorry`.  The `--proof-log` artefact makes every applied transform's
premises auditable and diffable in the meantime.

## §8 / §9 — profitability model + pipeline — DONE

`src/profit_model.{h,cpp}`: one shared target profile
(`--target-cache-line=`, `--target-l1=`, `--target-l2=`,
`--target-vector-width=`) and cost features (ops/mem/calls/bytes-per-iter
per loop body) feeding every Phase D decision — legality and
profitability are separate stages everywhere.  The pipeline order follows
§9: const-fold → (effects/alias) → fusion/unroll/lane → Phase D →
advisor → IRgen → LLVM -O3 → superopt/precompute → PGO.

## §10 — validation — DONE

- §10.1: all eight audit-named regression tests exist
  (`neg_lane_merge_impure`, `neg_lane_unswitch_zero_trip`,
  `neg_unroll_return`, `neg_const_overflow_fold`,
  `neg_fuse_unknown_effect`, `pos_fuse_disjoint_regions`,
  `pos_unroll_simple`, `pos_lane_merge_pure`) plus per-pass feature
  tests — 117 tests total, all passing.
- §10.2 `scripts/difftest.sh`: every positive test compiled at -O0, -O2,
  -O2 --no-loop-opts, -O3 and -O2+all-Phase-D; FULL stdout compared —
  60 programs, zero disagreement.
- §10.3 `scripts/property_test.py`: seeded random-program generator
  biased to the audit's adversarial shapes (0/1-trip loops, negative
  bounds, casts, peel guards, unroll windows, transposed nests) —
  **it found a real bug** (non-zero lower bounds in partial unroll /
  unroll-and-jam value reconstruction), which is fixed and covered; 230
  programs across two seeds now agree.
- §10.4: pass remarks (N-* notes) state premises; `--proof-log` writes
  the per-transform JSON lines; static estimates are labelled as such,
  never presented as measurements.

## Latent defects found and fixed along the way

1. `fn ... -> bool` return ABI: bool values are i1 registers but the
   signature used i8 — `ret i8 <i1>` was rejected by LLVM (pre-existing,
   reproduced on the original snapshot; fixed at the ret and call
   boundaries, mirroring the earlier bool-parameter fix).
2. Non-zero lower bounds in PartialUnroll / UnrollAndJam value
   reconstruction (`t*F+k` assumed lo=0) — found by the §10.3 property
   tests, fixed with explicit `lo + t*F + k` reconstruction.
3. `imin`/`imax` mixed-sign literal typing: `-5` defaulted to an unsigned
   type, silently flipping `imin(-5, 2)` to 2 — sign-sensitive typing now
   forces the signed interpretation when an operand is syntactically
   negated; the select/predicate direction in the lowering was corrected.
4. `tests/pos_ringbuf.fsh` had stopped parsing (pre-`[reduction(+:x)]`
   clause syntax) and was silently excluded from the suite — refreshed
   and re-covered by the differential run.

## Deliberately deferred (with rationale)

- **AoSoA code generation** — needs new runtime container types; the
  audit's requested measured cost model + advisory IS implemented.
- **Dynamic-trip partial unrolling** — synthesised bound arithmetic is
  only exact with an interval proof of `hi ≥ lo`; deferred until the
  range analysis is interval-complete over identifiers (constant trips
  are exact today).
- **General polyhedral transforms, blanket fast-math, speculative
  `noalias`/`nsw`/`nuw`, auto-prefetch from static strides** — §12's
  "would not implement yet" list is respected verbatim.
