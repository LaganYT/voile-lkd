/*
 * c_jit_threads.c - fact group C (JIT), multi-threaded facts.
 *
 *   T01 default JIT write-protection state of new threads
 *   T02 code written by one thread while other threads execute code in the same page
 *   T03 patching a branch in place while another thread loops through it
 *   T04 patching a literal pool slot (ldr x16, lit; blr x16) while another thread uses it
 *   T05 does a thread that toggles and patches slow down the executing threads
 *   T06 torn instruction fetch check (alternating instruction word)
 *
 * Time stamps come from cntvct_el0 (1 GHz on this machine, checked at start).
 */
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)

typedef uint64_t (*fn0_t)(void);

static inline uint64_t tick(void) { return cd_ns(); }

static void spin_pause(void) { __asm__ volatile("yield"); }

static void wait_until(_Atomic int *flag, int val, uint64_t timeout_ns) {
  uint64_t t0 = tick();
  while (atomic_load_explicit(flag, memory_order_acquire) != val) {
    spin_pause();
    if (tick() - t0 > timeout_ns) return;
  }
}

/* ------------------------------------------------------------------------- */
/* T01: default state of new threads                                          */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint32_t *code;
  int parent_write;
  int store_sig, call_sig;
  cd_probe_result sr, cr;
  uint64_t call_out;
} t01_arg;

static void *t01_thread(void *a) {
  t01_arg *t = a;
  /* A stub must already exist (installed by the parent). */
  t->store_sig = cd_probe_store32((volatile uint32_t *)&t->code[64], 0x12345678u, &t->sr);
  t->call_sig = cd_probe_call(&t->code[0], &t->call_out, &t->cr);
  return NULL;
}

static void t01_default_state(void) {
  int err = 0;
  uint32_t *c = cd_jit_map(64 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("T01.setup", "MAP_JIT failed: %s", errno_name(err)); return; }
  cd_install_probe_handlers();
  cd_jit_write();
  cd_emit_ret_const(&c[0], 17);
  cd_jit_exec();
  sys_icache_invalidate(c, 8);
  for (int pw = 0; pw < 2; pw++) {
    if (pw) cd_jit_write(); else cd_jit_exec();
    t01_arg t;
    memset(&t, 0, sizeof t);
    t.code = c;
    t.parent_write = pw;
    pthread_t th;
    pthread_create(&th, NULL, t01_thread, &t);
    pthread_join(th, NULL);
    cd_jit_exec();
    char b1[160], b2[160];
    INFO("T01.new-thread", "parent in %s when calling pthread_create: child store = %s ; child call = %s (returned %llu)",
         pw ? "write mode" : "exec mode", cd_probe_str(t.store_sig, &t.sr, b1, sizeof b1),
         cd_probe_str(t.call_sig, &t.cr, b2, sizeof b2), (unsigned long long)t.call_out);
  }
  /* The parent's own state is unchanged by another thread's toggling: toggle in a child, then probe in the parent. */
  cd_jit_exec();
  int sig = cd_probe_store32((volatile uint32_t *)&c[64], 1, NULL);
  INFO("T01.isolation", "parent (exec mode) store after children ran: %s", sig ? sig_name(sig) : "ok (unexpected)");
  munmap(c, 64 * KiB);
}

/* ------------------------------------------------------------------------- */
/* T02: writer thread vs executing threads in the same page                   */
/* ------------------------------------------------------------------------- */
#define T02_K 3
#define T02_STRIDE 128 /* bytes between executor stubs: stub in first 8 bytes, churn in the rest of the line */

typedef struct {
  uint32_t *code;
  _Atomic uint32_t ver[T02_K * 16]; /* padded: index i*16 */
  _Atomic int stop;
  _Atomic int started;
  int mode_isb;
  int mode_inval;
  int writes;
  /* results per executor */
  uint64_t calls[T02_K];
  uint64_t stale[T02_K];
  uint64_t faults[T02_K];
  uint64_t bad[T02_K];
} t02_shared;

typedef struct {
  t02_shared *sh;
  int idx;
} t02_exec_arg;

static void *t02_executor(void *a) {
  t02_exec_arg *ea = a;
  t02_shared *sh = ea->sh;
  int i = ea->idx;
  uint32_t *stub = sh->code + (i * T02_STRIDE) / 4;
  atomic_fetch_add(&sh->started, 1);
  uint64_t calls = 0, stale = 0, faults = 0, bad = 0;
  while (!atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
    uint32_t v = atomic_load_explicit(&sh->ver[i * 16], memory_order_acquire);
    if (sh->mode_isb) __asm__ volatile("isb" ::: "memory");
    uint64_t out = 0;
    int sig = cd_probe_call(stub, &out, NULL);
    calls++;
    if (sig) faults++;
    else if (out < v) stale++;
    else if (out > 0xffff) bad++;
  }
  sh->calls[i] = calls;
  sh->stale[i] = stale;
  sh->faults[i] = faults;
  sh->bad[i] = bad;
  return NULL;
}

static void *t02_writer(void *a) {
  t02_shared *sh = a;
  uint32_t cnt[T02_K] = {0};
  uint32_t junk = 0;
  while (atomic_load(&sh->started) < T02_K) spin_pause();
  for (int it = 0; it < sh->writes; it++) {
    int i = it % T02_K;
    uint32_t v = ++cnt[i];
    uint32_t *stub = sh->code + (i * T02_STRIDE) / 4;
    cd_jit_write();
    stub[0] = A64_MOVZ_X(0, v);
    stub[1] = A64_RET;
    /* churn: rewrite the rest of the same 128-byte line and of the neighbours with new "code" */
    for (int w = 2; w < T02_STRIDE / 4; w++) stub[w] = (junk++ & 1) ? A64_NOP : A64_BRK(0);
    cd_jit_exec();
    if (sh->mode_inval) sys_icache_invalidate(stub, T02_STRIDE);
    atomic_store_explicit(&sh->ver[i * 16], v, memory_order_release);
  }
  return NULL;
}

static void t02_run(int isb, int inval) {
  int err = 0;
  uint32_t *c = cd_jit_map(64 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("T02.setup", "MAP_JIT failed"); return; }
  t02_shared *sh = calloc(1, sizeof *sh);
  sh->code = c;
  sh->mode_isb = isb;
  sh->mode_inval = inval;
  sh->writes = T02_K * 20000;
  /* initial stubs */
  cd_jit_write();
  for (int i = 0; i < T02_K; i++) cd_emit_ret_const(c + (i * T02_STRIDE) / 4, 0);
  cd_jit_exec();
  sys_icache_invalidate(c, 4096);
  pthread_t ex[T02_K], wr;
  t02_exec_arg ea[T02_K];
  for (int i = 0; i < T02_K; i++) {
    ea[i].sh = sh;
    ea[i].idx = i;
    pthread_create(&ex[i], NULL, t02_executor, &ea[i]);
  }
  uint64_t t0 = now_ns();
  pthread_create(&wr, NULL, t02_writer, sh);
  pthread_join(wr, NULL);
  uint64_t t1 = now_ns();
  usleep(2000);
  atomic_store(&sh->stop, 1);
  for (int i = 0; i < T02_K; i++) pthread_join(ex[i], NULL);
  uint64_t calls = 0, stale = 0, faults = 0, bad = 0;
  for (int i = 0; i < T02_K; i++) { calls += sh->calls[i]; stale += sh->stale[i]; faults += sh->faults[i]; bad += sh->bad[i]; }
  const char *what = "writer: toggle+store+invalidate";
  if (!inval) what = "writer: toggle+store, NO invalidate";
  char buf[256];
  snprintf(buf, sizeof buf, "%s; executors: %s; %d rewrites in %.1f ms (%.2f us each); %llu calls, %llu stale (older version than published), %llu faults, %llu garbage",
           what, isb ? "isb after acquire" : "no isb", sh->writes, (t1 - t0) / 1e6, (t1 - t0) / 1e3 / sh->writes,
           (unsigned long long)calls, (unsigned long long)stale, (unsigned long long)faults, (unsigned long long)bad);
  if (stale || faults || bad) FAIL("T02.cross-thread", "%s", buf);
  else PASS("T02.cross-thread", "%s", buf);
  free(sh);
  munmap(c, 64 * KiB);
}

/* ------------------------------------------------------------------------- */
/* T03: patch a branch while another thread loops through it                  */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint32_t *code;
  _Atomic uint64_t progress;
  _Atomic uint32_t abort_flag;
  _Atomic int go;
  _Atomic int done;
  _Atomic uint64_t t_exit;
  _Atomic uint64_t result;
} t03_shared;

/* loop layout (words):
 *  0: add x2, x2, #1
 *  1: str x2, [x0]            progress
 *  2: ldr w3, [x1]            abort flag
 *  3: cbnz w3, +16 -> word 7
 *  4: b -16 -> word 0         <-- patched to nop
 *  5: mov x0, x2
 *  6: ret
 *  7: movz x0, #0
 *  8: ret
 */
static void t03_emit(uint32_t *c, int with_branch) {
  c[0] = A64_ADD_IMM_X(2, 2, 1);
  c[1] = A64_STR_X(2, 0);
  c[2] = A64_LDR_W(3, 1);
  c[3] = A64_CBNZ_X(3, 16) & ~(1u << 31); /* cbnz w3 */
  c[4] = with_branch ? A64_B(-16) : A64_NOP;
  c[5] = A64_MOV_X(0, 2);
  c[6] = A64_RET;
  c[7] = A64_MOVZ_X(0, 0);
  c[8] = A64_RET;
}

static void *t03_executor(void *a) {
  t03_shared *sh = a;
  for (;;) {
    wait_until(&sh->go, 1, 10000000000ULL);
    if (atomic_load(&sh->go) != 1) break;
    atomic_store(&sh->go, 2);
    uint64_t r = ((uint64_t(*)(volatile void *, volatile void *, uint64_t))sh->code)(&sh->progress, &sh->abort_flag, 0);
    uint64_t te = tick();
    atomic_store(&sh->result, r);
    atomic_store(&sh->t_exit, te);
    atomic_store_explicit(&sh->done, 1, memory_order_release);
    wait_until(&sh->go, 0, 10000000000ULL);
    if (atomic_load(&sh->abort_flag) == 2) break; /* terminate */
  }
  return NULL;
}

static void t03_run(int inval, int trials, const char *label) {
  int err = 0;
  uint32_t *c = cd_jit_map(16 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("T03.setup", "MAP_JIT failed"); return; }
  t03_shared *sh = calloc(1, sizeof *sh);
  sh->code = c;
  cd_jit_write();
  t03_emit(c, 1);
  cd_jit_exec();
  sys_icache_invalidate(c, 64);
  pthread_t th;
  pthread_create(&th, NULL, t03_executor, sh);
  uint64_t *lat_store = calloc(trials, sizeof(uint64_t));
  uint64_t *lat_inval = calloc(trials, sizeof(uint64_t));
  int nl = 0, stuck = 0;
  for (int k = 0; k < trials; k++) {
    /* restore the loop */
    cd_jit_write();
    c[4] = A64_B(-16);
    cd_jit_exec();
    sys_icache_invalidate(c, 64);
    atomic_store(&sh->abort_flag, 0);
    atomic_store(&sh->done, 0);
    atomic_store(&sh->progress, 0);
    atomic_store(&sh->go, 1);
    wait_until(&sh->go, 2, 1000000000ULL);
    /* wait for the loop to be running */
    uint64_t w0 = tick();
    while (atomic_load(&sh->progress) < 1000 && tick() - w0 < 100000000ULL) spin_pause();
    usleep(20 + (k % 7) * 13);
    /* patch the branch to a nop */
    cd_jit_write();
    c[4] = A64_NOP;
    uint64_t t_store = tick();
    cd_jit_exec();
    if (inval) sys_icache_invalidate(&c[4], 4);
    uint64_t t_inval = tick();
    /* wait for the executor to leave the loop on its own */
    uint64_t w1 = tick();
    while (!atomic_load_explicit(&sh->done, memory_order_acquire) && tick() - w1 < 30000000ULL) spin_pause();
    if (!atomic_load(&sh->done)) {
      stuck++;
      atomic_store(&sh->abort_flag, 1);
      while (!atomic_load_explicit(&sh->done, memory_order_acquire)) spin_pause();
    } else {
      uint64_t te = atomic_load(&sh->t_exit);
      lat_store[nl] = te > t_store ? te - t_store : 0;
      lat_inval[nl] = te > t_inval ? te - t_inval : 0;
      nl++;
    }
    atomic_store(&sh->go, 0);
    usleep(50);
  }
  atomic_store(&sh->abort_flag, 2);
  atomic_store(&sh->go, 1);
  atomic_store(&sh->go, 0);
  pthread_join(th, NULL);
  if (nl) {
    cd_stats s1 = cd_stats_of(lat_store, nl);
    cd_stats s2 = cd_stats_of(lat_inval, nl);
    INFO("T03.patch-latency", "%s: %d trials, %d never left the loop within 30 ms. exit delay after the patching store (ns): " CD_STATS_FMT,
         label, trials, stuck, CD_STATS_ARGS(s1, 1.0));
    INFO("T03.patch-latency", "%s: exit delay after sys_icache_invalidate returned (ns, 0 = already out): " CD_STATS_FMT, label,
         CD_STATS_ARGS(s2, 1.0));
  } else {
    INFO("T03.patch-latency", "%s: %d trials, the executor never left the loop (%d stuck)", label, trials, stuck);
  }
  free(lat_store);
  free(lat_inval);
  free(sh);
  munmap(c, 16 * KiB);
}

/* ------------------------------------------------------------------------- */
/* T04: literal pool patch                                                    */
/* ------------------------------------------------------------------------- */
/* loop: ldr x16, lit ; blr x16 ; (x0 holds the stub id) ; str x0, [x19] ; b loop
 * Encoded by hand:
 *   0: str x30, [sp, #-16]!      0xf81f0ffe
 *   1: str x19, [sp, #-16]!      0xf81f0ff3
 *   2: mov x19, x0               (x0 = address of the id cell)
 *   3: ldr x16, lit              (offset +? )
 *   4: blr x16
 *   5: str x0, [x19]
 *   6: ldr w3, [x19, #8]         stop flag
 *   7: cbnz w3, +8 -> 9
 *   8: b -20 -> word 3
 *   9: ldr x19, [sp], #16        0xf84107f3
 *  10: ldr x30, [sp], #16        0xf84107fe
 *  11: ret
 *  12,13: pad
 *  14..15: literal (8-byte aligned)
 */
typedef struct {
  uint32_t *code;
  _Atomic uint64_t cells[4]; /* [0] = last id seen, [1] = pad, [2] = stop flag (as 32 bit at +8 of cells[0]) */
  _Atomic int go;
  _Atomic int done;
} t04_shared;

static void *t04_executor(void *a) {
  t04_shared *sh = a;
  atomic_store(&sh->go, 1);
  ((void (*)(volatile void *))sh->code)(&sh->cells[0]);
  atomic_store_explicit(&sh->done, 1, memory_order_release);
  return NULL;
}

static void t04_literal(void) {
  int err = 0;
  uint8_t *base = cd_jit_map(16 * KiB, CD_RWX, 0, &err);
  if (!base) { FAIL("T04.setup", "MAP_JIT failed"); return; }
  uint32_t *c = (uint32_t *)base;
  uint32_t *stubA = (uint32_t *)(base + 1024), *stubB = (uint32_t *)(base + 2048);
  cd_jit_write();
  cd_emit_ret_const(stubA, 1);
  cd_emit_ret_const(stubB, 2);
  c[0] = 0xf81f0ffeu;                                  /* str x30, [sp, #-16]! */
  c[1] = 0xf81f0ff3u;                                  /* str x19, [sp, #-16]! */
  c[2] = A64_MOV_X(19, 0);                             /* mov x19, x0 */
  c[3] = A64_LDR_LIT_X(16, (14 - 3) * 4);              /* ldr x16, lit */
  c[4] = A64_BLR(16);
  c[5] = A64_STR_X(0, 19);                             /* str x0, [x19] */
  c[6] = A64_LDR_W(3, 19) | (2u << 10);                /* ldr w3, [x19, #8] (imm12 scaled by 4: 2 -> +8) */
  c[7] = A64_CBNZ_X(3, 8) & ~(1u << 31);               /* cbnz w3, +8 */
  c[8] = A64_B(-20);
  c[9] = 0xf84107f3u;                                  /* ldr x19, [sp], #16 */
  c[10] = 0xf84107feu;                                 /* ldr x30, [sp], #16 */
  c[11] = A64_RET;
  c[12] = A64_NOP;
  c[13] = A64_NOP;
  uint64_t lit = (uint64_t)(uintptr_t)stubA;
  memcpy(&c[14], &lit, 8);
  cd_jit_exec();
  sys_icache_invalidate(base, 4096);
  t04_shared *sh = calloc(1, sizeof *sh);
  sh->code = c;
  pthread_t th;
  pthread_create(&th, NULL, t04_executor, sh);
  while (!atomic_load(&sh->go)) spin_pause();
  usleep(1000);
  enum { N = 3000 };
  uint64_t *lat = calloc(N, sizeof(uint64_t));
  int nl = 0, miss = 0;
  uint64_t cur = 1;
  for (int k = 0; k < N; k++) {
    uint64_t want = (cur == 1) ? 2 : 1;
    uint64_t target = (uint64_t)(uintptr_t)(want == 1 ? stubA : stubB);
    cd_jit_write();
    __atomic_store_n((uint64_t *)&c[14], target, __ATOMIC_RELEASE);
    uint64_t t = tick();
    cd_jit_exec();
    uint64_t w0 = tick();
    while (atomic_load_explicit(&sh->cells[0], memory_order_acquire) != want && tick() - w0 < 5000000ULL) spin_pause();
    if (atomic_load(&sh->cells[0]) == want) { lat[nl++] = tick() - t; } else miss++;
    cur = want;
    usleep(30);
  }
  ((volatile uint32_t *)&sh->cells[1])[0] = 1; /* stop flag lives at +8 from cells[0] */
  pthread_join(th, NULL);
  cd_stats s = cd_stats_of(lat, nl);
  INFO("T04.literal", "retargeting 'ldr x16, lit; blr x16' by one aligned 64-bit store in write mode (no icache maintenance): %d switches, %d not seen within 5 ms; delay until the executor runs the new target (ns): " CD_STATS_FMT,
       N, miss, CD_STATS_ARGS(s, 1.0));
  free(lat);
  free(sh);
  munmap(base, 16 * KiB);
}

/* ------------------------------------------------------------------------- */
/* T05: does a toggling writer slow the executors down?                       */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint32_t *code;      /* tight loop: counts iterations */
  _Atomic int stop;
  _Atomic uint64_t iters;
  int idx;
} t05_exec;

static void *t05_worker(void *a) {
  t05_exec *e = a;
  uint64_t n = 0;
  /* executes a short stub 1 M times per slice and counts */
  while (!atomic_load_explicit(&e->stop, memory_order_relaxed)) {
    for (int i = 0; i < 100000; i++) n += ((fn0_t)e->code)();
    atomic_fetch_add(&e->iters, 100000);
  }
  (void)n;
  return NULL;
}

typedef struct {
  uint32_t *code;
  _Atomic int stop;
  _Atomic uint64_t writes;
  int do_inval;
} t05_writer;


static void *t05_writer_fn(void *a) {
  t05_writer *w = a;
  uint32_t junk = 0;
  while (!atomic_load_explicit(&w->stop, memory_order_relaxed)) {
    cd_jit_write();
    for (int i = 0; i < 16; i++) w->code[i] = (junk++ & 1) ? A64_NOP : A64_BRK(0);
    cd_jit_exec();
    if (w->do_inval) sys_icache_invalidate(w->code, 64);
    atomic_fetch_add(&w->writes, 1);
  }
  return NULL;
}

static void t05_interference(void) {
  int err = 0;
  uint32_t *c = cd_jit_map(64 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("T05.setup", "MAP_JIT failed"); return; }
  const int K = 4;
  t05_exec ex[4];
  pthread_t th[4];
  cd_jit_write();
  for (int i = 0; i < K; i++) cd_emit_ret_const(c + (i * 4096) / 4, 1);
  cd_jit_exec();
  sys_icache_invalidate(c, 64 * KiB);
  for (int mode = 0; mode < 4; mode++) {
    /* modes: 0 writer idle, 1 writer in another 16 KiB page (no invalidate), 2 same, with invalidate (other page), 3 writer in the executors' page */
    uint32_t *wcode = (mode == 3) ? c + 64 : c + (5 * 1024);
    t05_writer w;
    memset(&w, 0, sizeof w);
    w.code = wcode;
    w.do_inval = (mode >= 2);
    pthread_t wt;
    for (int i = 0; i < K; i++) {
      memset(&ex[i], 0, sizeof ex[i]);
      ex[i].code = c + (i * 4096) / 4;
      pthread_create(&th[i], NULL, t05_worker, &ex[i]);
    }
    if (mode > 0) pthread_create(&wt, NULL, t05_writer_fn, &w);
    usleep(100000); /* warm-up */
    uint64_t i0 = 0;
    for (int i = 0; i < K; i++) i0 += atomic_load(&ex[i].iters);
    uint64_t w0 = atomic_load(&w.writes);
    uint64_t t0 = now_ns();
    usleep(1500000);
    uint64_t i1 = 0;
    for (int i = 0; i < K; i++) i1 += atomic_load(&ex[i].iters);
    uint64_t w1 = atomic_load(&w.writes);
    uint64_t t1 = now_ns();
    for (int i = 0; i < K; i++) atomic_store(&ex[i].stop, 1);
    atomic_store(&w.stop, 1);
    for (int i = 0; i < K; i++) pthread_join(th[i], NULL);
    if (mode > 0) pthread_join(wt, NULL);
    double secs = (t1 - t0) / 1e9;
    INFO("T05.interference", "%d executor threads calling a JIT stub in a loop, writer %s: %.1f M calls/s total (%.2f ns per call per thread); writer did %.0f toggle+write(16 insns)%s cycles/s",
         K, mode == 0 ? "idle" : (mode == 1 ? "toggling and writing 16 insns in another 16 KiB page (no invalidate)" : (mode == 2 ? "toggling, writing and invalidating in another 16 KiB page" : "toggling, writing and invalidating in the 16 KiB page the executors run in")),
         (i1 - i0) / secs / 1e6, K * secs * 1e9 / (double)(i1 - i0), (w1 - w0) / secs, mode >= 2 ? "+invalidate" : "");
  }
  munmap(c, 64 * KiB);
}

/* ------------------------------------------------------------------------- */
/* T06: torn instruction fetch                                                */
/* ------------------------------------------------------------------------- */
typedef struct {
  uint32_t *code;
  _Atomic int stop;
  uint64_t calls, v1, v2, other, faults;
} t06_shared;

static void *t06_executor(void *a) {
  t06_shared *sh = a;
  uint64_t calls = 0, v1 = 0, v2 = 0, other = 0, faults = 0;
  while (!atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
    uint64_t out = 0;
    int sig = cd_probe_call(sh->code, &out, NULL);
    calls++;
    if (sig) faults++;
    else if (out == 0x5555) v1++;
    else if (out == 0xaaaa) v2++;
    else other++;
  }
  sh->calls = calls; sh->v1 = v1; sh->v2 = v2; sh->other = other; sh->faults = faults;
  return NULL;
}

static void t06_torn(void) {
  int err = 0;
  uint32_t *c = cd_jit_map(16 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("T06.setup", "MAP_JIT failed"); return; }
  cd_jit_write();
  c[0] = A64_MOVZ_X(0, 0x5555);
  c[1] = A64_RET;
  cd_jit_exec();
  sys_icache_invalidate(c, 8);
  t06_shared sh;
  memset(&sh, 0, sizeof sh);
  sh.code = c;
  pthread_t th[2];
  /* two executors on the same word */
  t06_shared sh2;
  memset(&sh2, 0, sizeof sh2);
  sh2.code = c;
  pthread_create(&th[0], NULL, t06_executor, &sh);
  pthread_create(&th[1], NULL, t06_executor, &sh2);
  uint64_t t0 = now_ns();
  uint64_t writes = 0;
  while (now_ns() - t0 < 1500000000ULL) {
    cd_jit_write();
    c[0] = A64_MOVZ_X(0, (writes & 1) ? 0x5555 : 0xaaaa);
    cd_jit_exec();
    sys_icache_invalidate(c, 4);
    writes++;
  }
  atomic_store(&sh.stop, 1);
  atomic_store(&sh2.stop, 1);
  pthread_join(th[0], NULL);
  pthread_join(th[1], NULL);
  uint64_t calls = sh.calls + sh2.calls, v1 = sh.v1 + sh2.v1, v2 = sh.v2 + sh2.v2, other = sh.other + sh2.other,
           faults = sh.faults + sh2.faults;
  char buf[256];
  snprintf(buf, sizeof buf, "writer flipped one instruction word 0x5555 <-> 0xaaaa %llu times with sys_icache_invalidate; 2 executors made %llu calls: %llu saw 0x5555, %llu saw 0xaaaa, %llu saw another value, %llu faulted",
           (unsigned long long)writes, (unsigned long long)calls, (unsigned long long)v1, (unsigned long long)v2,
           (unsigned long long)other, (unsigned long long)faults);
  if (other || faults) FAIL("T06.torn", "%s", buf);
  else PASS("T06.torn", "%s", buf);
  munmap(c, 16 * KiB);
}

int main(int argc, char **argv) {
  dm_arm_watchdog(200);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
  cd_install_probe_handlers();
  cd_print_header(argv[0]);
  INFO("HDR.timer", "cntvct_el0 frequency %llu Hz (1 tick = %.2f ns); mach_absolute_time tick = 41.67 ns",
       (unsigned long long)cd_cntfrq(), 1e9 / (double)cd_cntfrq());
#define RUN(tag, fn) do { if (!only || strstr(only, tag)) fn; } while (0)
  RUN("T01", t01_default_state());
  RUN("T02", t02_run(0, 1));
  RUN("T02", t02_run(1, 1));
  RUN("T02", t02_run(0, 0));
  RUN("T03", t03_run(1, 1000, "writer: store + sys_icache_invalidate"));
  RUN("T03", t03_run(0, 300, "writer: store only, no invalidate"));
  RUN("T04", t04_literal());
  RUN("T05", t05_interference());
  RUN("T06", t06_torn());
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
