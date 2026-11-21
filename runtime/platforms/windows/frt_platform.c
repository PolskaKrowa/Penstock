/* fsht-03:03 (ch. 25 §1/§2): the WINDOWS platform layer — Win32 threads,
 * condition variables, QueryPerformanceCounter, MSVCRT files.  The
 * worker pool is full (Win32 threads); FRT_AFFINITY pinning is best
 * effort (SetThreadAffinityMask; ch. 25 §2).  -lpthread is NOT
 * requested: this layer needs no POSIX thread library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "common/frt_platform.h"

/* facility flags for the common core */
#define FRT_PAL_DSTK_GUARD 1     /* VirtualAlloc + PAGE_NOACCESS guards */
#define FRT_PAL_SIGSETJMP 1       /* sigsetjmp via CRT */
#define FRT_PAL_PROC_ARGS 0

/* ---- threads ---------------------------------------------------------- */
static SRWLOCK g_mu = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;

int frt_pal_thread_create(frt_pal_thread_t* out, void* (*fn)(void*), void* arg) {
  HANDLE th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)fn, arg, 0, NULL);
  if (!th) return 1;
  memset(out, 0, sizeof *out);
  memcpy(out->handle, &th, sizeof(HANDLE));
  return 0;
}
void frt_pal_thread_detach(frt_pal_thread_t* t) {
  HANDLE th;
  memcpy(&th, t->handle, sizeof(HANDLE));
  CloseHandle(th);   /* threads run detached; the handle is released */
}

void frt_pal_lock(void) { AcquireSRWLockExclusive(&g_mu); }
void frt_pal_unlock(void) { ReleaseSRWLockExclusive(&g_mu); }
void frt_pal_cond_wait(void) {
  SleepConditionVariableSRW(&g_cv, &g_mu, INFINITE, 0);
}
void frt_pal_cond_signal(void) { WakeConditionVariable(&g_cv); }
void frt_pal_cond_broadcast(void) { WakeAllConditionVariable(&g_cv); }

/* ---- pool primitives --------------------------------------------------- */
int frt_pal_worker_count(int want) {
  if (want > 0) return want;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  int n = (int)si.dwNumberOfProcessors;
  if (n <= 0) return 1;
  if (n > FRT_PAL_THREAD_MAX) n = FRT_PAL_THREAD_MAX;
  return n;
}
void frt_pal_pin_thread(frt_pal_thread_t* t, int cpu) {
  HANDLE th;
  memcpy(&th, t->handle, sizeof(HANDLE));
  SetThreadAffinityMask(th, (DWORD_PTR)1 << (cpu % 64));  /* best effort */
}
void frt_pal_yield(void) { SwitchToThread(); }
int frt_pal_ncpu(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
int frt_pal_smt_sibling(int cpu) {
  /* no topology walk in the first cut — no co-scheduling pairs */
  (void)cpu;
  return -1;
}

/* ---- monotonic time ---------------------------------------------------- */
uint64_t frt_pal_time_ns(void) {
  LARGE_INTEGER freq, c;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&c);
  return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
uint64_t frt_pal_time_ms(void) {
  return frt_pal_time_ns() / 1000000ull;
}

/* ---- memory ------------------------------------------------------------- */
uint64_t frt_pal_page_size(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize ? (uint64_t)si.dwPageSize : 4096;
}
void* frt_pal_dstk_reserve(uint64_t* usable, uint64_t want, uint64_t* got) {
  const uint64_t guard = 1ull << 20;
  for (;;) {
    uint64_t total = want + guard;
    char* p = (char*)VirtualAlloc(NULL, (SIZE_T)total, MEM_RESERVE,
                                  PAGE_NOACCESS);
    if (p) {
      if (VirtualProtect(p, (SIZE_T)want, PAGE_READWRITE, NULL)) {
        *usable = want;
        if (got) *got = total;
        return p;
      }
      VirtualFree(p, 0, MEM_RELEASE);
    }
    if (want <= (16ull << 20)) return NULL;
    want /= 2;
  }
}
uint64_t frt_pal_cycles(void) { return frt_pal_time_ns(); }

/* ---- files / streams (MSVCRT) -------------------------------------------- */
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
  FILE* f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  return sz;
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
uint64_t frt_pal_args_count(void) { return 0; }
const char* frt_pal_args_get(uint64_t i) { (void)i; return ""; }
const char* frt_pal_env_get(const char* name) {
  const char* v = getenv(name);
  return v ? v : NULL;
}
