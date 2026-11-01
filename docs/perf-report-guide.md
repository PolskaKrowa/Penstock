# Penstock performance report guide

Enable with `--report`:

```
penstock --report prog.fsh -o app
```

The compiler builds `app`, executes it once with `FRT_TIMING=1` (the
binary writes `app.frtiming` — per-parfor-region cycle counts), then
prints a merged report to stderr.

## Layers

**[1] Frontend** — diagnostic tallies by code, plus the struct layout
table: name, size, align, padding, and every field's `name:offset@size`,
so layout decisions are auditable without dumping IR.

**[2] IR** — text-scanned module metrics:

- user function and parfor chunk-function counts
- loops carrying the `llvm.loop.vectorize.enable` annotation ([static]
  estimate — present only where `@vectorize` / `--force-vectorize`
  requested them; see language-spec §14.3)
- fast-math floating ops overall and inside parfor bodies (reduction
  rewrite candidates)
- `noalias` parameter count (alias analysis wins)
- SoA plane operation count
- with `--use-profile`: branch-weight coverage

**[3] Runtime timing** — from `<exe>.frtiming`:

```
FRESHET-TIMING 1
PF <tag> <cycles> <calls> <chunks> <pinfails>
```

Tags are `hash(fn:line)` of the parfor site; the report resolves them to
`fn:line`. `cycles` is the wall-clock TSC span of the region (caller
thread, includes worker execution). `pinfails` counts threads that could
not be pinned (cgroup restrictions). Run your own workloads with
`FRT_TIMING=1 ./app` to collect without recompiling.

**[4] PGO** — with `--profile`: counter inventory; with
`--use-profile`: hot functions ranked by measured calls and coverage.

## Reading it

- High `E-*` counts are hard blockers — the binary would have been slow;
  they are the reason a program *didn't* compile.
- `parfor chunk fns` ≈ number of parallel regions; each region pays a
  dispatch (~µs) — chunk short loops or fuse them.
- `loops w/ vectorise annotation [static]` vs total loops: annotations are
  requests, not proofs — what the backend actually did is
  `loops vectorised [backend]` vs `vectorisation missed [backend]`
  (the record carries the miss reason).
- Region timing with `chunks` >> threads means `affinity:spread` —
  check `pinfails` before trusting spread numbers.

## Environment

| var | effect |
|---|---|
| `FRT_THREADS` | worker pool size (default: all cores) |
| `FRT_AFFINITY=1` | pin workers to distinct cores (best effort) |
| `FRT_TIMING=1` | write `<exe>.frtiming` at exit |
| `FRT_PROF_OUT` | PGO profile path override |

## Metric labels (audit §13.1 integration)

Every performance-report metric is now labelled with its provenance:

- **[static]** — a front-end estimate from the generated IR, e.g.
  `loops w/ vectorise annotation` (formerly misnamed "vectorizable
  loops"; counting annotations proves nothing about the backend).
- **[backend]** — a transformation the backend actually performed,
  parsed from the `--opt-remarks` optimisation-record YAML: `loops
  vectorised [backend]`, `vectorisation missed [backend]` (with
  reasons), `fns inlined [backend]` (+misses).
- **[runtime]** — measured while the binary ran with `FRT_TIMING=1`:
  per-region cycles/chunks, and the `ST` scheduler counters
  (`parfor regions [runtime]: N inline (serial cutoff), M dispatched
  to pool`) for tuning `FRT_SERIAL_CUTOFF`.

The mode line additionally records the runtime IR optimisation level
(`runtime IR -O2`), any `--target`, `--veclib` and forced-vectorize
settings, and both PGO paths (custom counters and LLVM IR PGO).

Inspecting the backend artefact:

```sh
# the module the backend optimises (input):
penstock --emit-combined-ir kernel.fsh -o kernel
# the OPTIMISED module the backend produced (output):
penstock --emit-final-ir kernel.fsh -o kernel        # -> kernel.final.ll
# backend remarks + record file:
penstock --opt-remarks=vectorize,inline kernel.fsh -o kernel
#   -> kernel.opt-records.yaml (parsed by layer [2b] of --report)
```
