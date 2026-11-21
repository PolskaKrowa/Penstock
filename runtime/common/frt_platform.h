/* fsht-03:03 (registry 10.03.02, ch. 25 §1) — the platform layer API.
 *
 * A conforming runtime splits into a target-independent core
 * (common/frt_common.c) and per-platform layers (platforms/<name>/)
 * implementing EXACTLY this surface: thread creation and joining,
 * work-stealing worker pool primitives, mutex equivalents, monotonic
 * time, file and stream primitives, and the program argument /
 * environment accessors.  Every function here MUST be provided by each
 * platform layer; unavailable facilities degrade silently to the
 * documented default (a knob never aborts a program, ch. 25 §4).
 *
 * Platform selection (runtime/frt_rt.c umbrella): the requested
 * --target triple is passed to the runtime COMPILE, so the compiler's
 * predefined macros (_WIN32, __wasm__) select the layer — the runtime
 * object code linked into a program is built for the program's exact
 * target (ch. 25 §1 MUST).  FRT_PLATFORM_BAREMETAL selects the
 * freestanding layer explicitly.
 */
#ifndef FRT_PLATFORM_H
#define FRT_PLATFORM_H
#include <stdint.h>
#include <stddef.h>

/* ---- threads ---------------------------------------------------------- */
/* opaque per-platform thread storage (pthread_t / HANDLE / unused) */
#define FRT_PAL_THREAD_MAX 64
typedef struct { uintptr_t handle[4]; } frt_pal_thread_t;

/* create a worker thread running fn(arg); returns 0 on success.  A
 * single-threaded platform (wasm, baremetal) returns 1 WITHOUT creating
 * anything — the pool then runs pool-of-one and parfor serialises
 * (ch. 25 §2: reductions still compute, @reproducible unchanged). */
int frt_pal_thread_create(frt_pal_thread_t* out, void* (*fn)(void*), void* arg);
void frt_pal_thread_detach(frt_pal_thread_t* t);

/* ---- mutex equivalents ------------------------------------------------ */
void frt_pal_lock(void);
void frt_pal_unlock(void);
void frt_pal_cond_wait(void);       /* wait for the queue condition */
void frt_pal_cond_signal(void);
void frt_pal_cond_broadcast(void);

/* ---- pool primitives --------------------------------------------------- */
/* workers to create for `want` (0 = auto); pool-of-one platforms return 0 */
int frt_pal_worker_count(int want);
void frt_pal_pin_thread(frt_pal_thread_t* t, int cpu);  /* best effort */
void frt_pal_yield(void);           /* sched_yield / SwitchToThread / no-op */
int frt_pal_ncpu(void);              /* logical cpus (>= 1) */
int frt_pal_smt_sibling(int cpu);    /* SMT sibling or -1 (linux probes) */

/* ---- monotonic time ---------------------------------------------------- */
uint64_t frt_pal_time_ns(void);
uint64_t frt_pal_time_ms(void);

/* ---- memory ------------------------------------------------------------- */
uint64_t frt_pal_page_size(void);
/* data-stack reservation: guard-page platform reserves PROT_NONE tails;
 * linear-memory platforms (wasm) hand back plain memory and CLAMP the
 * request (ch. 25 §2 SHOULD).  Returns the usable base or NULL. */
void* frt_pal_dstk_reserve(uint64_t* usable, uint64_t want,
                            uint64_t* got);
/* rdtsc-style cycle counter (best effort; falls back to time) */
uint64_t frt_pal_cycles(void);

/* ---- files / streams ----------------------------------------------------- */
/* 1 = the platform has a filesystem (FRT_TIMING/FRT_PROF_OUT write;
 * baremetal: 0 — they become parsed no-ops, ch. 25 §4) */
int frt_pal_has_filesystem(void);
/* sequential buffered file access used by the streaming I/O layer */
void* frt_pal_fopen_read(const char* path);
void* frt_pal_fopen_write(const char* path);
int64_t frt_pal_fread(void* f, void* buf, uint64_t n);
int64_t frt_pal_fwrite(void* f, const void* buf, uint64_t n);
int frt_pal_fflush(void* f);
int frt_pal_fclose(void* f);
int64_t frt_pal_fsize(const char* path);
int frt_pal_remove(const char* path);
void* frt_pal_stdin(void);
int frt_pal_ferror_code(void);      /* errno / GetLastError -> IoError code */
int frt_pal_eof(void* f);           /* non-zero when the stream is at EOF */

/* ---- args / env ----------------------------------------------------------- */
uint64_t frt_pal_args_count(void);
const char* frt_pal_args_get(uint64_t i);
const char* frt_pal_env_get(const char* name);   /* NULL when absent */

#endif /* FRT_PLATFORM_H */
