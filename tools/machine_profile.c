/* machine_profile.c — bundled machine-profile microbenchmark
 *
 * The bundled machine-profile microbenchmark the numerical profiling
 * report (registry fsht-03:04-10.06.01) compares against: STREAM-style
 * memory bandwidth (triad), an FMA-throughput peak, and the runtime's
 * machine query (caches, page, RAM, SMT/NUMA topology).  The result is
 * a measured file the report reads back:
 *
 *     machine_profile [out.machprof]
 *
 * default output path: ./machine_profile.machprof (or argv[1]); the
 * report looks for <executable>.machprof next to the profiled binary.
 *
 * Everything is honest by construction: each number carries the
 * measurement it came from, and a kernel the machine cannot run reports
 * 0 rather than an estimate.  Bandwidth kernels size their working sets
 * to 4x L3 (fallback 256 MiB when L3 is unknown) and report the best of
 * R repeats; the FMA peak uses 16 independent fma chains so the vector
 * unit's throughput, not latency, is what is measured.
 *
 * Build (the same runtime the compiler links):
 *   cc -O2 -DNDEBUG machine_profile.c ../runtime/frt_rt.c -lm -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

/* ---- the runtime's machine query (runtime/frt_rt.c) ------------------ */
typedef struct frt_machine {
  int32_t cores_physical, threads_logical, smt, numa_nodes;
  int64_t l1d_bytes, l2_bytes, l3_bytes, page_bytes;
  int64_t mem_total_bytes, mem_available_bytes;
} frt_machine_t;
extern int32_t frt_query_machine(frt_machine_t* out);
extern uint64_t frt_pal_time_ns(void);

static double now_s(void) { return (double)frt_pal_time_ns() * 1e-9; }

/* best-of-R timing of one kernel call */
static double best_of(int R, void (*run)(void*), void* ctx) {
  double best = 1e30;
  for (int r = 0; r < R; ++r) {
    double t0 = now_s();
    run(ctx);
    double dt = now_s() - t0;
    if (dt < best) best = dt;
  }
  return best;
}

/* ---- STREAM-style triad: a[i] = b[i] + s*c[i] -------------------------- */
typedef struct {
  double *a, *b, *c;
  int64_t n;
  double s;
} TriadCtx;

static void triad_run(void* vctx) {
  TriadCtx* t = (TriadCtx*)vctx;
  const int64_t n = t->n;
  const double s = t->s;
  double* a = t->a;
  const double* b = t->b;
  const double* c = t->c;
  for (int64_t i = 0; i < n; ++i) a[i] = b[i] + s * c[i];
}

/* ---- FMA throughput: 16 independent a[j] = fma(a[j], x, y) chains ------ */
typedef struct { double a[16], x, y; int64_t reps; } FmaCtx;
#ifdef FP_CONTRACT_OFF
#pragma STDC FP_CONTRACT OFF
#endif
static void fma_run(void* vctx) {
  FmaCtx* f = (FmaCtx*)vctx;
  const int64_t reps = f->reps;
  double a0 = f->a[0], a1 = f->a[1], a2 = f->a[2], a3 = f->a[3];
  double a4 = f->a[4], a5 = f->a[5], a6 = f->a[6], a7 = f->a[7];
  double a8 = f->a[8], a9 = f->a[9], a10 = f->a[10], a11 = f->a[11];
  double a12 = f->a[12], a13 = f->a[13], a14 = f->a[14], a15 = f->a[15];
  const double x = f->x, y = f->y;
  for (int64_t r = 0; r < reps; ++r) {
    a0 += x * a0 + y; a1 += x * a1 + y; a2 += x * a2 + y; a3 += x * a3 + y;
    a4 += x * a4 + y; a5 += x * a5 + y; a6 += x * a6 + y; a7 += x * a7 + y;
    a8 += x * a8 + y; a9 += x * a9 + y; a10 += x * a10 + y; a11 += x * a11 + y;
    a12 += x * a12 + y; a13 += x * a13 + y; a14 += x * a14 + y; a15 += x * a15 + y;
  }
  f->a[0] = a0; f->a[1] = a1; f->a[2] = a2; f->a[3] = a3;
  f->a[4] = a4; f->a[5] = a5; f->a[6] = a6; f->a[7] = a7;
  f->a[8] = a8; f->a[9] = a9; f->a[10] = a10; f->a[11] = a11;
  f->a[12] = a12; f->a[13] = a13; f->a[14] = a14; f->a[15] = a15;
}

int main(int argc, char** argv) {
  const char* outPath = argc > 1 ? argv[1] : "machine_profile.machprof";
  frt_machine_t m;
  memset(&m, 0, sizeof m);
  frt_query_machine(&m);

  /* ---- bandwidth: triad over 4x L3 (cap 256 MiB, floor 32 MiB) -------- */
  int64_t perArray = m.l3_bytes > 0 ? 4 * m.l3_bytes : (256ll << 20);
  if (perArray > (256ll << 20)) perArray = 256ll << 20;
  if (perArray < (32ll << 20)) perArray = 32ll << 20;
  int64_t n = perArray / (int64_t)sizeof(double);
  TriadCtx tc;
  tc.a = (double*)malloc((size_t)n * sizeof(double));
  tc.b = (double*)malloc((size_t)n * sizeof(double));
  tc.c = (double*)malloc((size_t)n * sizeof(double));
  tc.n = n;
  tc.s = 3.0 / 4.0;   /* dyadic value: exact arithmetic, no NaN paths */
  double bw = 0;
  if (tc.a && tc.b && tc.c) {
    for (int64_t i = 0; i < n; ++i) {
      tc.b[i] = 0.25 * (double)(i & 63);
      tc.c[i] = 0.5 * (double)(i & 31);
      tc.a[i] = 0.0;
    }
    double dt = best_of(5, triad_run, &tc);
    /* STREAM counting: triad moves 3 arrays (2 reads + 1 write) */
    double bytes = (double)n * (double)sizeof(double) * 3.0;
    if (dt > 0) bw = bytes / dt;
  }
  free(tc.a); free(tc.b); free(tc.c);

  /* ---- FMA throughput: 16 chains x 2 flops (a += x*a + y is one fma,
   * 2 flops each, 16 per unrolled step) ---------------------------------- */
  FmaCtx fc;
  memset(&fc, 0, sizeof fc);
  fc.x = 1.0 + 1e-9;
  fc.y = 1e-12;
  for (int j = 0; j < 16; ++j) fc.a[j] = 1.0 + 0.25 * (double)j;
  fc.reps = 20000000;
  double fmaPeak = 0;
  {
    double dt = best_of(3, fma_run, &fc);
    /* 16 fma per iteration, 2 flops per fma */
    double flops = (double)fc.reps * 16.0 * 2.0;
    if (dt > 0) fmaPeak = flops / dt;
  }

  /* ---- write the profile ------------------------------------------------ */
  FILE* f = fopen(outPath, "w");
  if (!f) {
    fprintf(stderr, "machine_profile: cannot write %s\n", outPath);
    return 1;
  }
  fprintf(f, "MACHINE-PROFILE 1\n");
  fprintf(f, "BW_TRIAD_BYTES_PER_S %.2f\n", bw);
  fprintf(f, "FMA_PEAK_FLOP_S %.2f\n", fmaPeak);
  fprintf(f, "CORES_PHYSICAL %d\n", m.cores_physical);
  fprintf(f, "THREADS_LOGICAL %d\n", m.threads_logical);
  fprintf(f, "SMT %d\n", m.smt);
  fprintf(f, "NUMA_NODES %d\n", m.numa_nodes);
  fprintf(f, "L1D_BYTES %lld\n", (long long)m.l1d_bytes);
  fprintf(f, "L2_BYTES %lld\n", (long long)m.l2_bytes);
  fprintf(f, "L3_BYTES %lld\n", (long long)m.l3_bytes);
  fprintf(f, "PAGE_BYTES %lld\n", (long long)m.page_bytes);
  fprintf(f, "MEM_TOTAL_BYTES %lld\n", (long long)m.mem_total_bytes);
  fprintf(f, "MEM_AVAILABLE_BYTES %lld\n", (long long)m.mem_available_bytes);
  fclose(f);

  printf("machine profile written to %s\n", outPath);
  printf("  triad bandwidth : %.2f GB/s (working set %.1f MiB x3)\n",
         bw / 1e9, (double)perArray / 1048576.0);
  printf("  fma peak        : %.2f GFLOP/s (16 independent chains)\n",
         fmaPeak / 1e9);
  printf("  topology        : %d core(s) / %d thread(s), smt=%d, numa=%d\n",
         m.cores_physical, m.threads_logical, m.smt, m.numa_nodes);
  printf("  caches          : L1D %lld KiB, L2 %lld KiB, L3 %lld KiB\n",
         (long long)m.l1d_bytes >> 10, (long long)m.l2_bytes >> 10,
         (long long)m.l3_bytes >> 10);
  printf("  memory          : total %.2f GiB, available %.2f GiB\n",
         (double)m.mem_total_bytes / 1073741824.0,
         (double)m.mem_available_bytes / 1073741824.0);
  return 0;
}
