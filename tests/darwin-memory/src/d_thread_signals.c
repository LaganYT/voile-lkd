/*
 * d_thread_signals.c - fact group D (faults and signals): interrupting threads.
 *
 *   S01 pthread_kill to a thread running a tight loop in a MAP_JIT region: handler
 *       latency, redirection to another pc (JIT stub in the same region, JIT stub in
 *       another region, host function), latency until the thread is back in C
 *   S02 pthread_kill to a thread blocked in read(), os_sync_wait_on_address, usleep
 *   S03 signal-based suspend/resume cycle (handler parks on os_sync_wait_on_address)
 *   S04 Mach thread_suspend / thread_get_state / thread_set_state / thread_resume
 *   S05 per-thread signal masks, process-directed delivery, coalescing, uc_sigmask
 *   S06 which signals exist (no real-time signals, no sigqueue)
 */
#include "cd_common.h"
#include <dlfcn.h>
#include <mach/thread_act.h>

#define KiB (1ULL << 10)

static inline uint64_t tick(void) { return cd_ns(); }
static void spin_pause(void) { __asm__ volatile("yield"); }

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _cd_host_exit\n"
    "_cd_host_exit:\n"
    "  movz x0, #0x4321\n"
    "  ret\n");
extern void cd_host_exit(void);

static uint32_t *g_code, *g_code_b;

static int setup_jit(void) {
  g_code = cd_jit_map(64 * KiB, CD_RWX, 0, NULL);
  g_code_b = cd_jit_map(64 * KiB, CD_RWX, 0, NULL);
  if (!g_code || !g_code_b) return -1;
  cd_jit_write();
  g_code[0] = A64_LDR_X(1, 0);
  g_code[1] = A64_ADD_IMM_X(1, 1, 1);
  g_code[2] = A64_STR_X(1, 0);
  g_code[3] = A64_B(-12);
  g_code[4] = A64_MOVZ_X(0, 0x1234);
  g_code[5] = A64_RET;
  g_code_b[0] = A64_MOVZ_X(0, 0x5678);
  g_code_b[1] = A64_RET;
  cd_jit_exec();
  sys_icache_invalidate(g_code, 64);
  sys_icache_invalidate(g_code_b, 64);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S01                                                                        */
/* ------------------------------------------------------------------------- */
static volatile uint64_t g_counter __attribute__((aligned(128)));
static _Atomic int g_go, g_done, g_quit;
static volatile uint64_t g_t_handler, g_t_back, g_ret_value, g_pc_seen;
static volatile int g_handler_ran;
static volatile uint64_t g_redirect;
static volatile pthread_t g_handler_thread;

static void kick_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig; (void)si;
  g_t_handler = tick();
  g_pc_seen = CD_PC(uc);
  g_handler_thread = pthread_self();
  g_handler_ran++;
  if (g_redirect) CD_PC(uc) = g_redirect;
}

static void *spin_thread(void *a) {
  (void)a;
  while (!atomic_load_explicit(&g_quit, memory_order_relaxed)) {
    if (atomic_load_explicit(&g_go, memory_order_acquire) != 1) { spin_pause(); continue; }
    atomic_store(&g_go, 2);
    uint64_t r = ((uint64_t(*)(volatile uint64_t *))g_code)(&g_counter);
    g_t_back = tick();
    g_ret_value = r;
    atomic_store_explicit(&g_done, 1, memory_order_release);
    while (atomic_load_explicit(&g_go, memory_order_acquire) != 0 && !atomic_load(&g_quit)) spin_pause();
  }
  return NULL;
}

static int child_redirect(void *a) {
  (void)a;
  if (setup_jit()) { INFO("S01.setup", "MAP_JIT unavailable: %s", errno_name(errno)); return 0; }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = kick_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  pthread_t th;
  pthread_create(&th, NULL, spin_thread, NULL);
  struct { const char *name; uint64_t target; uint64_t expect; } modes[3] = {
      {"JIT stub in the same MAP_JIT region", (uint64_t)(uintptr_t)&g_code[4], 0x1234},
      {"JIT stub in another MAP_JIT region", (uint64_t)(uintptr_t)&g_code_b[0], 0x5678},
      {"host function (movz x0,#0x4321; ret in the executable's text)", (uint64_t)(uintptr_t)&cd_host_exit, 0x4321}};
  enum { N = 4000 };
  for (int m = 0; m < 3; m++) {
    uint64_t *l1 = calloc(N, 8), *l2 = calloc(N, 8), *lk = calloc(N, 8);
    int wrong = 0, pc_out = 0, n = 0;
    g_redirect = modes[m].target;
    for (int i = 0; i < N; i++) {
      g_counter = 0;
      atomic_store(&g_done, 0);
      g_handler_ran = 0;
      atomic_store(&g_go, 1);
      while (atomic_load(&g_go) != 2) spin_pause();
      while (g_counter < 2000) spin_pause();
      usleep(i % 17);
      uint64_t t0 = tick();
      pthread_kill(th, SIGUSR1);
      uint64_t t_after_kill = tick();
      uint64_t w0 = tick();
      while (!atomic_load_explicit(&g_done, memory_order_acquire) && tick() - w0 < 1000000000ULL) spin_pause();
      if (!atomic_load(&g_done)) { wrong++; atomic_store(&g_quit, 1); break; }
      if (g_ret_value != modes[m].expect) wrong++;
      if (g_pc_seen < (uint64_t)(uintptr_t)g_code || g_pc_seen >= (uint64_t)(uintptr_t)g_code + 16) pc_out++;
      if (getenv("CD_DEBUG") && n < 6) {
        char dbg[200];
        int dn = snprintf(dbg, sizeof dbg, "DEBUG S01 trial %d: t0=%llu kill_dur=%llu handler-t0=%lld back-t0=%lld ret=%#llx counter=%llu\n", n, (unsigned long long)t0, (unsigned long long)(t_after_kill - t0), (long long)(g_t_handler - t0), (long long)(g_t_back - t0), (unsigned long long)g_ret_value, (unsigned long long)g_counter);
        (void)!write(1, dbg, (size_t)dn);
      }
      l1[n] = g_t_handler - t0;
      l2[n] = g_t_back - t0;
      lk[n] = t_after_kill - t0;
      n++;
      atomic_store(&g_go, 0);
      usleep(20);
    }
    cd_stats s1 = cd_stats_of(l1, n), s2 = cd_stats_of(l2, n), sk = cd_stats_of(lk, n);
    INFO("S01.redirect", "target thread spinning in a JIT loop, handler sets pc to: %s. %d trials, %d wrong return values, %d with pc outside the loop", modes[m].name, n, wrong, pc_out);
    INFO("S01.redirect", "  pthread_kill call duration (us): " CD_STATS_FMT, CD_STATS_ARGS(sk, 1000.0));
    INFO("S01.redirect", "  pthread_kill -> handler entry in the target (us): " CD_STATS_FMT, CD_STATS_ARGS(s1, 1000.0));
    INFO("S01.redirect", "  pthread_kill -> target back in C after sigreturn at the new pc (us): " CD_STATS_FMT, CD_STATS_ARGS(s2, 1000.0));
    free(l1); free(l2); free(lk);
  }
  g_redirect = 0;
  atomic_store(&g_quit, 1);
  pthread_join(th, NULL);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S02: blocked threads                                                       */
/* ------------------------------------------------------------------------- */
static int g_pipe[2];
static _Atomic uint32_t g_futex_word;
static volatile uint64_t g_t_h2, g_t_back2, g_block_result;
static volatile int g_block_errno, g_block_kind;
static _Atomic int g_blk_ready, g_blk_done;

static void blk_handler(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si; (void)uctx;
  g_t_h2 = tick();
}

static void *blk_thread(void *a) {
  (void)a;
  char c;
  for (;;) {
    int kind = g_block_kind;
    if (kind < 0) break;
    atomic_store(&g_blk_ready, 1);
    long r;
    errno = 0;
    if (kind == 0) r = read(g_pipe[0], &c, 1);
    else if (kind == 1) r = os_sync_wait_on_address((void *)&g_futex_word, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
    else if (kind == 2) { r = usleep(5000000); }
    else { struct timespec ts = {5, 0}; r = nanosleep(&ts, NULL); }
    g_block_errno = errno;
    g_block_result = (uint64_t)r;
    g_t_back2 = tick();
    atomic_store_explicit(&g_blk_done, 1, memory_order_release);
    while (atomic_load(&g_blk_done) && g_block_kind >= 0) spin_pause();
  }
  return NULL;
}

static int child_blocked(void *a) {
  (void)a;
  if (pipe(g_pipe)) return 1;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = blk_handler;
  sa.sa_flags = SA_SIGINFO; /* no SA_RESTART */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  static const char *names[] = {"read() on an empty pipe", "os_sync_wait_on_address", "usleep(5 s)", "nanosleep(5 s)"};
  pthread_t th;
  for (int kind = 0; kind < 4; kind++) {
    g_block_kind = kind;
    atomic_store(&g_blk_done, 0);
    atomic_store(&g_blk_ready, 0);
    pthread_create(&th, NULL, blk_thread, NULL);
    while (!atomic_load(&g_blk_ready)) spin_pause();
    usleep(2000);
    enum { N = 500 };
    uint64_t *l1 = calloc(N, 8), *l2 = calloc(N, 8);
    int n = 0, eintr = 0, other = 0;
    for (int i = 0; i < N; i++) {
      if (i > 0) {
        /* re-arm the blocking call: wake the thread loop by finishing and restarting it */
        atomic_store(&g_blk_ready, 0);
        atomic_store(&g_blk_done, 0);
        /* the thread loops back and blocks again */
        uint64_t w = tick();
        while (!atomic_load(&g_blk_ready) && tick() - w < 200000000ULL) spin_pause();
        usleep(500);
      }
      uint64_t t0 = tick();
      pthread_kill(th, SIGUSR1);
      uint64_t w0 = tick();
      while (!atomic_load_explicit(&g_blk_done, memory_order_acquire) && tick() - w0 < 500000000ULL) spin_pause();
      if (!atomic_load(&g_blk_done)) break;
      l1[n] = g_t_h2 - t0;
      l2[n] = g_t_back2 - t0;
      if (g_block_errno == EINTR) eintr++; else other++;
      n++;
    }
    g_block_kind = -1;
    atomic_store(&g_blk_done, 0);
    /* unblock the thread so it can exit: feed the pipe / word / signal */
    (void)!write(g_pipe[1], "x", 1);
    atomic_store(&g_futex_word, 1);
    os_sync_wake_by_address_all((void *)&g_futex_word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    pthread_kill(th, SIGUSR1);
    pthread_join(th, NULL);
    atomic_store(&g_futex_word, 0);
    char c;
    if (n) {
      cd_stats s1 = cd_stats_of(l1, n), s2 = cd_stats_of(l2, n);
      INFO("S02.blocked", "%-26s: %d signals, %d returned EINTR, %d returned something else. pthread_kill -> handler (us): " CD_STATS_FMT, names[kind], n, eintr, other, CD_STATS_ARGS(s1, 1000.0));
      INFO("S02.blocked", "%-26s: pthread_kill -> blocked call returned in the target (us): " CD_STATS_FMT, names[kind], CD_STATS_ARGS(s2, 1000.0));
    } else {
      INFO("S02.blocked", "%-26s: no measurement (the call never returned)", names[kind]);
    }
    free(l1); free(l2);
    /* drain the pipe fully */
    int fl = fcntl(g_pipe[0], F_GETFL);
    fcntl(g_pipe[0], F_SETFL, fl | O_NONBLOCK);
    while (read(g_pipe[0], &c, 1) == 1) {}
    fcntl(g_pipe[0], F_SETFL, fl);
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S03: signal-based suspend/resume                                           */
/* ------------------------------------------------------------------------- */
static _Atomic uint32_t g_ack, g_resume;
static volatile uint64_t g_t_ack;

static void park_handler(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si; (void)uctx;
  g_t_ack = tick();
  atomic_store_explicit(&g_ack, 1, memory_order_release);
  while (atomic_load_explicit(&g_resume, memory_order_acquire) == 0)
    os_sync_wait_on_address((void *)&g_resume, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
}

static void *counter_thread(void *a) {
  (void)a;
  /* spin in the JIT loop until the counter reaches the quit mark */
  while (!atomic_load(&g_quit)) {
    g_counter = 0;
    ((uint64_t(*)(volatile uint64_t *))g_code)(&g_counter);
  }
  return NULL;
}

static int child_park(void *a) {
  (void)a;
  if (setup_jit()) { INFO("S03.setup", "MAP_JIT unavailable"); return 0; }
  /* The loop never returns by itself: use the redirect to leave it at the end. */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = park_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  struct sigaction sq;
  memset(&sq, 0, sizeof sq);
  sq.sa_sigaction = kick_handler;
  sq.sa_flags = SA_SIGINFO;
  sigemptyset(&sq.sa_mask);
  sigaction(SIGUSR2, &sq, NULL);
  g_redirect = (uint64_t)(uintptr_t)&g_code[4];
  atomic_store(&g_quit, 0);
  pthread_t th;
  pthread_create(&th, NULL, counter_thread, NULL);
  while (g_counter < 1000) spin_pause();
  enum { N = 3000 };
  uint64_t *s = calloc(N, 8), *r = calloc(N, 8), *tot = calloc(N, 8);
  int stopped_ok = 0, n = 0;
  for (int i = 0; i < N; i++) {
    atomic_store(&g_ack, 0);
    atomic_store(&g_resume, 0);
    usleep(i % 13);
    uint64_t t0 = tick();
    pthread_kill(th, SIGUSR1);
    while (!atomic_load_explicit(&g_ack, memory_order_acquire)) spin_pause();
    uint64_t t1 = tick();
    uint64_t c1 = g_counter;
    uint64_t w = tick();
    while (tick() - w < 20000) spin_pause();
    uint64_t c2 = g_counter;
    if (c1 == c2) stopped_ok++;
    uint64_t t2 = tick();
    atomic_store_explicit(&g_resume, 1, memory_order_release);
    os_sync_wake_by_address_all((void *)&g_resume, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    uint64_t w2 = tick();
    uint64_t c3 = g_counter;
    while (g_counter == c3 && tick() - w2 < 100000000ULL) spin_pause();
    uint64_t t3 = tick();
    s[n] = t1 - t0;
    r[n] = t3 - t2;
    tot[n] = t3 - t0 - 20000;
    n++;
  }
  /* leave the loop: SIGUSR2 redirects to the 'movz x0,#0x1234; ret' stub */
  atomic_store(&g_quit, 1);
  pthread_kill(th, SIGUSR2);
  pthread_join(th, NULL);
  cd_stats ss = cd_stats_of(s, n), rr = cd_stats_of(r, n), tt = cd_stats_of(tot, n);
  INFO("S03.suspend", "signal-based stop of a thread spinning in JIT code: %d cycles, thread really stopped (counter frozen for 20 us) in %d of them", n, stopped_ok);
  INFO("S03.suspend", "  pthread_kill -> handler parked (us): " CD_STATS_FMT, CD_STATS_ARGS(ss, 1000.0));
  INFO("S03.suspend", "  wake (store + os_sync_wake_by_address_all) -> thread running again (counter moves) (us): " CD_STATS_FMT, CD_STATS_ARGS(rr, 1000.0));
  INFO("S03.suspend", "  whole cycle without the 20 us freeze check (us): " CD_STATS_FMT, CD_STATS_ARGS(tt, 1000.0));
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S04: Mach thread suspension                                                */
/* ------------------------------------------------------------------------- */
static mach_port_t g_mach_th;
static _Atomic int g_mach_ready;

static void *mach_target(void *a) {
  (void)a;
  g_mach_th = mach_thread_self();
  atomic_store(&g_mach_ready, 1);
  while (!atomic_load(&g_quit)) {
    g_counter = 0;
    uint64_t r = ((uint64_t(*)(volatile uint64_t *))g_code)(&g_counter);
    g_ret_value = r;
    g_t_back = tick();
    atomic_store_explicit(&g_done, 1, memory_order_release);
    while (atomic_load(&g_done) && !atomic_load(&g_quit)) spin_pause();
  }
  return NULL;
}

static int child_mach(void *a) {
  (void)a;
  if (setup_jit()) { INFO("S04.setup", "MAP_JIT unavailable"); return 0; }
  atomic_store(&g_quit, 0);
  atomic_store(&g_mach_ready, 0);
  atomic_store(&g_done, 0);
  pthread_t th;
  pthread_create(&th, NULL, mach_target, NULL);
  while (!atomic_load(&g_mach_ready)) spin_pause();
  while (g_counter < 1000) spin_pause();
  enum { N = 3000 };
  uint64_t *a_su = calloc(N, 8), *a_gs = calloc(N, 8), *a_re = calloc(N, 8), *a_all = calloc(N, 8);
  int stopped = 0, pc_in = 0, n = 0, kr_bad = 0;
  for (int i = 0; i < N; i++) {
    if (i > 0) {
      /* previous iteration ended with the thread left in the loop */
    }
    usleep(i % 11);
    uint64_t t0 = tick();
    kern_return_t kr = thread_suspend(g_mach_th);
    uint64_t t1 = tick();
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    kern_return_t kr2 = thread_get_state(g_mach_th, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt);
    uint64_t t2 = tick();
    if (kr != KERN_SUCCESS || kr2 != KERN_SUCCESS) { kr_bad++; if (kr == KERN_SUCCESS) thread_resume(g_mach_th); continue; }
    uint64_t c1 = g_counter;
    uint64_t w = tick();
    while (tick() - w < 20000) spin_pause();
    if (c1 == g_counter) stopped++;
    if (ts.__pc >= (uint64_t)(uintptr_t)g_code && ts.__pc < (uint64_t)(uintptr_t)g_code + 16) pc_in++;
    uint64_t t3 = tick();
    thread_resume(g_mach_th);
    uint64_t w2 = tick();
    uint64_t c3 = g_counter;
    while (g_counter == c3 && tick() - w2 < 100000000ULL) spin_pause();
    uint64_t t4 = tick();
    a_su[n] = t1 - t0;
    a_gs[n] = t2 - t1;
    a_re[n] = t4 - t3;
    a_all[n] = t4 - t0 - 20000;
    n++;
  }
  cd_stats su = cd_stats_of(a_su, n), gs = cd_stats_of(a_gs, n), re = cd_stats_of(a_re, n), al = cd_stats_of(a_all, n);
  INFO("S04.mach", "thread_suspend / thread_get_state / thread_resume on a thread spinning in JIT code: %d cycles, %d kern_return failures, thread really stopped in %d, pc inside the JIT loop in %d", n, kr_bad, stopped, pc_in);
  INFO("S04.mach", "  thread_suspend returns (us): " CD_STATS_FMT, CD_STATS_ARGS(su, 1000.0));
  INFO("S04.mach", "  thread_get_state ARM_THREAD_STATE64 (us): " CD_STATS_FMT, CD_STATS_ARGS(gs, 1000.0));
  INFO("S04.mach", "  thread_resume -> thread running again (us): " CD_STATS_FMT, CD_STATS_ARGS(re, 1000.0));
  INFO("S04.mach", "  whole cycle without the 20 us freeze check (us): " CD_STATS_FMT, CD_STATS_ARGS(al, 1000.0));
  free(a_su); free(a_gs); free(a_re); free(a_all);

  /* redirect through thread_set_state */
  enum { M = 2000 };
  uint64_t *rd = calloc(M, 8);
  int wrong = 0, m = 0;
  for (int i = 0; i < M; i++) {
    atomic_store(&g_done, 0);
    /* thread is in the loop (it restarts it after each return) */
    g_counter = 0;
    uint64_t w = tick();
    while (g_counter < 1000 && tick() - w < 500000000ULL) spin_pause();
    usleep(i % 7);
    uint64_t t0 = tick();
    thread_suspend(g_mach_th);
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    thread_get_state(g_mach_th, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt);
    ts.__pc = (uint64_t)(uintptr_t)&g_code[4];
    kern_return_t ks = thread_set_state(g_mach_th, ARM_THREAD_STATE64, (thread_state_t)&ts, ARM_THREAD_STATE64_COUNT);
    thread_resume(g_mach_th);
    uint64_t w0 = tick();
    while (!atomic_load_explicit(&g_done, memory_order_acquire) && tick() - w0 < 500000000ULL) spin_pause();
    if (!atomic_load(&g_done) || g_ret_value != 0x1234 || ks != KERN_SUCCESS) wrong++;
    else rd[m++] = g_t_back - t0;
    atomic_store(&g_done, 0);
    /* wait for the thread to re-enter the loop (it restarts it itself) */
  }
  cd_stats rs = cd_stats_of(rd, m);
  INFO("S04.mach", "redirect by thread_suspend + get_state + set_state(pc = JIT stub) + resume: %d trials, %d wrong; suspend -> target back in C at the new pc (us): " CD_STATS_FMT, M, wrong, CD_STATS_ARGS(rs, 1000.0));
  atomic_store(&g_quit, 1);
  atomic_store(&g_done, 0);
  /* make the thread leave the loop if it is inside it */
  thread_suspend(g_mach_th);
  arm_thread_state64_t ts2;
  mach_msg_type_number_t cnt2 = ARM_THREAD_STATE64_COUNT;
  thread_get_state(g_mach_th, ARM_THREAD_STATE64, (thread_state_t)&ts2, &cnt2);
  ts2.__pc = (uint64_t)(uintptr_t)&g_code[4];
  thread_set_state(g_mach_th, ARM_THREAD_STATE64, (thread_state_t)&ts2, ARM_THREAD_STATE64_COUNT);
  thread_resume(g_mach_th);
  pthread_join(th, NULL);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S05: masks                                                                 */
/* ------------------------------------------------------------------------- */
static volatile pthread_t g_who[256];
static _Atomic int g_who_n;
static volatile int g_sa_mask_during;
static _Atomic int g_h_count;

static void who_handler(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si; (void)uctx;
  int i = atomic_fetch_add(&g_who_n, 1);
  if (i < 256) g_who[i] = pthread_self();
}

static void count_handler(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si; (void)uctx;
  atomic_fetch_add(&g_h_count, 1);
}

static void mask_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig; (void)si;
  sigset_t cur;
  pthread_sigmask(SIG_BLOCK, NULL, &cur);
  g_sa_mask_during = sigismember(&cur, SIGUSR2) | (sigismember(&cur, SIGUSR1) << 1) | (sigismember(&cur, SIGURG) << 2);
  /* edit the mask saved in the context: SIGURG will be blocked after return if sigreturn applies uc_sigmask */
  uc->uc_sigmask |= (1u << (SIGURG - 1));
  /* live changes made with pthread_sigmask inside the handler */
  sigset_t un, bl;
  sigemptyset(&un);
  sigaddset(&un, SIGUSR2);
  pthread_sigmask(SIG_UNBLOCK, &un, NULL);
  sigemptyset(&bl);
  sigaddset(&bl, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &bl, NULL);
}

static void *sleeper(void *a) {
  _Atomic int *stop = a;
  while (!atomic_load(stop)) usleep(200);
  return NULL;
}

static void *mask_probe_thread(void *a) {
  sigset_t cur;
  pthread_sigmask(SIG_BLOCK, NULL, &cur);
  *(int *)a = sigismember(&cur, SIGUSR1) | (sigismember(&cur, SIGUSR2) << 1);
  return NULL;
}

static int child_masks(void *a) {
  (void)a;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = who_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR2, &sa, NULL);
  /* a. process-directed signals: who gets them? */
  _Atomic int stop = 0;
  pthread_t th[4];
  for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, sleeper, &stop);
  usleep(5000);
  atomic_store(&g_who_n, 0);
  for (int i = 0; i < 200; i++) { kill(getpid(), SIGUSR2); usleep(300); }
  int counts[5] = {0};
  for (int i = 0; i < atomic_load(&g_who_n) && i < 256; i++) {
    int k = 4;
    for (int j = 0; j < 4; j++) if (pthread_equal(g_who[i], th[j])) k = j;
    counts[k]++;
  }
  INFO("S05.process-directed", "200 kill(getpid(), SIGUSR2) with the signal unblocked in main + 4 sleeping threads: handled %d times; by thread 0..3 = %d %d %d %d, by the main thread = %d",
       atomic_load(&g_who_n), counts[0], counts[1], counts[2], counts[3], counts[4]);
  /* b. main blocks SIGUSR2: delivered to another thread */
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR2);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
  atomic_store(&g_who_n, 0);
  for (int i = 0; i < 200; i++) { kill(getpid(), SIGUSR2); usleep(300); }
  memset(counts, 0, sizeof counts);
  for (int i = 0; i < atomic_load(&g_who_n) && i < 256; i++) {
    int k = 4;
    for (int j = 0; j < 4; j++) if (pthread_equal(g_who[i], th[j])) k = j;
    counts[k]++;
  }
  INFO("S05.process-directed", "same with SIGUSR2 blocked in the main thread: handled %d times; by thread 0..3 = %d %d %d %d, by the main thread = %d", atomic_load(&g_who_n), counts[0], counts[1], counts[2], counts[3], counts[4]);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  atomic_store(&stop, 1);
  for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);

  /* c. pending, thread-directed, and coalescing */
  struct sigaction sc;
  memset(&sc, 0, sizeof sc);
  sc.sa_sigaction = count_handler;
  sc.sa_flags = SA_SIGINFO;
  sigemptyset(&sc.sa_mask);
  sigaction(SIGUSR1, &sc, NULL);
  sigset_t b1;
  sigemptyset(&b1);
  sigaddset(&b1, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &b1, NULL);
  atomic_store(&g_h_count, 0);
  for (int i = 0; i < 10; i++) pthread_kill(pthread_self(), SIGUSR1);
  sigset_t pend;
  sigpending(&pend);
  int was_pending = sigismember(&pend, SIGUSR1);
  pthread_sigmask(SIG_UNBLOCK, &b1, NULL);
  INFO("S05.coalescing", "10 pthread_kill(self, SIGUSR1) while blocked: pending=%d, after unblocking the handler ran %d time(s) (standard signals do not queue)", was_pending, atomic_load(&g_h_count));

  /* d. mask inheritance */
  pthread_sigmask(SIG_BLOCK, &b1, NULL);
  int probe = -1;
  pthread_t pt;
  pthread_create(&pt, NULL, mask_probe_thread, &probe);
  pthread_join(pt, NULL);
  INFO("S05.inherit", "new thread created while SIGUSR1 is blocked in the creator: its mask has SIGUSR1=%d SIGUSR2=%d (1 = blocked)", probe & 1, (probe >> 1) & 1);
  pthread_sigmask(SIG_UNBLOCK, &b1, NULL);

  /* e. uc_sigmask edits and live mask changes inside a handler */
  struct sigaction sm;
  memset(&sm, 0, sizeof sm);
  sm.sa_sigaction = mask_handler;
  sm.sa_flags = SA_SIGINFO;
  sigemptyset(&sm.sa_mask);
  sigaddset(&sm.sa_mask, SIGTERM);  /* sa_mask: SIGTERM blocked while the handler runs */
  sigaction(SIGUSR1, &sm, NULL);
  sigset_t pre;
  sigemptyset(&pre);
  sigaddset(&pre, SIGUSR2);
  pthread_sigmask(SIG_SETMASK, &pre, NULL);     /* pre-signal mask = { SIGUSR2 } */
  pthread_kill(pthread_self(), SIGUSR1);
  sigset_t after;
  pthread_sigmask(SIG_BLOCK, NULL, &after);
  INFO("S05.uc_sigmask", "inside the handler (pre-signal mask {SIGUSR2}, sa_mask {SIGTERM}): SIGUSR2 blocked=%d, SIGUSR1 (the delivered signal) blocked=%d, SIGURG blocked=%d",
       g_sa_mask_during & 1, (g_sa_mask_during >> 1) & 1, (g_sa_mask_during >> 2) & 1);
  INFO("S05.uc_sigmask", "handler did: uc_sigmask |= SIGURG; pthread_sigmask(UNBLOCK, SIGUSR2); pthread_sigmask(BLOCK, SIGPIPE). Mask after sigreturn: SIGURG=%d (1 = the edit of uc_sigmask is applied), SIGUSR2=%d (1 = the live UNBLOCK was discarded), SIGPIPE=%d (0 = the live BLOCK was discarded), SIGTERM=%d, SIGUSR1=%d",
       sigismember(&after, SIGURG), sigismember(&after, SIGUSR2), sigismember(&after, SIGPIPE), sigismember(&after, SIGTERM), sigismember(&after, SIGUSR1));
  return 0;
}

/* ------------------------------------------------------------------------- */
/* S06: signal inventory                                                      */
/* ------------------------------------------------------------------------- */
static void s06(void) {
#ifdef SIGRTMIN
  INFO("S06.signals", "SIGRTMIN is defined: %d", SIGRTMIN);
#else
  INFO("S06.signals", "SIGRTMIN/SIGRTMAX are not defined in <signal.h>: no real-time signals; NSIG=%d, sigset_t is %zu bytes", NSIG, sizeof(sigset_t));
#endif
  static const char *syms[] = {"sigqueue", "sigtimedwait", "sigwaitinfo", "sigwait", "signalfd", "timer_create", "pthread_sigqueue", "tgkill", "sigaltstack", "pthread_kill", "sigsuspend", "sigtimedwait"};
  char sb2[300];
  int o2 = 0;
  for (size_t i = 0; i < sizeof syms / sizeof syms[0]; i++)
    o2 += snprintf(sb2 + o2, sizeof sb2 - o2, " %s=%s", syms[i], dlsym(RTLD_DEFAULT, syms[i]) ? "present" : "absent");
  INFO("S06.symbols", "libSystem symbols:%s", sb2);
  struct sigaction cur;
  char buf[512];
  int o = 0;
  for (int s = 1; s < NSIG; s++) {
    sigaction(s, NULL, &cur);
    if (cur.sa_handler != SIG_DFL) o += snprintf(buf + o, sizeof buf - o, " %s(%d)=%s", sig_name(s), s, cur.sa_handler == SIG_IGN ? "IGN" : "handler");
  }
  INFO("S06.signals", "signals with a non-default disposition at program start:%s", o ? buf : " none");
}

int main(int argc, char **argv) {
  dm_arm_watchdog(220);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
  cd_print_header(argv[0]);
  INFO("HDR.timer", "cntvct_el0 frequency %llu Hz in the parent process; children created by fork() see 24000000 Hz (all stamps are normalised to ns with the process own CNTFRQ_EL0)", (unsigned long long)cd_cntfrq());
  cd_install_probe_handlers();
  static const struct { const char *tag; cd_child_fn fn; unsigned ms; } tests[] = {
      {"S01", child_redirect, 60000}, {"S02", child_blocked, 90000}, {"S03", child_park, 60000},
      {"S04", child_mach, 90000},     {"S05", child_masks, 60000}};
  for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
    if (only && !strstr(only, tests[i].tag)) continue;
    int to = 0;
    char sb[96];
    int st = cd_run_child(tests[i].fn, NULL, tests[i].ms, &to);
    INFO(tests[i].tag, "child result: %s", cd_status_str(st, to, sb, sizeof sb));
  }
  if (!only || strstr(only, "S06")) s06();
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
