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

/* ---- machine query -- */
/* The machine-profile surface the language exposes through the
 * freshet.machine_* intrinsics and the `machine` stdlib module: one
 * structure filled once (cached), value 0 = unknown/absent on this
 * platform (ch. 25 §4: a query NEVER aborts a program).  Linux fills it
 * from sysconf, /sys/devices/system/{cpu,node}, /proc/meminfo; other
 * layers return all-zero (the documented "unknown" spelling). */
typedef struct frt_machine {
  int32_t cores_physical;   /* unique (pkg, core) pairs seen */
  int32_t threads_logical; /* online logical CPUs */
  int32_t smt;             /* 1 when any SMT sibling pair was found */
  int32_t numa_nodes;      /* NUMA nodes with CPUs (>= 1; 0 = unknown) */
  int64_t l1d_bytes;       /* per-core L1D, 0 = unknown */
  int64_t l2_bytes;        /* per-core L2, 0 = unknown */
  int64_t l3_bytes;        /* per-package L3, 0 = unknown */
  int64_t page_bytes;      /* OS page size (always known, >= 1) */
  int64_t mem_total_bytes;      /* 0 = unknown */
  int64_t mem_available_bytes;   /* 0 = unknown */
} frt_machine_t;
/* fills *out; returns 1 (query performed) or 0 (nothing known).  Fields
 * an individual platform cannot determine stay 0 = unknown. */
int frt_pal_query_machine(frt_machine_t* out);

/* ---- transparent huge-page hint ------------------------------- */
/* best-effort MADV_HUGEPAGE (or host equivalent) over [p, p+n); a no-op
 * on platforms without the facility.  Enabled for big allocations when
 * the program opts in through FRT_HUGEPAGE=1 (frt_common.c). */
void frt_pal_madvise_huge(void* p, uint64_t bytes);

/* ---- NUMA topology + memory policy ---------------------------- */
/* number of NUMA nodes that carry CPUs (>= 1 when known; 0 = unknown /
 * single-socket-desktop reporting is still >= 1 node) */
int frt_pal_numa_nodes(void);
/* the NUMA node owning `cpu` (-1 = unknown) */
int frt_pal_cpu_node(int cpu);
/* set the CALLING thread's memory policy for subsequent first-touch
 * allocations.  mode: 0 = default (system), 1 = local (bind to the
 * current node), 2 = interleave (round-robin pages across nodes).
 * Returns the mode actually applied (-1 = platform without the
 * facility); best effort — cgroups may refuse. */
int frt_pal_numa_policy(int mode);

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

/* ---- positioned I/O --
 * pread/pwrite at an explicit offset (parallel regions writing disjoint
 * file ranges), fsync/ftruncate (durable billion-voxel checkpoints), and
 * read-only / read-write mmap windows over file contents.  Handles are
 * the same FILE* stream objects the sequential layer hands out; offsets
 * are absolute bytes.  Platforms without the facility return -1 / NULL
 * and the language surfaces document the fallibility. */
int64_t frt_pal_pread(void* f, void* buf, uint64_t n, int64_t off);
int64_t frt_pal_pwrite(void* f, const void* buf, uint64_t n, int64_t off);
int32_t frt_pal_fsync(void* f);
int32_t frt_pal_ftruncate(void* f, int64_t size);
void* frt_pal_mmap_ro(const char* path, uint64_t* lenOut);
void* frt_pal_mmap_rw(const char* path, uint64_t* lenOut);
int32_t frt_pal_munmap(void* p, uint64_t len);

/* ---- args / env ----------------------------------------------------------- */
uint64_t frt_pal_args_count(void);
const char* frt_pal_args_get(uint64_t i);
const char* frt_pal_env_get(const char* name);   /* NULL when absent */

#endif /* FRT_PLATFORM_H */
