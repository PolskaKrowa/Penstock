# Penstock PGO guide

Profile-guided optimization in three steps:

```
# 1. instrument
penstock --profile prog.fsh -o prog_prof

# 2. run with representative data  (writes prog_prof.fprof)
./prog_prof                     # or: FRT_PROF_OUT=my.prof ./prog_prof

# 3. recompile guided
penstock --use-profile=prog_prof.fprof prog.fsh -o prog
```

## What gets instrumented

- **Function entry counters** — one `i64` per function, bumped at entry.
- **Branch edges** — two counters per branch site (taken / not-taken),
  attached to `if` conditions and loop exits.

The counters live in `@__frt_prof_*` globals; `main` registers them with
`frt_prof_register` once; an `atexit` hook writes the `.fprof` text file:

```
FRESHET-PROF 1
F <fnhash> <count>
B <edgehash> <count>     (edgehash = site or site ^ 0x5A5A…)
```

Hashes are stable across compilations (`fnname:declline`), so editing the
program between steps 2 and 3 keeps counters matched (new sites simply
have no data).

## What the profile changes

With `--use-profile`:

- `!{!"function_entry_count", i64 N}` on definitions — LLVM's inliner
  and block placer see real hotness.
- `!{!"branch_weights", i32 taken, i32 notTaken}` on profiled branches —
  hot paths get straight-line layout, cold paths move out of line.
- Functions with > 100k calls get the `hot` attribute (aggressive
  inlining/unrolling).
- Unprofiled branches get neutral 50/50 weights.

Verify with `penstock -S --use-profile=... prog.fsh -o prog.ll` and grep:

```
define internal i64 @u_classify(double %vparam) hot #7 !prof !100
!100 = !{!"function_entry_count", i64 4000000}
!101 = !{!"branch_weights", i32 2000632, i32 1999368}
```

## PGO + performance report

`penstock --report --use-profile=p.fprof prog.fsh` runs the binary once
with `FRT_TIMING=1` and merges:

1. frontend diagnostic tallies + struct layout table
2. IR metrics (vectorizable loops, reductions, branch-weight coverage)
3. runtime parfor region timings (`<exe>.frtiming`, correlated by tag to
   fn:line)
4. PGO layer — hot functions ranked by measured call count

## Notes

- Instrumented binaries run slower; never ship them.
- Profiles are per-architecture; `--march` mismatch is fine (counters
  still apply) but performance conclusions should use matching builds.
- The profile file is plain text — diffable and auditable.

## Thread-safe counters + LLVM IR PGO (audit §11 integration)

**Custom counters are now atomics.** The per-site bump is a single
`atomicrmw add ... seq_cst` instead of the historical
load/add/store triple, which lost updates when a counter site sat
inside a `parfor` chunk fn executed by several workers.  Training runs
under parallel execution are reproducible again.  (The counters remain
a source-level approximation: they carry function/branch frequencies
only, with no value profiling.)

**LLVM IR-based PGO is wired into the driver** (the workflow the Clang
user manual recommends for performance-oriented PGO — lower overhead,
smaller raw profiles, better runtime performance than front-end
instrumentation):

```sh
# 1. training build:
penstock --llvm-pgo-generate app.fsh -o app_prof
# 2. run with a named profile output:
LLVM_PROFILE_FILE=app.profraw ./app_prof
# 3. reuse (profraw / a directory of them / a comma list; raw profiles
#    are merged automatically with llvm-profdata, which is looked up in
#    $LLVM_ROOT/bin, then $PATH — or pointed at explicitly):
penstock --llvm-pgo-use=app.profraw app.fsh -o app
```

`--llvm-profdata=<exe>` overrides the tool location.  If the tool is
unavailable, merge manually (`llvm-profdata merge -output=x.profdata
*.profraw`) and pass the `.profdata` directly.  Notes:

- the backend must bundle the LLVM profile runtime (all stock LLVM
  19-23 `clang` installs do; some clang-compatible wrappers do not);
- both paths can coexist: `--profile` (custom counters, feeds
  `--use-profile`) and `--llvm-pgo-*` (feeds the LLVM inliner /
  layout / vectoriser through the standard `-fprofile-use` pipeline);
- `-O4` evolution is skipped on instrumented builds, and its fitness
  now (a) takes the MEDIAN of repeated samples after a warm-up rather
  than the single fastest run and (b) rejects any candidate whose
  stdout differs from the reference run (checksum gate), so the search
  can no longer pick a miscompiling configuration.
