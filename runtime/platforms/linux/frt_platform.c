/* fsht-03:03 (ch. 25 §1/§2): the LINUX platform layer — pthreads/POSIX.
 * The full worker pool (pthreads, work stealing, affinity close/spread),
 * monotonic clock_gettime, POSIX files, /proc-based topology probing.
 * This is the layer that owns -lpthread: the driver requests it
 * conditionally on the linux triple (ch. 25 §1) — no unconditional
 * POSIX assumption exists anywhere in common/. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "common/frt_platform.h"

/* facility flags for the common core */
#define FRT_PAL_DSTK_GUARD 1     /* mmap + PROT_NONE guard tails */
#define FRT_PAL_SIGSETJMP 1       /* sigsetjmp-based test capture */
#define FRT_PAL_PROC_ARGS 1       /* /proc/self/cmdline argv capture */

/* ---- threads ---------------------------------------------------------- */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

int frt_pal_thread_create(frt_pal_thread_t* out, void* (*fn)(void*), void* arg) {
  pthread_t th;
  if (pthread_create(&th, NULL, fn, arg) != 0) return 1;
  memset(out, 0, sizeof *out);
  memcpy(out->handle, &th, sizeof(pthread_t));
  return 0;
}
void frt_pal_thread_detach(frt_pal_thread_t* t) {
  pthread_t th;
  memcpy(&th, t->handle, sizeof(pthread_t));
  pthread_detach(th);
}

void frt_pal_lock(void) { pthread_mutex_lock(&g_mu); }
void frt_pal_unlock(void) { pthread_mutex_unlock(&g_mu); }
void frt_pal_cond_wait(void) { pthread_cond_wait(&g_cv, &g_mu); }
void frt_pal_cond_signal(void) { pthread_cond_signal(&g_cv); }
void frt_pal_cond_broadcast(void) { pthread_cond_broadcast(&g_cv); }

/* ---- pool primitives --------------------------------------------------- */
int frt_pal_worker_count(int want) {
  if (want > 0) return want;
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n <= 0) return 1;
  if (n > FRT_PAL_THREAD_MAX) n = FRT_PAL_THREAD_MAX;
  return (int)n;
}
void frt_pal_pin_thread(frt_pal_thread_t* t, int cpu) {
  pthread_t th;
  memcpy(&th, t->handle, sizeof(pthread_t));
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(th, sizeof set, &set);   /* best effort (§2) */
}
void frt_pal_yield(void) { sched_yield(); }
int frt_pal_ncpu(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}
int frt_pal_smt_sibling(int cpu) {
  char path[128];
  snprintf(path, sizeof path,
           "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
           cpu);
  FILE* f = fopen(path, "r");
  if (!f) return -1;
  int sib = -1;
  char buf[64];
  if (fgets(buf, sizeof buf, f)) {
    int a = -1, b = -1;
    if (sscanf(buf, "%d,%d", &a, &b) == 2) {
      sib = (a == cpu) ? b : a;
    } else if (sscanf(buf, "%d-%d", &a, &b) == 2 && b - a == 1) {
      sib = (a == cpu) ? b : a;
    }
  }
  fclose(f);
  return sib;
}

/* ---- monotonic time ---------------------------------------------------- */
uint64_t frt_pal_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t frt_pal_time_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* ---- memory ------------------------------------------------------------- */
uint64_t frt_pal_page_size(void) {
  long sz = sysconf(_SC_PAGESIZE);
  return sz > 0 ? (uint64_t)sz : 4096;
}
void* frt_pal_dstk_reserve(uint64_t* usable, uint64_t want, uint64_t* got) {
  /* RW front + PROT_NONE guard tail (MAP_NORESERVE) */
  const uint64_t guard = 1ull << 20;   /* 1 MiB */
  for (;;) {
    uint64_t total = want + guard;
    char* p = (char*)mmap(NULL, (size_t)total, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p != MAP_FAILED) {
      if (mprotect(p + want, (size_t)guard, PROT_NONE) != 0) {
        munmap(p, (size_t)total);
      } else {
        *usable = want;
        if (got) *got = total;
        return p;
      }
    }
    if (want <= (16ull << 20)) return NULL;   /* floor: 16 MiB */
    want /= 2;
  }
}
uint64_t frt_pal_cycles(void) {
#if defined(__x86_64__) || defined(__i386__)
  return (uint64_t)__builtin_ia32_rdtsc();
#else
  return frt_pal_time_ns();
#endif
}

/* ---- files / streams ----------------------------------------------------- */
int frt_pal_has_filesystem(void) { return 1; }
void* frt_pal_fopen_read(const char* path) { return fopen(path, "rb"); }
void* frt_pal_fopen_write(const char* path) { return fopen(path, "wb"); }
int64_t frt_pal_fread(void* f, void* buf, uint64_t n) {
  return (int64_t)fread(buf, 1, (size_t)n, (FILE*)f);
}
int64_t frt_pal_fwrite(void* f, const void* buf, uint64_t n) {
  return (int64_t)fwrite(buf, 1, (size_t)n, (FILE*)f);
}
int frt_pal_fflush(void* f) { return fflush((FILE*)f); }
int frt_pal_fclose(void* f) { return fclose((FILE*)f); }
int64_t frt_pal_fsize(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
  return (int64_t)st.st_size;
}
int frt_pal_remove(const char* path) { return remove(path); }
void* frt_pal_stdin(void) { return stdin; }
int frt_pal_ferror_code(void) {
  int e = errno;
  if (e == ENOENT) return 1;
  if (e == EACCES || e == EPERM) return 2;
  if (e == EBUSY) return 4;
  return 5;
}
int frt_pal_eof(void* f) { return feof((FILE*)f); }

/* ---- args / env ----------------------------------------------------------- */
/* argv capture happens in common/ via /proc/self/cmdline (FRT_PAL_PROC_ARGS) */
uint64_t frt_pal_args_count(void) { return 0; }
const char* frt_pal_args_get(uint64_t i) { (void)i; return ""; }
const char* frt_pal_env_get(const char* name) {
  const char* v = getenv(name);
  return v ? v : NULL;
}
