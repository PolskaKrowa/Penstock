// C baseline: AoS vs SoA field sweep (same shape as bench_soa.fsh)
// build: clang -O3 -march=native bench_soa_c.c -o bench_soa_c -lm -lpthread
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>

typedef struct { double x,y,z,w, vx,vy,vz,vw; } P8;

static double now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec*1e9 + ts.tv_nsec;
}

int main(void) {
  size_t n = 4000000;
  int reps = 10;

  // ---- AoS
  P8* a = aligned_alloc(64, n*sizeof(P8));
  for (size_t i = 0; i < n; i++) { a[i].x = 1.5; a[i].vy = 2.5; }
  double t0 = now_ns(), sa = 0;
  for (int r = 0; r < reps; r++)
    for (size_t i = 0; i < n; i++) sa += a[i].x + a[i].vy;
  double t1 = now_ns();

  // ---- SoA
  double *px = aligned_alloc(64, n*sizeof(double));
  double *pvy = aligned_alloc(64, n*sizeof(double));
  for (size_t i = 0; i < n; i++) { px[i] = 1.5; pvy[i] = 2.5; }
  double t2 = now_ns(), sb = 0;
  for (int r = 0; r < reps; r++)
    for (size_t i = 0; i < n; i++) sb += px[i] + pvy[i];
  double t3 = now_ns();

  printf("C AoS sum=%.1f  %6.1f ms | C SoA sum=%.1f  %6.1f ms | speedup %.2f\n",
         sa, (t1-t0)/1e6, sb, (t3-t2)/1e6, (t1-t0)/(t3-t2));
  return 0;
}
