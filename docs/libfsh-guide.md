# libfsh — Freshet HPC standard library

This document describes the public `libfsh` API surface introduced in
Stage A of the report integration.  See `include/fsh/` for the header
contracts; `src/libfsh.c` for the reference implementation that wraps
the existing `frt_*` runtime ABI.

## Scope

`libfsh` is a **documented public library with a stable ABI** (once
released; for now considered experimental), not merely a larger collection
of compiler intrinsics.  It layers over `runtime/frt_rt.c` — the
`frt_*` symbols the IR generator already emits — and exposes a typed,
ownership-aware, error-checked surface.

```
include/fsh/             # public API, language declarations
libfsh_core              # memory, spans, errors, portable utilities
libfsh_math              # accurate and optional fast/vector math
libfsh_simd              # portable SIMD abstractions and target kernels
libfsh_parallel          # pools, reductions, scheduling, atomics
libfsh_io                # buffered and binary scientific I/O
libfsh_mpi               # optional MPI bindings/interop (planned)
libfsh_gpu               # future accelerator runtime/API (planned)
runtime/frt_*            # internal ABI required by generated programs
```

For now the whole library is one compilation unit (`libfsh.c` +
`frt_rt.c`); the split above is the *intended* public-API boundary,
not a build-time requirement.

## Quick start

```c
#include <fsh/fsh.h>

int main(void) {
  /* Aligned allocation with overflow checking. */
  double* v = fsh_alloc(1024 * sizeof(double), 64);
  if (!v) { /* fsh_errno set; or abort() if FSH_FAIL_ABORT */ }

  /* Typed span view. */
  fsh_span s = fsh_span_make(v, 1024);

  /* Parallel for with explicit schedule. */
  fsh_range r = {.lo = 0, .hi = 1024, .sched = FSH_SCHED_STATIC};
  fsh_parallel_for(&r, my_body, &s);

  /* Reduction with deterministic (bit-reproducible) mode. */
  double sum = fsh_reduce(0, 1024, FSH_RED_SUM, FSH_RED_DETERMINISTIC,
                           my_value_at, &s);

  fsh_free(v);
  return 0;
}
```

## Error model

Every function that can fail returns a sentinel (`NULL`, `0`, or
negative `fsh_err`) and sets the per-thread error code retrieved via
`fsh_last_errno()` (planned).  Callers can opt into abort-on-failure
via `fsh_set_alloc_fail_mode(FSH_FAIL_ABORT)` — useful for HPC kernels
where recovery is impossible.

| error code           | meaning                                            |
|----------------------|----------------------------------------------------|
| `FSH_OK`             | success                                            |
| `FSH_ERR_NOMEM`      | allocation failed                                  |
| `FSH_ERR_OVERFLOW`   | size computation overflowed (e.g. count × size)    |
| `FSH_ERR_INVALID`   | invalid argument (null, negative, etc.)            |
| `FSH_ERR_RANGE`      | span/range bounds violated                         |
| `FSH_ERR_UNSUPPORTED`| feature not available on this target               |

## Memory and alignment model (report §5.2)

The library guarantees:

- `fsh_alloc(bytes, align)` returns an address that is a multiple of
  `align` (or `NULL`).
- The minimum alignment that callers may rely on is **16 bytes**.
- SoA plane allocations are **64-byte aligned** (cache-line aligned) so
  vector loads never straddle a line.

The "declared struct alignment" (from `align(N)` in Freshet source) and
the "allocation alignment" (above) are distinct: a struct declared
`align(64)` may live inside an `fsh_alloc(bytes, 16)` allocation and
therefore NOT be 64-aligned at runtime.  Code that requires a specific
alignment must request it from the allocator.

## Parallel primitives (report §5.2 + §6.1)

```c
typedef struct {
  int64_t lo, hi;          /* half-open iteration range [lo, hi) */
  int32_t threads;         /* 0 = use default worker count */
  int32_t chunk;           /* 0 = heuristic */
  fsh_sched sched;         /* STATIC | DYNAMIC | GUIDED | SPREAD */
  int32_t affinity;        /* 0 = no pinning; 1 = pin (best effort) */
} fsh_range;

int32_t fsh_parallel_for(const fsh_range* r, fsh_body_fn body, void* ctx);
```

**Memory model (report §6.1):**

- Within one `fsh_parallel_for` region: writes by iteration `i` are NOT
  visible to iteration `j` (`i != j`) — even if they target the same
  address.  Cross-iteration communication requires a reduction clause
  or an atomic operation.
- After the region completes: all writes by all iterations are visible
  to the calling thread (release-acquire pairing on the region's
  `remaining` counter).
- Nested regions: outer iteration's writes are visible to inner
  iterations of the same outer chunk; sibling outer iterations are
  isolated as above.

## Reductions (report §2.5 + §5.2)

```c
typedef enum { FSH_RED_SUM, FSH_RED_PRODUCT, FSH_RED_MIN, FSH_RED_MAX } fsh_red_op;
typedef enum { FSH_RED_DETERMINISTIC, FSH_RED_FAST } fsh_red_mode;

double fsh_reduce(int64_t lo, int64_t hi, fsh_red_op op, fsh_red_mode mode,
                  double (*body)(int64_t i, void* ctx), void* ctx);
```

- **`FSH_RED_DETERMINISTIC`**: same iteration order every run; bit-reproducible.
- **`FSH_RED_FAST`**: per-worker accumulators, reassociated; faster
  but not bit-reproducible.  Not currently faster in the MVP (the
  per-worker version requires runtime support that lands with Stage C).

For floating-point reductions, the `mode` argument is the language
contract: `FSH_RED_DETERMINISTIC` matches the `@strictfp` semantics
(IEEE-754 reference), `FSH_RED_FAST` matches the default fast-math
semantics (reassociation permitted).

## Math (report §2.10)

The library separates **scalar math semantics** (always IEEE-754
accurate) from **vector math backends** (SLEEF / SVML / libmvec where
the target supports them).  Use:

- `fsh_math_*_f64` / `fsh_math_*_f32` — accurate scalar variants
  (forward to libm / LLVM intrinsics).
- `fsh_math_*_fast_*` — lower-precision, vectorisable variants
  (documented error envelope, ~1 ULP for transcendentals).
- `fsh_math_fma_*` — explicit fused multiply-add.  Contraction of
  `a*b+c` into FMA is NOT done by the compiler unless the user calls
  this function explicitly (or marks a function with the future
  `@fastmath` attribute).

## SIMD (report §2.7 + §5.2)

```c
fsh_f64x4 v = fsh_loadu_f64x4(ptr);   // unaligned load
fsh_f64x4 y = fsh_mul_f64x4(v, v);
double sum = fsh_hsum_f64x4(y);
```

The `fsh_vecN_T` types are typedefs to the target's native SIMD type
when one is available (`__m256d` on x86_64 with AVX2, `float32x4_t` on
ARM NEON).  When no native type is available they fall back to a
struct of N elements and the operations lower to scalar loops.  The
compiler's vectoriser can still vectorise the scalar fallback if it
chooses to.

`fsh_prefetch_r` / `fsh_prefetch_w` / `fsh_clflush` are advanced APIs
with target-specific implementations.  The default policy is "do not use
them" — they can make performance worse when applied without a measured
reason.

## What libfsh deliberately does NOT do (report §5.4)

- **Does not invent a replacement for all of libc.**  Keeps familiar C
  ABI interoperability and uses platform libraries for mundane system
  operations.
- **Does not reinvent MPI.**  A small Freshet binding/RAII wrapper over
  a supported MPI implementation is planned (`libfsh_mpi`); see
  report §6.2.
- **Does not prematurely recreate OpenMP.**  Interoperability with
  OpenMP-based libraries is a goal; a compatibility backend may follow.
- **Does not hard-code architecture-specific intrinsics into every call
  site.**  They live behind target-specific runtime/standard-library
  implementations with a portable fallback (Stage R3).

## ABI versioning

ABI versioning lands when the API is stable.  For now consider
everything here experimental — both the function signatures and the
struct layouts may change between releases.  Once the API is stable,
`FSH_VERSION_MAJOR` will become the ABI version, and breaking changes
will require bumping it.

## Feature-status table (report §9)

Do not read a function as a completed portability or determinism
guarantee until its row says so. Current state of the public `libfsh`
surface:

| status | API / area | notes |
|---|---|---|
| implemented + tested | allocation helpers, `fsh_span` accessors, text I/O helpers | exercised through the Penstock regression suite |
| implemented, documented fallback | `fsh_reduce` | both deterministic and fast modes run **sequentially** in the MVP; results are correct, the parallel path is future work |
| partially implemented | `fsh_thread_id` | returns `0` in the MVP (only the main caller is distinguishable) |
| implemented with documented fallback | fast math variants | call the same scalar libm routines as the accurate variants |
| implemented, scalar default | SIMD helpers | the portable scalar fallback is the default; native backends (`libfsh_simd_x86.c` / `libfsh_simd_arm.c`) are future work |
| partially implemented | binary file writes | the requested endianness conversion is ignored on write; host byte order is emitted |
| planned | `fsh_last_errno` | referenced by the header/guide but not yet implemented — do not rely on it |
| forward-looking | `fsh_parallel_for` scheduling | range/chunk/thread/affinity arguments are passed through, but not every advertised scheduling choice maps to a distinct runtime schedule yet |

The rule going forward: an API is only documented as a guarantee on
targets where the implementation meets the contract, and this table is
the place to check.
