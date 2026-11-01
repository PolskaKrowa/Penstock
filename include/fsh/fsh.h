/* fsh.h — Freshet HPC standard library, top-level umbrella.
 *
 * This is the public API surface described in the report §5 ("libfsh").
 * It is layered over the existing `frt_*` runtime ABI (runtime/frt_rt.c):
 * the headers in include/fsh/ wrap the runtime with typed, ownership-aware
 * Freshet-friendly functions, while preserving the existing C ABI used by
 * generated programs.
 *
 * Public API boundary (report §5.1):
 *
 *     include/fsh/             # public API, language declarations
 *     libfsh_core              # memory, spans, errors, portable utilities
 *     libfsh_math              # accurate and optional fast/vector math
 *     libfsh_simd             # portable SIMD abstractions and target kernels
 *     libfsh_parallel          # pools, reductions, scheduling, atomics
 *     libfsh_io                # buffered and binary scientific I/O
 *     libfsh_mpi               # optional MPI bindings/interop
 *     libfsh_gpu               # future accelerator runtime/API
 *     runtime/frt_*            # internal ABI required by generated programs
 *
 * For now the whole library is one compilation unit (one .a / one .so)
 * implementing these headers — the split above is the *intended* public
 * boundary, not a build-time requirement.  ABI versioning lands when the
 * API is stable; for now consider everything here experimental.
 *
 * USAGE from C (and from Freshet via the existing `extern fn` interop):
 *
 *     #include <fsh/fsh.h>
 *     void* p = fsh_alloc(1024, 64);
 *     ...
 *     fsh_free(p);
 *
 * The library is C99 so it can be called from any language with a C FFI.
 * Freshet-side wrappers in the standard library will call these.
 */
#ifndef FSH_H
#define FSH_H

#include "fsh_core.h"
#include "fsh_parallel.h"
#include "fsh_math.h"
#include "fsh_simd.h"
#include "fsh_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Library version (separate from the compiler version).  Bumped when
 * the ABI changes; see docs/libfsh-versioning.md (planned). */
#define FSH_VERSION_MAJOR 0
#define FSH_VERSION_MINOR 1
#define FSH_VERSION_PATCH 0
#define FSH_VERSION_STRING "0.1.0-experimental"

/* Returns a static string describing the build (compiler, target, SIMD
 * backend).  Used by `freshet --version` and by the LSP server-info. */
const char* fsh_build_info(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_H
