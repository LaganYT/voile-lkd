/*
 * c_jit_cost.c - fact group C (JIT), costs in nanoseconds.
 *
 *   K01 pthread_jit_write_protect_np toggle cost (pair, single toggle, unchanged state)
 *   K02 pthread_jit_write_with_callback_np cost
 *   K03 sys_icache_invalidate cost versus range size
 *   K04 cost of one complete patch (toggle, store, toggle, invalidate)
 *   K05 mprotect RW<->RX pair (the alternative to MAP_JIT toggling), alone and with busy threads
 *   K06 first-touch cost of fresh MAP_JIT pages
 *   K07 mmap/munmap cost of MAP_JIT versus plain anonymous memory
 *   K08 call cost: host function versus JIT stub, direct and through a pointer
 */
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)

typedef uint64_t (*fn0_t)(void);

static inline uint64_t tick(void) { return cd_ns(); }

/* Runs fn() reps times and returns the sorted per-iteration ns values. */
typedef void (*bench_fn)(long iters, void *ctx);

static cd_stats bench(bench_fn f, void *ctx, long iters, int reps, double *out_min) {
  uint64_t *v = calloc(reps, sizeof(uint64_t));
  for (int r = 0; r < reps; r++) {
    uint64_t t0 = tick();
    f(iters, ctx);
    uint64_t t1 = tick();
    v[r] = (t1 - t0) * 1000 / (uint64_t)iters; /* picoseconds per iteration */
  }
  cd_stats s = cd_stats_of(v, reps);
  if (out_min) *out_min = s.min / 1000.0;
  free(v);
  return s;
}
#define PS_FMT "min=%.2f ns median=%.2f ns max=%.2f ns (n=%zu reps)"
#define PS_ARGS(s) (s).min / 1000.0, (s).p50 / 1000.0, (s).max / 1000.0, (s).n

/* ---------------------------------------------------------------- K01 */
static void b_toggle_pair(long n, void *ctx) {
  (void)ctx;
  for (long i = 0; i < n; i++) {
    pthread_jit_write_protect_np(0);
    pthread_jit_write_protect_np(1);
  }
}
static void b_toggle_same_exec(long n, void *ctx) {
  (void)ctx;
  pthread_jit_write_protect_np(1);
  for (long i = 0; i < n; i++) pthread_jit_write_protect_np(1);
}
static void b_toggle_same_write(long n, void *ctx) {
  (void)ctx;
  pthread_jit_write_protect_np(0);
  for (long i = 0; i < n; i++) pthread_jit_write_protect_np(0);
  pthread_jit_write_protect_np(1);
}
static void k01(void) {
  cd_stats s = bench(b_toggle_pair, NULL, 5000000, 7, NULL);
  INFO("K01.toggle-pair", "pthread_jit_write_protect_np(0) + (1): " PS_FMT "  -> about %.2f ns per toggle", PS_ARGS(s), s.p50 / 2000.0);
  s = bench(b_toggle_same_exec, NULL, 5000000, 7, NULL);
  INFO("K01.toggle-same", "pthread_jit_write_protect_np(1) while already in exec mode: " PS_FMT, PS_ARGS(s));
  s = bench(b_toggle_same_write, NULL, 5000000, 7, NULL);
  INFO("K01.toggle-same", "pthread_jit_write_protect_np(0) while already in write mode: " PS_FMT, PS_ARGS(s));
}

/* ---------------------------------------------------------------- K02 */
static int cb_nop(void *ctx) { (*(volatile long *)ctx)++; return 0; }
PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(cb_nop);
static void b_callback(long n, void *ctx) {
  for (long i = 0; i < n; i++) pthread_jit_write_with_callback_np(cb_nop, ctx);
}
static void k02(void) {
  volatile long counter = 0;
  cd_stats s = bench(b_callback, (void *)&counter, 2000000, 7, NULL);
  INFO("K02.callback", "pthread_jit_write_with_callback_np(trivial callback): " PS_FMT " (counter=%ld)", PS_ARGS(s), (long)counter);
}

/* ---------------------------------------------------------------- K03 */
typedef struct { void *p; size_t n; int kind; } inval_ctx;
static void b_inval(long iters, void *ctx) {
  inval_ctx *c = ctx;
  if (c->kind == 0) for (long i = 0; i < iters; i++) sys_icache_invalidate(c->p, c->n);
  else for (long i = 0; i < iters; i++) __builtin___clear_cache((char *)c->p, (char *)c->p + c->n);
}
static void k03(void) {
  uint8_t *p = cd_jit_map(8 * MiB, CD_RWX, 0, NULL);
  if (!p) { FAIL("K03.setup", "MAP_JIT failed"); return; }
  cd_jit_write();
  memset(p, 0, 8 * MiB);
  cd_jit_exec();
  static const size_t sizes[] = {4, 64, 128, 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    inval_ctx c = {p, sizes[i], 0};
    long iters = sizes[i] <= 4096 ? 200000 : (sizes[i] <= 65536 ? 20000 : (sizes[i] <= 1048576 ? 500 : 50));
    cd_stats s = bench(b_inval, &c, iters, 5, NULL);
    INFO("K03.inval", "sys_icache_invalidate(%8zu bytes): median %.1f ns (%.3f ns per 128-byte line, %.1f GB/s)", sizes[i],
         s.p50 / 1000.0, s.p50 / 1000.0 / ((sizes[i] + 127) / 128), sizes[i] / (s.p50 / 1000.0));
  }
  inval_ctx c = {p, 64, 1};
  cd_stats s = bench(b_inval, &c, 200000, 5, NULL);
  INFO("K03.inval", "__builtin___clear_cache(64 bytes): median %.1f ns", s.p50 / 1000.0);
  /* Unaligned 8-byte range straddling a 128-byte line boundary */
  inval_ctx c2 = {p + 124, 8, 0};
  s = bench(b_inval, &c2, 200000, 5, NULL);
  INFO("K03.inval", "sys_icache_invalidate(8 bytes straddling a 128-byte line boundary): median %.1f ns", s.p50 / 1000.0);
  munmap(p, 8 * MiB);
}

/* ---------------------------------------------------------------- K04 */
typedef struct { uint32_t *c; int inval; } patch_ctx;
static void b_patch(long n, void *ctx) {
  patch_ctx *pc = ctx;
  for (long i = 0; i < n; i++) {
    pthread_jit_write_protect_np(0);
    pc->c[0] = (i & 1) ? A64_NOP : A64_BRK(0);
    pthread_jit_write_protect_np(1);
    if (pc->inval) sys_icache_invalidate(pc->c, 4);
  }
}
static void k04(void) {
  uint32_t *c = cd_jit_map(16 * KiB, CD_RWX, 0, NULL);
  if (!c) { FAIL("K04.setup", "MAP_JIT failed"); return; }
  patch_ctx pc = {c, 0};
  cd_stats s = bench(b_patch, &pc, 2000000, 7, NULL);
  INFO("K04.patch", "toggle(0), one 4-byte store, toggle(1), no invalidate: " PS_FMT, PS_ARGS(s));
  pc.inval = 1;
  s = bench(b_patch, &pc, 1000000, 7, NULL);
  INFO("K04.patch", "toggle(0), one 4-byte store, toggle(1), sys_icache_invalidate(4 bytes): " PS_FMT, PS_ARGS(s));
  munmap(c, 16 * KiB);
}

/* ---------------------------------------------------------------- K05 */
typedef struct { void *p; size_t n; } mp_ctx;
static void b_mprotect_pair(long n, void *ctx) {
  mp_ctx *c = ctx;
  for (long i = 0; i < n; i++) {
    mprotect(c->p, c->n, PROT_READ | PROT_EXEC);
    mprotect(c->p, c->n, PROT_READ | PROT_WRITE);
  }
}
static _Atomic int k05_stop;
static void *k05_spin(void *a) { (void)a; while (!atomic_load_explicit(&k05_stop, memory_order_relaxed)) __asm__ volatile("yield"); return NULL; }
static void k05(void) {
  uint8_t *p = mmap(NULL, 1 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 0, 1 * MiB);
  for (int nth = 0; nth <= 8; nth += 4) {
    pthread_t th[8];
    atomic_store(&k05_stop, 0);
    for (int i = 0; i < nth; i++) pthread_create(&th[i], NULL, k05_spin, NULL);
    usleep(2000);
    static const size_t sizes[] = {16 * KiB, 128 * KiB, 1 * MiB};
    for (size_t k = 0; k < 3; k++) {
      mp_ctx c = {p, sizes[k]};
      cd_stats s = bench(b_mprotect_pair, &c, 20000, 5, NULL);
      INFO("K05.mprotect-pair", "mprotect(RX)+mprotect(RW) of %7zu bytes with %d other busy threads: " PS_FMT " per pair", sizes[k], nth, PS_ARGS(s));
    }
    atomic_store(&k05_stop, 1);
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
  }
  munmap(p, 1 * MiB);
}

/* ---------------------------------------------------------------- K06 */
static void k06(void) {
  const size_t pg = 16 * KiB, total = 256 * MiB;
  uint8_t *p = cd_jit_map(total, CD_RWX, 0, NULL);
  if (!p) { FAIL("K06.setup", "MAP_JIT failed"); return; }
  size_t n = total / pg;
  uint64_t t0 = tick();
  for (size_t i = 0; i < n; i++) {
    uint32_t *c = (uint32_t *)(p + i * pg);
    pthread_jit_write_protect_np(0);
    c[0] = A64_MOVZ_X(0, 3);
    c[1] = A64_RET;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(c, 8);
    (void)((fn0_t)c)();
  }
  uint64_t t1 = tick();
  uint64_t t2 = tick();
  for (size_t i = 0; i < n; i++) {
    uint32_t *c = (uint32_t *)(p + i * pg);
    pthread_jit_write_protect_np(0);
    c[0] = A64_MOVZ_X(0, 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(c, 4);
    (void)((fn0_t)c)();
  }
  uint64_t t3 = tick();
  INFO("K06.first-touch", "fresh 16 KiB MAP_JIT pages (%zu pages): write+invalidate+first call = %.0f ns per page (includes the zero-fill page fault); the same pages again (already resident) = %.0f ns per page",
       n, (double)(t1 - t0) / n, (double)(t3 - t2) / n);
  /* Plain write-only first touch of fresh JIT pages, for comparison with an ordinary anonymous mapping */
  uint8_t *q = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  uint64_t t4 = tick();
  for (size_t i = 0; i < n; i++) q[i * pg] = 1;
  uint64_t t5 = tick();
  INFO("K06.first-touch", "plain anonymous RW pages: first store = %.0f ns per 16 KiB page", (double)(t5 - t4) / n);
  munmap(q, total);
  munmap(p, total);
}

/* ---------------------------------------------------------------- K07 */
static void k07(void) {
  for (int jit = 0; jit < 2; jit++) {
    for (size_t sz = 16 * KiB; sz <= 4 * 1024 * MiB; sz *= 256) {
      uint64_t tm = 0, tu = 0;
      int reps = 2000;
      for (int i = 0; i < reps; i++) {
        uint64_t t0 = tick();
        void *p = jit ? mmap(NULL, sz, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0)
                      : mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        uint64_t t1 = tick();
        if (p == MAP_FAILED) { INFO("K07.mmap", "mmap failed %s", errno_name(errno)); break; }
        munmap(p, sz);
        uint64_t t2 = tick();
        tm += t1 - t0;
        tu += t2 - t1;
      }
      INFO("K07.mmap", "%s %9zu KiB: mmap %.0f ns, munmap (untouched) %.0f ns", jit ? "MAP_JIT RWX " : "plain RW    ", sz / KiB,
           (double)tm / reps, (double)tu / reps);
    }
  }
}

/* ---------------------------------------------------------------- K08 */
static uint64_t __attribute__((noinline)) host_stub(void) { __asm__ volatile(""); return 1; }
typedef struct { fn0_t fn; } call_ctx;
static void b_call(long n, void *ctx) {
  call_ctx *c = ctx;
  uint64_t acc = 0;
  for (long i = 0; i < n; i++) acc += c->fn();
  __asm__ volatile("" ::"r"(acc));
}
static void k08(void) {
  uint32_t *c = cd_jit_map(16 * KiB, CD_RWX, 0, NULL);
  cd_jit_write();
  cd_emit_ret_const(c, 1);
  cd_jit_exec();
  sys_icache_invalidate(c, 8);
  call_ctx hc = {host_stub};
  volatile call_ctx hv = {host_stub};
  (void)hv;
  cd_stats s = bench(b_call, &hc, 100000000, 5, NULL);
  INFO("K08.call", "indirect call of a host function returning a constant: " PS_FMT, PS_ARGS(s));
  call_ctx jc = {(fn0_t)c};
  s = bench(b_call, &jc, 100000000, 5, NULL);
  INFO("K08.call", "indirect call of a JIT stub (movz x0,#1; ret) in a MAP_JIT page: " PS_FMT, PS_ARGS(s));
  munmap(c, 16 * KiB);
}

int main(int argc, char **argv) {
  dm_arm_watchdog(220);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
  cd_install_probe_handlers();
  cd_print_header(argv[0]);
  INFO("HDR.timer", "cntvct_el0 frequency %llu Hz (1 tick = %.2f ns)", (unsigned long long)cd_cntfrq(), 1e9 / (double)cd_cntfrq());
#define RUN(tag, fn) do { if (!only || strstr(only, tag)) fn; } while (0)
  RUN("K01", k01());
  RUN("K02", k02());
  RUN("K03", k03());
  RUN("K04", k04());
  RUN("K05", k05());
  RUN("K06", k06());
  RUN("K07", k07());
  RUN("K08", k08());
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
