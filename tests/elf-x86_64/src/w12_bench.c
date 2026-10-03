// Witness (perf): portable C kernels timed with CLOCK_MONOTONIC. Built three ways: x86-64 ELF under fexdarwin (FEXCore),
// macOS x86_64 under Rosetta 2 (-DWITNESS_HOSTED -arch x86_64) and native macOS arm64 (-DWITNESS_HOSTED -arch arm64).
// Results are INFO only; each kernel prints a checksum so the three builds can be compared for correctness too.
#include "rt.h"
#ifdef WITNESS_HOSTED
#include <time.h>
static u64 now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; }
#else
static u64 now_ns(void) { i64 t[2]; sys2(SYS_clock_gettime, 1, (i64)t); return (u64)t[0] * 1000000000ull + (u64)t[1]; }
#endif

static u64 g_sink;
static u8 g_buf[8 << 20];

__attribute__((noinline)) static u64 k_int(u64 n) {
  u64 a = 1, b = 2;
  for (u64 i = 0; i < n; i++) { a = a * 6364136223846793005ull + 1442695040888963407ull; b ^= a >> 29; b += (a & 0xff) * 3; }
  return a ^ b;
}
__attribute__((noinline)) static u64 k_mem(u64 rounds) {
  u64 s = 0;
  for (u64 r = 0; r < rounds; r++) {
    for (u64 i = 0; i < sizeof g_buf; i += 64) { g_buf[i] = (u8)(g_buf[i] + r + i); }
    for (u64 i = 0; i < sizeof g_buf; i += 64) s += g_buf[i];
  }
  return s;
}
__attribute__((noinline)) static u64 k_float(u64 n) {
  double x = 1.0, y = 0.5;
  for (u64 i = 0; i < n; i++) { x = x * 1.0000001 + y; y = y * 0.9999999 - x * 1e-9; }
  u64 r; __builtin_memcpy(&r, &x, 8); return r;
}
__attribute__((noinline)) static u64 fib(u64 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__attribute__((noinline)) static u64 k_sort(u64 rounds) {
  static u32 a[100000];
  u64 s = 0;
  for (u64 r = 0; r < rounds; r++) {
    u32 v = (u32)r * 2654435761u + 1;
    for (int i = 0; i < 100000; i++) { v ^= v << 13; v ^= v >> 17; v ^= v << 5; a[i] = v; }
    // shell sort, branchy integer code
    for (int gap = 50000; gap > 0; gap /= 2)
      for (int i = gap; i < 100000; i++) { u32 t = a[i]; int j = i; while (j >= gap && a[j - gap] > t) { a[j] = a[j - gap]; j -= gap; } a[j] = t; }
    s += a[0] + a[99999] + a[50000];
  }
  return s;
}

static void bench(const char* name, u64 (*fn)(u64), u64 arg) {
  u64 t0 = now_ns();
  u64 r = fn(arg);
  u64 dt = now_ns() - t0;
  g_sink ^= r;
  puts_(name); puts_(" ms="); put_dec((i64)(dt / 1000000)); puts_(" checksum="); put_hex(r, 16); puts_("\n");
}

static int run(void) {
  puts_("w12_bench\n");
  bench("int_lcg_400M", k_int, 400000000ull);
  bench("mem_8MiB_x200", k_mem, 200);
  bench("float_200M", k_float, 200000000ull);
  bench("calls_fib32", fib, 32);
  bench("sort_100k_x20", k_sort, 20);
  return 0;
}
WITNESS_MAIN(run)
