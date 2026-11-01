# Penstock `-O4` — verified superoptimisation and optional runtime tuning

`-O4` runs the deterministic `-O3` pipeline, then attempts a wider SAT/SMT-backed
LLVM instruction rewrite. Souper proposes a smaller equivalent expression and
Alive2 must verify the candidate before it can replace the IR. See
[`docs/superopt-guide.md`](superopt-guide.md) for tool setup, size limits, and
verification details.

The previous genetic runtime tuner is still available but is opt-in because it
executes and times a binary rather than proving instruction-sequence equivalence:

```sh
bin/penstock -O4 program.fsh -o app
bin/penstock -O4 --superopt-budget=20 program.fsh -o app
bin/penstock -O4 --evo-runtime-tune --evo-budget=30 --evo-seed=42 program.fsh -o app
```

## Legacy runtime tuning (opt-in)

### What it searches

| knob | search space | why it matters |
|---|---|---|
| clang opt level | `-O2`, `-O3` | `-O3`'s aggressive unrolling sometimes loses to `-O2` (I-cache pressure) |
| `-march` | `native`, `x86-64-v3`, `x86-64` | wide vectors can hurt small/irregular loops; skipped if you pinned `-march=` |
| worker pool size | 1 … 2× cores | oversubscription vs. parallel coverage |
| thread pinning | off, on | `FRT_AFFINITY` — helps or hurts depending on co-tenancy |
| default parfor chunk | auto, 64, 256, 1024, 4096, 16384 | chunk granularity trades scheduling overhead vs. load balance (`FRT_CHUNK`, new in R3) |

Search space on a 2-core machine: 4 threads × 6 chunks × 2 pin × 2 opt
× 3 march = **288 configurations** — too many to try exhaustively, fine
for a genetic algorithm to prune.

### How it works

1. The normal pipeline links the incumbent binary (`-O3`, defaults).
2. Candidate binaries are compiled lazily from the finished IR, one per
   opt-level × march pair (max 6 links), memoized.
3. A genome = (opt, march, threads, pin, chunk). The engine evaluates
   genomes by `fork`/`exec`-ing the candidate with
   `FRT_THREADS/FRT_AFFINITY/FRT_CHUNK` set, timing wall-clock with
   adaptive best-of-k repetitions (more reps for sub-100 ms programs).
4. Evolution: population 8, seeded with the incumbent plus heuristic
   defaults (serial, `-O2`, pinned, small chunk) and random individuals;
   tournament selection (k=3), uniform crossover, per-gene mutation
   (~35%), 2 elites per generation; stops at the budget, on convergence,
   or after 200 generations.
5. **Re-validation**: the champion faces the incumbent in alternating
   timed runs; it must win the median comparison by > 3% or the
   defaults are kept. The engine never ships a slower binary due to
   measurement noise.
6. **Baking**: a confirmed winner is compiled into the final binary —
   relinked with the winning clang flags and a runtime built with
   `-DFRT_TUNED_THREADS/…_AFFINITY/…_CHUNK` defaults, so nothing depends
   on environment variables. The env-var equivalent is printed either
   way, e.g.:

```
evo: winner 5.8 ms vs 7.1 ms (1.22x)  config: -O3 march=x86-64-v3 threads=4 pin=0 chunk=256
evo: tuning baked into app (reproduce with: FRT_THREADS=4 FRT_AFFINITY=0 FRT_CHUNK=256)
```

### Knobs

| flag | default | meaning |
|---|---|---|
| `--evo-budget=<sec>` | 10 | wall-clock budget for the whole search (compiles + runs) |
| `--evo-seed=<n>` | time-derived | RNG seed; fixed seed + fixed machine = reproducible search |

### When it skips itself (with a notice)

- `--profile` builds — instrumented binaries time meaningless.
- A single program run exceeds ~60% of the budget — raise
  `--evo-budget` or use `-O3`.
- The program exits nonzero or hangs under timing — fix the program
  first; the engine only tunes programs that run cleanly.
- `-S` / `-c` — no linkable binary to tune.

## Combination with the rest of the pipeline

`-O4` composes with everything: `--soa=auto` runs first (source-level
transform), `--use-profile` metadata is already baked into the IR the
engine relinks, and `--report` / `--run` execute the final, possibly
re-tuned binary. Typical release build:

```sh
bin/penstock --soa=auto --use-profile=app.fprof -O4 --evo-budget=30 \
    program.fsh -o app
```
