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
#include <fcntl.h>
#include <sys/uio.h>   /* pread/pwrite via fileno'd FILE* */
#include "../../common/frt_platform.h"

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

/* ---- machine query / huge-page hint / NUMA policy ---------- */
/* Machine query / huge-page hint / NUMA topology, all through sysfs+proc.
 * Every probe is best-effort: a missing or unreadable file leaves the
 * field at its "unknown" value (0), never fails the program. */

#include <sys/syscall.h>

/* shared with runtime/common/frt_common.c (the pure list parser) */
extern int frt_parse_cpu_list(const char* s, int* out, int cap);

/* parse a sysfs size like "32K", "512K", "20480K", "1M", "2M" into bytes */
static int64_t frt_parse_sysfs_size(const char* s) {
  if (!s || !*s) return 0;
  char* end = NULL;
  long long v = strtoll(s, &end, 10);
  if (end == s) return 0;
  if (end && (*end == 'K' || *end == 'k')) v *= 1024;
  else if (end && (*end == 'M' || *end == 'm')) v *= 1024 * 1024;
  else if (end && (*end == 'G' || *end == 'g')) v *= 1024 * 1024 * 1024;
  return (int64_t)v;
}

/* read one small integer/text file under /sys into out (best effort) */
static int frt_sysfs_read_i64(const char* dir, const char* leaf,
                              int64_t* out) {
  char path[192];
  snprintf(path, sizeof path, "%s/%s", dir, leaf);
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  char buf[64];
  int ok = 0;
  if (fgets(buf, sizeof buf, f)) {
    *out = frt_parse_sysfs_size(buf);
    ok = 1;
  }
  fclose(f);
  return ok;
}

int frt_pal_query_machine(frt_machine_t* out) {
  if (!out) return 0;
  memset(out, 0, sizeof *out);
  int known = 0;

  /* logical CPUs online */
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu <= 0) ncpu = 1;
  out->threads_logical = (int32_t)ncpu;

  /* page size */
  long pg = sysconf(_SC_PAGESIZE);
  out->page_bytes = pg > 0 ? (int64_t)pg : 4096;
  known = 1;

  /* SMT + physical cores: unique (physical_package_id, core_id) pairs */
  {
    int ncpuC = (int)ncpu;
    if (ncpuC > 256) ncpuC = 256;   /* bounded probe */
    int32_t pkg[256], core[256];
    int nseen = 0, nsmt = 0;
    for (int c = 0; c < ncpuC; ++c) {
      char d[96];
      snprintf(d, sizeof d, "/sys/devices/system/cpu/cpu%d/topology", c);
      int64_t pk = -1, co = -1;
      frt_sysfs_read_i64(d, "physical_package_id", &pk);
      frt_sysfs_read_i64(d, "core_id", &co);
      int dup = 0;
      for (int i = 0; i < nseen; ++i)
        if (pkg[i] == (int32_t)pk && core[i] == (int32_t)co) { dup = 1; break; }
      if (!dup && nseen < 256) {
        pkg[nseen] = (int32_t)pk;
        core[nseen] = (int32_t)co;
        ++nseen;
      }
      int sib = frt_pal_smt_sibling(c);
      if (sib >= 0 && sib != c) nsmt = 1;
    }
    out->cores_physical = nseen > 0 ? nseen : (int32_t)ncpu;
    out->smt = nsmt;
  }

  /* caches of cpu0: level 1 data, level 2, level 3 (shared) */
  for (int idx = 0; idx < 8; ++idx) {
    char d[96];
    snprintf(d, sizeof d, "/sys/devices/system/cpu/cpu0/cache/index%d", idx);
    int64_t lvl = 0, sz = 0;
    if (!frt_sysfs_read_i64(d, "level", &lvl)) continue;
    /* type: Data / Instruction / unified — only Data+unified count */
    char tp[96];
    snprintf(tp, sizeof tp, "%s/type", d);
    FILE* f = fopen(tp, "r");
    char tb[32] = {0};
    if (f) { if (fgets(tb, sizeof tb, f)) {} fclose(f); }
    if (tb[0] == 'I' && tb[1] == 'n') continue;   /* Instruction */
    frt_sysfs_read_i64(d, "size", &sz);
    if (lvl == 1 && !out->l1d_bytes) out->l1d_bytes = sz;
    else if (lvl == 2 && !out->l2_bytes) out->l2_bytes = sz;
    else if (lvl == 3 && !out->l3_bytes) out->l3_bytes = sz;
  }

  /* NUMA nodes: /sys/devices/system/node/nodeN with cpus */
  {
    int nodes = 0;
    for (int n = 0; n < 64; ++n) {
      char d[96];
      snprintf(d, sizeof d, "/sys/devices/system/node/node%d", n);
      char cp[128];
      snprintf(cp, sizeof cp, "%s/cpulist", d);
      FILE* f = fopen(cp, "r");
      if (!f) break;
      char cb[128];
      int has = 0;
      if (fgets(cb, sizeof cb, f)) has = cb[0] != '\n' && cb[0] != 0;
      fclose(f);
      if (has) ++nodes;
      else {
        /* node exists but is cpu-less (memory-only): probe cpumap too */
        char cm[128];
        snprintf(cm, sizeof cm, "%s/cpumap", d);
        FILE* g = fopen(cm, "r");
        if (!g) break;
        fclose(g);
      }
    }
    out->numa_nodes = nodes > 0 ? nodes : 1;
  }

  /* RAM from /proc/meminfo (kB lines) */
  {
    FILE* f = fopen("/proc/meminfo", "r");
    if (f) {
      char line[256];
      while (fgets(line, sizeof line, f)) {
        long long kb = 0;
        if (sscanf(line, "MemTotal: %lld", &kb) == 1)
          out->mem_total_bytes = (int64_t)kb * 1024;
        else if (sscanf(line, "MemAvailable: %lld", &kb) == 1)
          out->mem_available_bytes = (int64_t)kb * 1024;
      }
      fclose(f);
    }
  }
  return known;
}

void frt_pal_madvise_huge(void* p, uint64_t bytes) {
#if defined(MADV_HUGEPAGE)
  (void)madvise(p, (size_t)bytes, MADV_HUGEPAGE);
#else
  (void)p; (void)bytes;
#endif
}

int frt_pal_numa_nodes(void) {
  int n = 0;
  for (int i = 0; i < 64; ++i) {
    char d[96];
    snprintf(d, sizeof d, "/sys/devices/system/node/node%d/cpulist", i);
    FILE* f = fopen(d, "r");
    if (!f) break;
    fclose(f);
    ++n;
  }
  return n > 0 ? n : 1;
}

int frt_pal_cpu_node(int cpu) {
  for (int i = 0; i < 64; ++i) {
    char d[96];
    snprintf(d, sizeof d, "/sys/devices/system/node/node%d/cpulist", i);
    FILE* f = fopen(d, "r");
    if (!f) return -1;   /* no node dirs -> single node, cpu unattributed */
    char buf[512];
    int found = 0;
    if (fgets(buf, sizeof buf, f)) {
      int list[256];
      int n = frt_parse_cpu_list(buf, list, 256);
      for (int k = 0; k < n; ++k)
        if (list[k] == cpu) { found = 1; break; }
    }
    fclose(f);
    if (found) return i;
  }
  return -1;
}

/* set_mempolicy via raw syscall: MPOL_DEFAULT=0, MPOL_BIND=2,
 * MPOL_INTERLEAVE=3; MPOL_F_MEMS_ALLOWED nodemask query not needed —
 * the full host mask (~0ULL) is used, and cgroups clamp it silently. */
int frt_pal_numa_policy(int mode) {
#if defined(SYS_set_mempolicy)
  long nodemask = ~0L;   /* all nodes the policy may touch */
  long maxnode = 8 * (long)sizeof(long);
  long m;
  switch (mode) {
    case 1: m = 2; break;   /* MPOL_BIND: local node */
    case 2: m = 3; break;  /* MPOL_INTERLEAVE */
    default: m = 0; break; /* MPOL_DEFAULT */
  }
  long rc = syscall(SYS_set_mempolicy, m, &nodemask, maxnode);
  (void)rc;   /* best effort: cgroup restrictions return EPERM here */
  return mode;
#else
  (void)mode;
  return -1;
#endif
}

/* ---- positioned I/O / mmap (linux) -------------------------------- */
int64_t frt_pal_pread(void* f, void* buf, uint64_t n, int64_t off) {
  if (!f) return -1;
  return (int64_t)pread(fileno((FILE*)f), buf, (size_t)n, (off_t)off);
}
int64_t frt_pal_pwrite(void* f, const void* buf, uint64_t n, int64_t off) {
  if (!f) return -1;
  return (int64_t)pwrite(fileno((FILE*)f), buf, (size_t)n, (off_t)off);
}
int32_t frt_pal_fsync(void* f) {
  if (!f) return -1;
  return fsync(fileno((FILE*)f)) == 0 ? 0 : -1;
}
int32_t frt_pal_ftruncate(void* f, int64_t size) {
  if (!f) return -1;
  return ftruncate(fileno((FILE*)f), (off_t)size) == 0 ? 0 : -1;
}
void* frt_pal_mmap_ro(const char* path, uint64_t* lenOut) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
  void* p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return NULL;
  if (lenOut) *lenOut = (uint64_t)st.st_size;
  return p;
}
void* frt_pal_mmap_rw(const char* path, uint64_t* lenOut) {
  int fd = open(path, O_RDWR);
  if (fd < 0) return NULL;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
  void* p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return NULL;
  if (lenOut) *lenOut = (uint64_t)st.st_size;
  return p;
}
int32_t frt_pal_munmap(void* p, uint64_t len) {
  if (!p) return -1;
  return munmap(p, (size_t)len) == 0 ? 0 : -1;
}
