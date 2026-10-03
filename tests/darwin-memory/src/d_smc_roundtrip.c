/*
 * d_smc_roundtrip.c - fact group D (faults and signals): cost of a fault-and-resume
 * round trip, which is what detecting self-modifying code through write protection
 * of guest code pages costs.  One round trip is
 *     mprotect(page, PROT_READ)            (the page now holds translated code)
 *     store into the page -> SIGBUS        (guest code modifies itself)
 *     handler: mprotect(page, PROT_RW)     (invalidate translations, allow the write)
 *     sigreturn, the store runs again
 *
 *   M01 same page ping-pong, thread stack / sigaltstack / mach_vm_protect
 *   M02 breakdown: bare signal round trip (no protection change), mprotect alone
 *   M03 different pages: sequential, random; growth of the VM map entry count
 *   M04 other threads busy on other cores
 *   M05 4 KiB guest pages inside a 16 KiB host page
 *   M06 mprotect cost versus number of VM entries
 *   M07 no-fault baseline: store to a writable page
 */
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define PG (16 * KiB)

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _cd_store64\n"
    "_cd_store64:\n"
    "  str x1, [x0]\n"
    "  ret\n");
extern void cd_store64(uint64_t addr, uint64_t v);

static inline uint64_t tick(void) { return cd_ns(); }

enum { H_MPROTECT = 1, H_MACHPROT = 2, H_SKIP = 3, H_NONE = 4 };
static volatile int g_hmode = H_MPROTECT;
static volatile uint64_t g_faults;
static volatile uint64_t g_t_in;

static void h(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig;
  g_faults++;
  uintptr_t a = (uintptr_t)si->si_addr & ~(uintptr_t)(PG - 1);
  switch (g_hmode) {
  case H_MPROTECT: mprotect((void *)a, PG, PROT_READ | PROT_WRITE); break;
  case H_MACHPROT: mach_vm_protect(mach_task_self(), (mach_vm_address_t)a, PG, FALSE, VM_PROT_READ | VM_PROT_WRITE); break;
  case H_SKIP: CD_PC(uc) += 4; break;
  default: break;
  }
}

static void install(int use_alt) {
  if (use_alt) {
    size_t sz = 128 * KiB;
    stack_t ss = {.ss_sp = malloc(sz), .ss_size = sz, .ss_flags = 0};
    sigaltstack(&ss, NULL);
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = h;
  sa.sa_flags = SA_SIGINFO | (use_alt ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGSEGV, &sa, NULL);
}

static void report(const char *id, const char *what, uint64_t *v, size_t n) {
  cd_stats s = cd_stats_of(v, n);
  INFO(id, "%s (ns): " CD_STATS_FMT, what, CD_STATS_ARGS(s, 1.0));
}

#define N_ITER 100000

/* One protect + faulting store + handler cycle on the same page. */
static int child_pingpong(void *arg) {
  int variant = (int)(intptr_t)arg; /* 0 thread stack mprotect, 1 altstack mprotect, 2 altstack mach_vm_protect */
  install(variant >= 1);
  g_hmode = variant == 2 ? H_MACHPROT : H_MPROTECT;
  uint8_t *p = mmap(NULL, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, 16 * PG);
  uint64_t *tp = malloc(N_ITER * 8), *tf = malloc(N_ITER * 8), *tt = malloc(N_ITER * 8);
  g_faults = 0;
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    mprotect(p, PG, PROT_READ);
    uint64_t t1 = tick();
    *(volatile uint64_t *)p = (uint64_t)i; /* faults, handler unprotects, store retried */
    uint64_t t2 = tick();
    tp[i] = t1 - t0;
    tf[i] = t2 - t1;
    tt[i] = t2 - t0;
  }
  const char *vn = variant == 0 ? "handler on the thread stack, mprotect" : (variant == 1 ? "handler on a sigaltstack, mprotect" : "handler on a sigaltstack, mach_vm_protect");
  char w[160];
  snprintf(w, sizeof w, "[%s] faults taken %llu of %d; mprotect(page, R) alone", vn, (unsigned long long)g_faults, N_ITER);
  report("M01.pingpong", w, tp, N_ITER);
  snprintf(w, sizeof w, "[%s] faulting store until it completes (signal + handler + unprotect + retry)", vn);
  report("M01.pingpong", w, tf, N_ITER);
  snprintf(w, sizeof w, "[%s] whole cycle protect + faulting store", vn);
  report("M01.pingpong", w, tt, N_ITER);
  return 0;
}

/* Pure signal round trip: the handler only skips the instruction. */
static int child_bare(void *arg) {
  (void)arg;
  install(1);
  g_hmode = H_SKIP;
  uint8_t *p = mmap(NULL, PG, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
  uint64_t *t = malloc(N_ITER * 8);
  g_faults = 0;
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    cd_store64((uint64_t)(uintptr_t)p, 1); /* faults on the read-only page, handler skips the store */
    t[i] = tick() - t0;
  }
  char w[120];
  snprintf(w, sizeof w, "bare fault round trip, handler only advances the pc (%llu faults)", (unsigned long long)g_faults);
  report("M02.bare", w, t, N_ITER);
  /* mprotect alone on one page, RW -> R -> RW */
  uint8_t *q = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(q, 1, PG);
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    mprotect(q, PG, PROT_READ);
    mprotect(q, PG, PROT_READ | PROT_WRITE);
    t[i] = tick() - t0;
  }
  report("M02.bare", "mprotect(R) + mprotect(RW) on one page (two calls)", t, N_ITER);
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    mach_vm_protect(mach_task_self(), (mach_vm_address_t)q, PG, FALSE, VM_PROT_READ);
    mach_vm_protect(mach_task_self(), (mach_vm_address_t)q, PG, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    t[i] = tick() - t0;
  }
  report("M02.bare", "mach_vm_protect(R) + mach_vm_protect(RW) on one page (two calls)", t, N_ITER);
  /* no-fault baseline: store to a writable page */
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    cd_store64((uint64_t)(uintptr_t)q, (uint64_t)i);
    t[i] = tick() - t0;
  }
  report("M07.baseline", "store to a writable page, timed with the same two timestamps (includes ~2 timer reads)", t, N_ITER);
  return 0;
}

/* Different pages. */
static int child_pages(void *arg) {
  int order = (int)(intptr_t)arg; /* 0 sequential, 1 random */
  install(1);
  g_hmode = H_MPROTECT;
  const size_t npages = 4096; /* 64 MiB */
  uint8_t *p = mmap(NULL, npages * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, npages * PG);
  size_t *idx = malloc(npages * sizeof(size_t));
  for (size_t i = 0; i < npages; i++) idx[i] = i;
  if (order == 1) {
    srand(12345);
    for (size_t i = npages - 1; i > 0; i--) { size_t j = (size_t)rand() % (i + 1); size_t t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
  }
  uint64_t *t = malloc(npages * 8);
  /* protect the whole range at once: one VM entry */
  uint64_t a0 = tick();
  mprotect(p, npages * PG, PROT_READ);
  uint64_t a1 = tick();
  INFO("M03.pages", "[%s] mprotect(64 MiB = 4096 pages, R) in one call: %.1f us", order ? "random order" : "sequential", (a1 - a0) / 1e3);
  g_faults = 0;
  for (size_t i = 0; i < npages; i++) {
    uint64_t t0 = tick();
    *(volatile uint64_t *)(p + idx[i] * PG) = i;
    t[i] = tick() - t0;
  }
  /* mean over four quarters of the run: does the cost grow as the VM map entry count grows? */
  double q[4];
  for (int k = 0; k < 4; k++) {
    uint64_t s = 0;
    for (size_t i = k * npages / 4; i < (k + 1) * npages / 4; i++) s += t[i];
    q[k] = (double)s / (npages / 4);
  }
  INFO("M03.pages", "[%s] %llu faults, first fault on each of 4096 pages of one big read-only entry: mean per quarter of the run = %.0f %.0f %.0f %.0f ns",
       order ? "random order" : "sequential", (unsigned long long)g_faults, q[0], q[1], q[2], q[3]);
  uint64_t *cp = malloc(npages * 8);
  memcpy(cp, t, npages * 8);
  report("M03.pages", order ? "[random order] per-fault time" : "[sequential] per-fault time", cp, npages);
  /* Count VM entries now */
  size_t entries = 0;
  mach_vm_address_t a = (mach_vm_address_t)p;
  while (a < (mach_vm_address_t)p + npages * PG) {
    dm_region r = dm_region_at_or_after(a);
    if (!r.valid || r.start >= (mach_vm_address_t)p + npages * PG) break;
    entries++;
    a = r.start + r.size;
  }
  INFO("M03.pages", "[%s] after all faults the 64 MiB range is %zu mach_vm_region entries", order ? "random order" : "sequential", entries);
  return 0;
}

/* VM map fragmentation: alternate R / RW on every other page, then measure mprotect and faults. */
static int child_fragment(void *arg) {
  (void)arg;
  install(1);
  g_hmode = H_MPROTECT;
  const size_t npages = 8192; /* 128 MiB */
  uint8_t *p = mmap(NULL, npages * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, npages * PG);
  uint64_t t0 = tick();
  for (size_t i = 0; i < npages; i += 2) mprotect(p + i * PG, PG, PROT_READ);
  uint64_t t1 = tick();
  INFO("M06.fragment", "protecting every other page of 8192 pages (4096 mprotect calls, each splitting an entry): %.2f us per call", (t1 - t0) / 1e3 / (npages / 2));
  size_t entries = 0;
  mach_vm_address_t a = (mach_vm_address_t)p;
  while (a < (mach_vm_address_t)p + npages * PG) {
    dm_region r = dm_region_at_or_after(a);
    if (!r.valid || r.start >= (mach_vm_address_t)p + npages * PG) break;
    entries++;
    a = r.start + r.size;
  }
  INFO("M06.fragment", "range now has %zu VM entries", entries);
  const int N = 50000;
  uint64_t *t = malloc(N * 8), *tp = malloc(N * 8);
  g_faults = 0;
  for (int i = 0; i < N; i++) {
    size_t pg = ((size_t)i * 2) % npages;
    uint64_t s0 = tick();
    mprotect(p + pg * PG, PG, PROT_READ);
    uint64_t s1 = tick();
    *(volatile uint64_t *)(p + pg * PG) = (uint64_t)i;
    uint64_t s2 = tick();
    tp[i] = s1 - s0;
    t[i] = s2 - s1;
  }
  report("M06.fragment", "mprotect(R) on a page inside a 4096-entry fragmented range", tp, N);
  report("M06.fragment", "faulting store (handler unprotects) inside the fragmented range", t, N);
  return 0;
}

/* Busy threads on other cores. */
static _Atomic int g_stop;
static void *busy(void *a) { (void)a; volatile uint64_t x = 0; while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) x++; return NULL; }

static int child_busy(void *arg) {
  int nthreads = (int)(intptr_t)arg;
  install(1);
  g_hmode = H_MPROTECT;
  pthread_t th[16];
  for (int i = 0; i < nthreads; i++) pthread_create(&th[i], NULL, busy, NULL);
  usleep(5000);
  uint8_t *p = mmap(NULL, 16 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, 16 * PG);
  uint64_t *tt = malloc(N_ITER * 8);
  for (int i = 0; i < N_ITER; i++) {
    uint64_t t0 = tick();
    mprotect(p, PG, PROT_READ);
    *(volatile uint64_t *)p = (uint64_t)i;
    tt[i] = tick() - t0;
  }
  char w[120];
  snprintf(w, sizeof w, "%d other threads spinning on other cores: protect + faulting store cycle", nthreads);
  report("M04.busy", w, tt, N_ITER);
  atomic_store(&g_stop, 1);
  for (int i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
  return 0;
}

/* 4 KiB guest pages inside a 16 KiB host page. */
static int child_subpage(void *arg) {
  (void)arg;
  install(1);
  g_hmode = H_MPROTECT;
  uint8_t *p = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, PG);
  mprotect(p, PG, PROT_READ);
  g_faults = 0;
  for (int sub = 0; sub < 4; sub++) *(volatile uint64_t *)(p + sub * 4096 + 64) = 1; /* one write in each 4 KiB guest page */
  INFO("M05.subpage", "the 16 KiB host page protects four 4 KiB guest pages: 4 writes to the 4 guest pages = %llu fault(s)", (unsigned long long)g_faults);
  mprotect(p, PG, PROT_READ);
  g_faults = 0;
  for (int i = 0; i < 1000; i++) *(volatile uint64_t *)(p + (i % 4) * 4096 + 64) = i;
  INFO("M05.subpage", "after the first fault the host page stays writable: 1000 further writes = %llu extra fault(s)", (unsigned long long)g_faults);
  return 0;
}

int main(int argc, char **argv) {
  dm_arm_watchdog(220);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
  cd_print_header(argv[0]);
  struct { const char *tag; cd_child_fn fn; intptr_t arg; unsigned ms; } tests[] = {
      {"M01", child_pingpong, 0, 60000}, {"M01", child_pingpong, 1, 60000}, {"M01", child_pingpong, 2, 60000},
      {"M02", child_bare, 0, 60000},     {"M03", child_pages, 0, 60000},    {"M03", child_pages, 1, 60000},
      {"M06", child_fragment, 0, 90000}, {"M04", child_busy, 4, 60000},     {"M04", child_busy, 8, 60000},
      {"M05", child_subpage, 0, 20000}};
  for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
    if (only && !strstr(only, tests[i].tag)) continue;
    int to = 0;
    char sb[96];
    int st = cd_run_child(tests[i].fn, (void *)tests[i].arg, tests[i].ms, &to);
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st)) INFO(tests[i].tag, "child result: %s", cd_status_str(st, to, sb, sizeof sb));
  }
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
