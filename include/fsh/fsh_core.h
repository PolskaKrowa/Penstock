/* fsh_core.h — memory, spans, errors (report §5.2 "Core features").
 *
 * The historical runtime exposes:
 *   void* frt_malloc(int64_t size, int64_t align);
 *   void  frt_free(void* p);
 *   void  frt_memset(void* p, int64_t n, int64_t unused);
 *   void  frt_memcpy(void* d, void* s, int64_t n);
 *   void* frt_soa_new(int64_t n, int32_t nf, int64_t* sizes);
 *   void  frt_soa_free(void* blk);
 *
 * These are the runtime ABI used by the generated LLVM IR.  The fsh_core
 * API layer:
 *   - renames them to the public `fsh_*` namespace,
 *   - adds overflow-checked sizing (report §2.7 / §5.2),
 *   - introduces `fsh_span<T>` (a typed pointer+length view with optional
 *     stride) for use as function parameters — the compiler lowers a
 *     proven-contiguous span to a zero-overhead pointer+length pair,
 *   - adds an explicit error code type so allocation failure does not
 *     `abort()` by default; the caller can opt into abort-on-failure via
 *     `fsh_set_alloc_fail_mode(FSH_FAIL_ABORT)`.
 */
#ifndef FSH_CORE_H
#define FSH_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- error model ------------------------------------------- */
typedef enum {
  FSH_OK = 0,
  FSH_ERR_NOMEM = 1,        /* allocation failed */
  FSH_ERR_OVERFLOW = 2,     /* size computation overflowed (e.g. count*size) */
  FSH_ERR_INVALID = 3,     /* invalid argument (null, negative, etc.) */
  FSH_ERR_RANGE = 4,        /* span/range bounds violated */
  FSH_ERR_UNSUPPORTED = 5,  /* feature not available on this target */
} fsh_err;

/* Fail-mode for allocation helpers.  Default is FSH_FAIL_RETURN (returns
 * NULL + sets errno), matching POSIX.  FSH_FAIL_ABORT calls abort() —
 * useful for HPC kernels where recovery is impossible. */
typedef enum {
  FSH_FAIL_RETURN = 0,
  FSH_FAIL_ABORT = 1,
} fsh_fail_mode;
void fsh_set_alloc_fail_mode(fsh_fail_mode m);

/* Returns the textual name of an fsh_err code. */
const char* fsh_err_name(fsh_err e);

/* ---------------- memory ------------------------------------------------ */
/* fsh_alloc: aligned allocation.  `align` must be a power of two and >= 16.
 * Returns NULL + sets fsh_errno on failure (when fail_mode == RETURN). */
void* fsh_alloc(int64_t bytes, int64_t align);

/* fsh_calloc: zero-initialised allocation. */
void* fsh_calloc(int64_t count, int64_t elem_size);

/* fsh_free: releases memory returned by fsh_alloc / fsh_calloc.  NULL-safe. */
void fsh_free(void* p);

/* fsh_realloc: grows/shrinks an allocation.  May move the buffer.  The
 * contents up to min(old, new) are preserved. */
void* fsh_realloc(void* p, int64_t new_bytes);

/* Overflow-checked multiplication: returns a* b if it does not overflow,
 * or 0 with fsh_errno=FSH_ERR_OVERFLOW otherwise.  Used internally by
 * fsh_calloc / fsh_soa_alloc and exposed for callers that need to compute
 * safe allocation sizes themselves. */
int64_t fsh_safe_mul(int64_t a, int64_t b);

/* ---------------- spans ------------------------------------------------- */
/* A span is a typed pointer + length view; the compiler lowers a proven
 * contiguous span to a plain pointer+length pair with no overhead.  In C
 * it is exposed as the untyped structure below; Freshet-side generics
 * provide `span[T]`. */
typedef struct {
  void*    data;
  int64_t  len;     /* number of elements */
  int64_t  stride;  /* bytes between consecutive elements; 0 == contiguous */
} fsh_span;

/* Constructs a contiguous span over the given buffer. */
fsh_span fsh_span_make(void* data, int64_t len);

/* Constructs a strided span (e.g. a column view of a row-major matrix). */
fsh_span fsh_span_make_strided(void* data, int64_t len, int64_t stride);

/* Bounds-checked element access.  Returns NULL on out-of-range.  In a
 * checked Freshet build, this is a hard error; in C the caller must
 * inspect the return. */
void* fsh_span_at(const fsh_span* s, int64_t index);

/* ---------------- SoA / AoSoA ----------------------------------------- */
/* fsh_soa_alloc: allocates the descriptor block + per-plane storage for
 * `n` records of a struct with `nf` fields of per-record sizes `sizes[nf]`.
 * Returns the descriptor (see runtime/frt_rt.c) on success, NULL on
 * overflow.  Equivalent to frt_soa_new with overflow checking. */
void* fsh_soa_alloc(int64_t n, int32_t nf, const int64_t* sizes);

/* Releases an SoA descriptor returned by fsh_soa_alloc.  NULL-safe. */
void fsh_soa_free(void* blk);

/* Returns the plane pointer for field index `i` of the descriptor. */
void* fsh_soa_plane(void* blk, int32_t i);

/* Returns the record count of an SoA descriptor. */
int64_t fsh_soa_len(void* blk);

/* ---------------- alignment model --------------------------------------- */
/* Report §5.2: "A single documented alignment model".  The library
 * guarantees that:
 *   - fsh_alloc(bytes, align) returns an address that is a multiple of
 *     `align` (or NULL),
 *   - the minimum alignment that callers may rely on is 16 bytes,
 *   - SoA plane allocations are 64-byte aligned (cache-line aligned) so
 *     vector loads never straddle a line.
 *
 * The "declared struct alignment" (from `align(N)` in Freshet source)
 * and the "allocation alignment" (above) are distinct: a struct declared
 * `align(64)` may live inside an `fsh_alloc(bytes, 16)` allocation and
 * therefore NOT be 64-aligned at runtime.  Code that requires a
 * specific alignment must request it from the allocator. */
int64_t fsh_alloc_min_align(void);   /* returns 16 */
int64_t fsh_soa_plane_align(void);   /* returns 64 */

/* report §9 (previously "planned"): errno of the most recent failed fsh
 * call on THIS thread.  Call it immediately after the failure — anything
 * else the thread does in between may overwrite errno.  Returns and
 * resets the snapshot (0 = no error pending). */
int32_t fsh_last_errno(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_CORE_H
