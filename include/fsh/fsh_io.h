/* fsh_io.h — buffered and binary scientific I/O (report §5.2 + §6.6).
 *
 * The historical runtime exposes only `frt_print_str`, `frt_print_f64`,
 * `frt_print_i64`, `frt_print_u64`, `frt_println`.  The fsh_io layer
 * adds:
 *   - buffered sequential I/O with explicit endianness/layout conventions,
 *   - simple dataset metadata (version, layout, provenance) so files
 *     written by one build of a simulation can be read by another,
 *   - checkpoint/restart primitives (a future extension).
 *
 * Parallel I/O is intentionally NOT in this header — that lives in
 * libfsh_mpi over a real MPI implementation (report §5.4 / §6.2).
 * The CPU-side I/O here is sequential.
 */
#ifndef FSH_IO_H
#define FSH_IO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- print (kept compatible with frt_print_*) ------------- */
void fsh_print_str(const char* s, int64_t len);
void fsh_print_f64(double v, int64_t digits);
void fsh_print_i64(int64_t v);
void fsh_print_u64(uint64_t v);
void fsh_println(void);

/* ---------------- binary I/O ----------------------------------------- */
typedef enum {
  FSH_ENDIAN_NATIVE = 0,    /* host byte order; fastest, non-portable */
  FSH_ENDIAN_LITTLE = 1,   /* explicit LE; portable */
  FSH_ENDIAN_BIG = 2,        /* explicit BE; portable */
} fsh_endian;

/* Binary file handle.  Opaque; use the accessors below. */
typedef struct fsh_file fsh_file;

/* Open / close.  `path` is filesystem path; `mode` is "rb" / "wb" / "ab".
 * Returns NULL on failure (sets fsh_errno). */
fsh_file* fsh_file_open(const char* path, const char* mode);
void      fsh_file_close(fsh_file* f);

/* Typed binary I/O with explicit endianness.  Returns the number of
 * elements actually read/written (may be < count on EOF or error). */
size_t fsh_file_read_f64(fsh_file* f, double* dst, size_t count, fsh_endian e);
size_t fsh_file_write_f64(fsh_file* f, const double* src, size_t count, fsh_endian e);
size_t fsh_file_read_i32(fsh_file* f, int32_t* dst, size_t count, fsh_endian e);
size_t fsh_file_write_i32(fsh_file* f, const int32_t* src, size_t count, fsh_endian e);

/* ---------------- dataset metadata (planned) ------------------------- */
/* A dataset header records:
 *   magic, version, schema (struct name + per-field type+size),
 *   endianness, optional checksum, optional provenance string.
 *
 * This is intentionally minimal in the MVP — the schema is just a
 * string + an int64 record count.  A full schema-aware format would
 * require versioning across Freshet releases; defer until the
 * language's struct layout stabilises. */
typedef struct {
  char     schema_name[64];
  int32_t  schema_version;
  int64_t  record_count;
  fsh_endian endianness;
} fsh_dataset_header;

int fsh_file_write_header(fsh_file* f, const fsh_dataset_header* h);
int fsh_file_read_header(fsh_file* f, fsh_dataset_header* h);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FSH_IO_H
