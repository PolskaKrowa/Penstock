/* fsht-03:03 (ch. 25 §1/§2): the WASM platform layer (wasm32-wasi and
 * wasm32-unknown-unknown — standalone/browser).
 *
 * SINGLE-THREADED: thread creation is refused (the shared-memory wasm
 * threads proposal is a registry-tracked deferral, ch. 25 §7), so the
 * pool stays pool-of-one and parfor SERIALISES: reductions still
 * compute, @reproducible output is unchanged by construction (§2).
 * Data stacks are plain linear memory (malloc) with the reservation
 * clamped; WASI file primitives come from wasi-libc's POSIX surface;
 * standalone builds without WASI get the embedder's defaults (knobs
 * arrive through the frt_config import, ch. 25 §4). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/frt_platform.h"

/* facility flags for the common core */
#define FRT_PAL_DSTK_GUARD 0     /* plain linear memory (ch. 25 §2) */
#define FRT_PAL_SIGSETJMP 0       /* no signal capture: tests run bare */
#define FRT_PAL_PROC_ARGS 0

/* ---- threads: pool-of-one by construction ------------------------------ */
int frt_pal_thread_create(frt_pal_thread_t* out, void* (*fn)(void*), void* arg) {
  (void)out; (void)fn; (void)arg;
  return 1;   /* no threads: the dispatcher drains synchronously */
}
void frt_pal_thread_detach(frt_pal_thread_t* t) { (void)t; }

/* the queue lock is uncontended (pool-of-one) but must still exist */
static int g_mu = 0;
void frt_pal_lock(void) { g_mu = 1; }
void frt_pal_unlock(void) { g_mu = 0; }
void frt_pal_cond_wait(void) { /* single-threaded: nothing to wait for */ }
void frt_pal_cond_signal(void) {}
void frt_pal_cond_broadcast(void) {}

/* ---- pool primitives --------------------------------------------------- */
int frt_pal_worker_count(int want) {
  (void)want;
  return 0;   /* pool-of-one: the calling thread runs everything */
}
void frt_pal_pin_thread(frt_pal_thread_t* t, int cpu) { (void)t; (void)cpu; }
void frt_pal_yield(void) { /* cooperative scheduler: nothing to yield */ }
int frt_pal_ncpu(void) { return 1; }
int frt_pal_smt_sibling(int cpu) { (void)cpu; return -1; }

/* ---- monotonic time ---------------------------------------------------- */
uint64_t frt_pal_time_ns(void) {
#if defined(__wasi__)
  /* WASI clock_time_get (wasi-libc maps clock_gettime) */
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
  /* standalone/browser: a monotonic counter from the embedder import
   * (frt_config); without it, an in-module counter (unspecified epoch
   * — ch. 23 §5 — and never part of observable program behaviour) */
  extern uint64_t frt_wasm_time_ns_fallback_(void);
  return frt_wasm_time_ns_fallback_();
}
uint64_t frt_pal_time_ms(void) { return frt_pal_time_ns() / 1000000ull; }

/* ---- memory ------------------------------------------------------------- */
uint64_t frt_pal_page_size(void) { return 65536; }   /* the wasm page */
void* frt_pal_dstk_reserve(uint64_t* usable, uint64_t want, uint64_t* got) {
  /* plain linear memory; CLAMP the reservation (ch. 25 §2 SHOULD) */
  if (want > (64ull << 20)) want = 64ull << 20;   /* clamp to 64 MiB */
  void* p = malloc((size_t)want);
  if (!p) {
    if (want <= (1ull << 20)) return NULL;
    want /= 2;
    p = malloc((size_t)want);
    if (!p) return NULL;
  }
  *usable = want;
  if (got) *got = want;
  return p;
}
uint64_t frt_pal_cycles(void) { return frt_pal_time_ns(); }

/* ---- files / streams ----------------------------------------------------- */
int frt_pal_has_filesystem(void) {
#if defined(__wasi__)
  return 1;    /* WASI preopens */
#else
  return 0;   /* standalone/browser: FRT_TIMING/FRT_PROF_OUT no-op */
#endif
}
void* frt_pal_fopen_read(const char* path) {
#if defined(__wasi__)
  return fopen(path, "rb");
#else
  (void)path;
  return NULL;
#endif
}
void* frt_pal_fopen_write(const char* path) {
#if defined(__wasi__)
  return fopen(path, "wb");
#else
  (void)path;
  return NULL;
#endif
}
int64_t frt_pal_fread(void* f, void* buf, uint64_t n) {
  return (int64_t)fread(buf, 1, (size_t)n, (FILE*)f);
}
int64_t frt_pal_fwrite(void* f, const void* buf, uint64_t n) {
  return (int64_t)fwrite(buf, 1, (size_t)n, (FILE*)f);
}
int frt_pal_fflush(void* f) { return fflush((FILE*)f); }
int frt_pal_fclose(void* f) { return fclose((FILE*)f); }
int64_t frt_pal_fsize(const char* path) {
#if defined(__wasi__)
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  return (int64_t)st.st_size;
#else
  (void)path;
  return -1;
#endif
}
int frt_pal_remove(const char* path) {
#if defined(__wasi__)
  return remove(path);
#else
  (void)path;
  return -1;
#endif
}
void* frt_pal_stdin(void) {
#if defined(__wasi__)
  return stdin;
#else
  return NULL;   /* reader_stdin yields a closed reader → first read Eof */
#endif
}
int frt_pal_ferror_code(void) {
  int e = errno;
  if (e == ENOENT) return 1;
  if (e == EACCES || e == EPERM) return 2;
  if (e == EBUSY) return 4;
  return 5;
}
int frt_pal_eof(void* f) { return feof((FILE*)f); }

/* ---- args / env ----------------------------------------------------------- */
/* WASI args/environ surface; standalone receives nothing (defaults) */
uint64_t frt_pal_args_count(void) { return 0; }
const char* frt_pal_args_get(uint64_t i) { (void)i; return ""; }
const char* frt_pal_env_get(const char* name) {
#if defined(__wasi__)
  const char* v = getenv(name);
  return v ? v : NULL;
#else
  (void)name;
  return NULL;   /* knobs default silently (ch. 25 §4) */
#endif
}
