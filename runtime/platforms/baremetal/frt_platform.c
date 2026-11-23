/* fsht-03:03 (ch. 25 §1/§7): the BAREMETAL platform layer — the
 * freestanding fallback.  No ratified baremetal TARGET exists (only the
 * layer's existence contract); everything degrades to the documented
 * default: no threads (pool-of-one, parfor serialises), no files
 * (FRT_TIMING/FRT_PROF_OUT are parsed no-ops), no environment (knobs
 * take compile-time defaults), monotonic time from a software counter,
 * allocations from a simple bump heap. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../common/frt_platform.h"

/* facility flags for the common core */
#define FRT_PAL_DSTK_GUARD 0
#define FRT_PAL_SIGSETJMP 0
#define FRT_PAL_PROC_ARGS 0

/* ---- threads: none ------------------------------------------------------ */
int frt_pal_thread_create(frt_pal_thread_t* out, void* (*fn)(void*), void* arg) {
  (void)out; (void)fn; (void)arg;
  return 1;
}
void frt_pal_thread_detach(frt_pal_thread_t* t) { (void)t; }
void frt_pal_lock(void) {}
void frt_pal_unlock(void) {}
void frt_pal_cond_wait(void) {}
void frt_pal_cond_signal(void) {}
void frt_pal_cond_broadcast(void) {}

/* ---- pool primitives ---------------------------------------------------- */
int frt_pal_worker_count(int want) { (void)want; return 0; }
void frt_pal_pin_thread(frt_pal_thread_t* t, int cpu) { (void)t; (void)cpu; }
void frt_pal_yield(void) {}
int frt_pal_ncpu(void) { return 1; }
int frt_pal_smt_sibling(int cpu) { (void)cpu; return -1; }

/* ---- monotonic time: software counter (unspecified epoch) --------------- */
static volatile uint64_t g_tick = 0;
uint64_t frt_pal_time_ns(void) { return ++g_tick; }
uint64_t frt_pal_time_ms(void) { return ++g_tick; }

/* ---- memory: bump heap ---------------------------------------------------- */
static uint8_t g_heap[1 << 20];   /* 1 MiB static pool */
static uint64_t g_heap_used = 0;
static void* bm_alloc(uint64_t n) {
  if (n & 7) n = (n | 7) + 1;
  if (g_heap_used + n > sizeof g_heap) return NULL;
  void* p = g_heap + g_heap_used;
  g_heap_used += n;
  return p;
}
uint64_t frt_pal_page_size(void) { return 4096; }
void* frt_pal_dstk_reserve(uint64_t* usable, uint64_t want, uint64_t* got) {
  if (want > (1ull << 20)) want = 1ull << 20;   /* clamped */
  void* p = bm_alloc(want);
  if (!p) return NULL;
  *usable = want;
  if (got) *got = want;
  return p;
}
uint64_t frt_pal_cycles(void) { return ++g_tick; }

/* ---- files: none ----------------------------------------------------------- */
int frt_pal_has_filesystem(void) { return 0; }
void* frt_pal_fopen_read(const char* path) { (void)path; return NULL; }
void* frt_pal_fopen_write(const char* path) { (void)path; return NULL; }
int64_t frt_pal_fread(void* f, void* buf, uint64_t n) { (void)f; (void)buf; return (int64_t)n; }
int64_t frt_pal_fwrite(void* f, const void* buf, uint64_t n) { (void)f; (void)buf; return 0; }
int frt_pal_fflush(void* f) { (void)f; return 0; }
int frt_pal_fclose(void* f) { (void)f; return 0; }
int64_t frt_pal_fsize(const char* path) { (void)path; return -1; }
int frt_pal_remove(const char* path) { (void)path; return -1; }
void* frt_pal_stdin(void) { return NULL; }
int frt_pal_ferror_code(void) { return 5; }
int frt_pal_eof(void* f) { (void)f; return 1; }

/* ---- args / env: compile-time defaults only ------------------------------- */
uint64_t frt_pal_args_count(void) { return 0; }
const char* frt_pal_args_get(uint64_t i) { (void)i; return ""; }
const char* frt_pal_env_get(const char* name) { (void)name; return NULL; }
