/*
 * cd_common.h - shared helpers for the C (JIT) and D (faults and signals)
 * witnesses of the darwin-memory group.
 *
 * Starts from common.h (earlier attempt, same group): same output convention,
 * same watchdog, same mach_vm_region helpers.  common.h is copied rather than
 * included because other witnesses of the group (a_, b_, e_) may edit it while
 * these programs are being built.  The copy was made on 2026-10-02.
 *
 * Output convention: every test prints lines "PASS <id> ...", "FAIL <id> ...",
 * "INFO <id> ..." or "NOTTESTABLE <id> ...".  PASS means the behaviour FEXCore
 * needs is present (or the stated expectation holds), FAIL means it is absent or
 * the expectation was wrong, INFO is a plain measurement.
 *
 * Nothing here needs root, a kernel extension, a network or an authorization
 * prompt.  Only plain arm64 (not arm64e) is supported: the thread state is
 * accessed through the non-opaque fields of <mach/arm/_structs.h>.
 */
#ifndef CD_COMMON_H
#define CD_COMMON_H

#define _DARWIN_C_SOURCE 1

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#define CD_UNUSED __attribute__((unused))

static int g_pass, g_fail, g_info;

#define DM_OUT(kind, id, ...)                                                  \
  do {                                                                         \
    char dm_line_[1024];                                                       \
    int dm_n_ = snprintf(dm_line_, sizeof dm_line_, "%s %s ", kind, id);       \
    if (dm_n_ < (int)sizeof dm_line_)                                          \
      dm_n_ += snprintf(dm_line_ + dm_n_, sizeof dm_line_ - dm_n_, __VA_ARGS__); \
    if (dm_n_ > (int)sizeof dm_line_ - 2) dm_n_ = (int)sizeof dm_line_ - 2;    \
    dm_line_[dm_n_++] = '\n';                                                  \
    (void)!write(1, dm_line_, (size_t)dm_n_);                                  \
  } while (0)
#define PASS(id, ...) do { g_pass++; DM_OUT("PASS", id, __VA_ARGS__); } while (0)
#define FAIL(id, ...) do { g_fail++; DM_OUT("FAIL", id, __VA_ARGS__); } while (0)
#define INFO(id, ...) do { g_info++; DM_OUT("INFO", id, __VA_ARGS__); } while (0)
#define NOTTESTABLE(id, ...) do { DM_OUT("NOTTESTABLE", id, __VA_ARGS__); } while (0)
#define CHECK(cond, id, ...)                                                   \
  do {                                                                         \
    if (cond) PASS(id, __VA_ARGS__); else FAIL(id, __VA_ARGS__);               \
  } while (0)

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* 24 MHz virtual counter (about 41.67 ns per tick); mach_absolute_time reads it directly. */
static inline uint64_t cd_cntvct(void) {
  uint64_t v;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
}
static inline uint64_t cd_cntfrq(void) {
  uint64_t v;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}
/* Nanoseconds from the virtual counter, normalised with this process's own CNTFRQ_EL0.
 * Measured on this machine: an exec'd process sees a 1 GHz counter, but a process created
 * by fork() without exec sees a 24 MHz counter (CNTFRQ_EL0 = 24000000).  Raw ticks must
 * never be compared between the two kinds of process. */
static inline uint64_t cd_ns(void) {
  uint64_t f = cd_cntfrq();
  uint64_t t = cd_cntvct();
  if (f == 1000000000ULL) return t;
  return (t / f) * 1000000000ULL + ((t % f) * 1000000000ULL) / f;
}

static CD_UNUSED const char *errno_name(int e) {
  switch (e) {
  case 0: return "0";
  case EPERM: return "EPERM";
  case ENOENT: return "ENOENT";
  case ESRCH: return "ESRCH";
  case EINTR: return "EINTR";
  case EIO: return "EIO";
  case ENXIO: return "ENXIO";
  case ENOEXEC: return "ENOEXEC";
  case EBADF: return "EBADF";
  case ENOMEM: return "ENOMEM";
  case EACCES: return "EACCES";
  case EFAULT: return "EFAULT";
  case EBUSY: return "EBUSY";
  case EEXIST: return "EEXIST";
  case ENODEV: return "ENODEV";
  case EINVAL: return "EINVAL";
  case EMFILE: return "EMFILE";
  case EAGAIN: return "EAGAIN";
  case ENOTSUP: return "ENOTSUP";
  case EOVERFLOW: return "EOVERFLOW";
  default: return "E?";
  }
}

static CD_UNUSED const char *kr_name(kern_return_t kr) {
  switch (kr) {
  case KERN_SUCCESS: return "KERN_SUCCESS";
  case KERN_INVALID_ADDRESS: return "KERN_INVALID_ADDRESS";
  case KERN_PROTECTION_FAILURE: return "KERN_PROTECTION_FAILURE";
  case KERN_NO_SPACE: return "KERN_NO_SPACE";
  case KERN_INVALID_ARGUMENT: return "KERN_INVALID_ARGUMENT";
  case KERN_FAILURE: return "KERN_FAILURE";
  case KERN_RESOURCE_SHORTAGE: return "KERN_RESOURCE_SHORTAGE";
  case KERN_NO_ACCESS: return "KERN_NO_ACCESS";
  case KERN_MEMORY_FAILURE: return "KERN_MEMORY_FAILURE";
  case KERN_MEMORY_ERROR: return "KERN_MEMORY_ERROR";
  case KERN_NOT_SUPPORTED: return "KERN_NOT_SUPPORTED";
  case KERN_INVALID_RIGHT: return "KERN_INVALID_RIGHT";
  case KERN_INVALID_CAPABILITY: return "KERN_INVALID_CAPABILITY";
  case KERN_DENIED: return "KERN_DENIED";
  case KERN_ABORTED: return "KERN_ABORTED";
  case KERN_TERMINATED: return "KERN_TERMINATED";
  default: return "KERN_?";
  }
}

static CD_UNUSED const char *sig_name(int s) {
  switch (s) {
  case 0: return "none";
  case SIGHUP: return "SIGHUP";
  case SIGINT: return "SIGINT";
  case SIGQUIT: return "SIGQUIT";
  case SIGILL: return "SIGILL";
  case SIGTRAP: return "SIGTRAP";
  case SIGABRT: return "SIGABRT";
  case SIGEMT: return "SIGEMT";
  case SIGFPE: return "SIGFPE";
  case SIGKILL: return "SIGKILL";
  case SIGBUS: return "SIGBUS";
  case SIGSEGV: return "SIGSEGV";
  case SIGSYS: return "SIGSYS";
  case SIGPIPE: return "SIGPIPE";
  case SIGALRM: return "SIGALRM";
  case SIGTERM: return "SIGTERM";
  case SIGUSR1: return "SIGUSR1";
  case SIGUSR2: return "SIGUSR2";
  default: return "SIG?";
  }
}

static CD_UNUSED const char *si_code_name(int sig, int code) {
  switch (sig) {
  case SIGSEGV:
    if (code == SEGV_MAPERR) return "SEGV_MAPERR";
    if (code == SEGV_ACCERR) return "SEGV_ACCERR";
    break;
  case SIGBUS:
    if (code == BUS_ADRALN) return "BUS_ADRALN";
    if (code == BUS_ADRERR) return "BUS_ADRERR";
    if (code == BUS_OBJERR) return "BUS_OBJERR";
    break;
  case SIGILL:
    if (code == ILL_ILLOPC) return "ILL_ILLOPC";
    if (code == ILL_ILLTRP) return "ILL_ILLTRP";
    if (code == ILL_PRVOPC) return "ILL_PRVOPC";
    break;
  case SIGTRAP:
    if (code == TRAP_BRKPT) return "TRAP_BRKPT";
    if (code == TRAP_TRACE) return "TRAP_TRACE";
    break;
  default: break;
  }
  if (code == SI_USER) return "SI_USER";
  if (code == SI_QUEUE) return "SI_QUEUE";
  if (code == SI_TIMER) return "SI_TIMER";
  if (code == 0) return "0";
  return "code?";
}

static const char *prot_str(int p, char *buf) {
  buf[0] = (p & 1) ? 'r' : '-';
  buf[1] = (p & 2) ? 'w' : '-';
  buf[2] = (p & 4) ? 'x' : '-';
  buf[3] = 0;
  return buf;
}

/* Watchdog: SIGALRM prints and exits.  Every program arms it in main(). */
static void dm_watchdog_handler(int sig) {
  (void)sig;
  static const char m[] = "FAIL WATCHDOG program exceeded its time budget\n";
  (void)!write(1, m, sizeof m - 1);
  _exit(124);
}
static void dm_arm_watchdog(unsigned seconds) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = dm_watchdog_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);
  alarm(seconds);
}

/* Basic region lookup through mach_vm_region (VM_REGION_BASIC_INFO_64). */
typedef struct {
  mach_vm_address_t start;
  mach_vm_size_t size;
  vm_prot_t prot, maxprot;
  int share_mode;
  bool valid;
} dm_region;

static dm_region dm_region_at_or_after(mach_vm_address_t addr) {
  dm_region r;
  memset(&r, 0, sizeof r);
  mach_vm_address_t a = addr;
  mach_vm_size_t sz = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t obj = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &cnt, &obj);
  if (kr != KERN_SUCCESS) return r;
  r.valid = true;
  r.start = a;
  r.size = sz;
  r.prot = info.protection;
  r.maxprot = info.max_protection;
  r.share_mode = info.shared;
  return r;
}

/* ------------------------------------------------------------------------- */
/* Statistics                                                                */
/* ------------------------------------------------------------------------- */
typedef struct {
  size_t n;
  uint64_t min, p50, p90, p99, p999, max;
  double mean;
} cd_stats;

static int cd_cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}
/* Sorts v in place. */
static CD_UNUSED cd_stats cd_stats_of(uint64_t *v, size_t n) {
  cd_stats s;
  memset(&s, 0, sizeof s);
  if (n == 0) return s;
  qsort(v, n, sizeof v[0], cd_cmp_u64);
  s.n = n;
  s.min = v[0];
  s.max = v[n - 1];
  s.p50 = v[n / 2];
  s.p90 = v[(n * 90) / 100];
  s.p99 = v[(n * 99) / 100];
  s.p999 = v[(n * 999) / 1000];
  long double sum = 0;
  for (size_t i = 0; i < n; i++) sum += v[i];
  s.mean = (double)(sum / n);
  return s;
}
/* Prints a stats line with values divided by `div` (1 = ns, 1000 = us). */
#define CD_STATS_FMT "n=%zu min=%.2f p50=%.2f p90=%.2f p99=%.2f p99.9=%.2f max=%.2f mean=%.2f"
#define CD_STATS_ARGS(s, div) (s).n, (s).min / (div), (s).p50 / (div), (s).p90 / (div), (s).p99 / (div), \
                              (s).p999 / (div), (s).max / (div), (s).mean / (div)

/* ------------------------------------------------------------------------- */
/* arm64 instruction encodings (A64)                                         */
/* ------------------------------------------------------------------------- */
#define A64_RET 0xd65f03c0u
#define A64_NOP 0xd503201fu
#define A64_UDF 0x00000000u
#define A64_BRK(imm) (0xd4200000u | ((uint32_t)(imm) << 5))
#define A64_MOVZ_X(rd, imm16) (0xd2800000u | ((uint32_t)(imm16) << 5) | (uint32_t)(rd))
#define A64_MOVK_X(rd, imm16, hw) (0xf2800000u | ((uint32_t)(hw) << 21) | ((uint32_t)(imm16) << 5) | (uint32_t)(rd))
#define A64_MOV_X(rd, rm) (0xaa0003e0u | ((uint32_t)(rm) << 16) | (uint32_t)(rd)) /* orr xd, xzr, xm */
#define A64_ADD_IMM_X(rd, rn, imm12) (0x91000000u | ((uint32_t)(imm12) << 10) | ((uint32_t)(rn) << 5) | (uint32_t)(rd))
#define A64_B(off_bytes) (0x14000000u | (((uint32_t)((int32_t)(off_bytes) >> 2)) & 0x03ffffffu))
#define A64_BL(off_bytes) (0x94000000u | (((uint32_t)((int32_t)(off_bytes) >> 2)) & 0x03ffffffu))
#define A64_BR(rn) (0xd61f0000u | ((uint32_t)(rn) << 5))
#define A64_BLR(rn) (0xd63f0000u | ((uint32_t)(rn) << 5))
#define A64_LDR_LIT_X(rt, off_bytes) (0x58000000u | ((((uint32_t)((int32_t)(off_bytes) >> 2)) & 0x7ffffu) << 5) | (uint32_t)(rt))
#define A64_LDR_X(rt, rn) (0xf9400000u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))  /* ldr xt, [xn] */
#define A64_STR_X(rt, rn) (0xf9000000u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))  /* str xt, [xn] */
#define A64_LDR_W(rt, rn) (0xb9400000u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))  /* ldr wt, [xn] */
#define A64_STR_W(rt, rn) (0xb9000000u | ((uint32_t)(rn) << 5) | (uint32_t)(rt))  /* str wt, [xn] */
#define A64_LDAXR_X(rt, rn) (0xc85ffc00u | ((uint32_t)(rn) << 5) | (uint32_t)(rt)) /* ldaxr xt, [xn] */
#define A64_CBNZ_X(rt, off_bytes) (0xb5000000u | ((((uint32_t)((int32_t)(off_bytes) >> 2)) & 0x7ffffu) << 5) | (uint32_t)(rt))
#define A64_BTI_C 0xd503245fu

/* Emits "movz x0, #v ; ret" at p (8 bytes).  v is 16 bits. */
static inline void cd_emit_ret_const(uint32_t *p, uint32_t v) {
  p[0] = A64_MOVZ_X(0, v & 0xffff);
  p[1] = A64_RET;
}

/* ------------------------------------------------------------------------- */
/* ucontext accessors (plain arm64 only)                                     */
/* ------------------------------------------------------------------------- */
#if !defined(__arm64__) || defined(__arm64e__)
#error "cd_common.h supports plain arm64 only"
#endif
#define CD_MC(uc) ((uc)->uc_mcontext)
#define CD_PC(uc) (CD_MC(uc)->__ss.__pc)
#define CD_SP(uc) (CD_MC(uc)->__ss.__sp)
#define CD_FP(uc) (CD_MC(uc)->__ss.__fp)
#define CD_LR(uc) (CD_MC(uc)->__ss.__lr)
#define CD_X(uc, i) (CD_MC(uc)->__ss.__x[i])
#define CD_CPSR(uc) (CD_MC(uc)->__ss.__cpsr)
#define CD_V(uc, i) (CD_MC(uc)->__ns.__v[i])
#define CD_FPSR(uc) (CD_MC(uc)->__ns.__fpsr)
#define CD_FPCR(uc) (CD_MC(uc)->__ns.__fpcr)
#define CD_FAR(uc) (CD_MC(uc)->__es.__far)
#define CD_ESR(uc) (CD_MC(uc)->__es.__esr)
#define CD_EXC(uc) (CD_MC(uc)->__es.__exception)

/* ESR_EL1 decode: exception class and a short description. */
static CD_UNUSED const char *cd_esr_describe(uint32_t esr, char *buf, size_t n) {
  uint32_t ec = esr >> 26, iss = esr & 0x1ffffff;
  const char *name = "EC?";
  switch (ec) {
  case 0x00: name = "unknown"; break;
  case 0x07: name = "fp-simd-trap"; break;
  case 0x0e: name = "illegal-state"; break;
  case 0x15: name = "svc64"; break;
  case 0x18: name = "msr-mrs-sys"; break;
  case 0x20: name = "iabort-lower"; break;
  case 0x21: name = "iabort-same"; break;
  case 0x22: name = "pc-alignment"; break;
  case 0x24: name = "dabort-lower"; break;
  case 0x25: name = "dabort-same"; break;
  case 0x26: name = "sp-alignment"; break;
  case 0x2c: name = "fp-exception"; break;
  case 0x30: name = "breakpoint-lower"; break;
  case 0x32: name = "sw-step-lower"; break;
  case 0x34: name = "watchpoint-lower"; break;
  case 0x3c: name = "brk64"; break;
  default: break;
  }
  if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
    uint32_t dfsc = iss & 0x3f;
    const char *kind = "dfsc?";
    switch (dfsc >> 2) {
    case 0: kind = "addr-size"; break;
    case 1: kind = "translation"; break;
    case 2: kind = "access-flag"; break;
    case 3: kind = "permission"; break;
    default: break;
    }
    if (dfsc == 0x21) kind = "alignment";
    if (dfsc == 0x10) kind = "sync-external";
    snprintf(buf, n, "EC=0x%02x(%s) ISS=0x%07x DFSC=0x%02x(%s%s) WnR=%u CM=%u", ec, name, iss, dfsc, kind,
             (dfsc < 0x10 ? " L" : ""), (iss >> 6) & 1, (iss >> 8) & 1);
  } else {
    snprintf(buf, n, "EC=0x%02x(%s) ISS=0x%07x", ec, name, iss);
  }
  return buf;
}

/* ------------------------------------------------------------------------- */
/* Child process runner with timeout                                         */
/* ------------------------------------------------------------------------- */
typedef int (*cd_child_fn)(void *);

/* Forks, runs fn(arg) in the child (exit status = return value), waits at most
 * timeout_ms.  Returns the raw wait status or -1; *timed_out set if killed. */
static CD_UNUSED int cd_run_child(cd_child_fn fn, void *arg, unsigned timeout_ms, int *timed_out) {
  if (timed_out) *timed_out = 0;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    alarm((timeout_ms + 999) / 1000 + 1);
    signal(SIGALRM, SIG_DFL);
    int rc = fn(arg);
    _exit(rc & 0xff);
  }
  uint64_t t0 = now_ns();
  int st = 0;
  for (;;) {
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) return st;
    if (r < 0 && errno != EINTR) return -1;
    if ((now_ns() - t0) / 1000000 > timeout_ms) {
      kill(pid, SIGKILL);
      waitpid(pid, &st, 0);
      if (timed_out) *timed_out = 1;
      return st;
    }
    usleep(500);
  }
}

static CD_UNUSED const char *cd_status_str(int st, int timed_out, char *buf, size_t n) {
  if (timed_out) snprintf(buf, n, "TIMEOUT(killed after the budget)");
  else if (st == -1) snprintf(buf, n, "fork-failed");
  else if (WIFSIGNALED(st)) snprintf(buf, n, "killed-by-%s(%d)%s", sig_name(WTERMSIG(st)), WTERMSIG(st),
                                     WCOREDUMP(st) ? "+core" : "");
  else snprintf(buf, n, "exit(%d)", WEXITSTATUS(st));
  return buf;
}

/* ------------------------------------------------------------------------- */
/* JIT region helpers                                                        */
/* ------------------------------------------------------------------------- */
#ifndef MAP_JIT
#define MAP_JIT 0x0800
#endif

static inline void cd_jit_write(void) { pthread_jit_write_protect_np(0); }
static inline void cd_jit_exec(void) { pthread_jit_write_protect_np(1); }

/* mmap(MAP_JIT) with RWX; returns NULL and sets *err on failure. */
static CD_UNUSED void *cd_jit_map(size_t size, int prot, int extra_flags, int *err) {
  void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON | MAP_JIT | extra_flags, -1, 0);
  if (p == MAP_FAILED) {
    if (err) *err = errno;
    return NULL;
  }
  if (err) *err = 0;
  return p;
}

#define CD_RWX (PROT_READ | PROT_WRITE | PROT_EXEC)

/* ------------------------------------------------------------------------- */
/* Probing a memory access with fault recovery (siglongjmp)                  */
/* ------------------------------------------------------------------------- */
typedef struct {
  int sig;
  int code;
  void *addr;
  uint64_t pc;
  uint64_t far;
  uint32_t esr;
} cd_probe_result;

static __thread sigjmp_buf *volatile t_probe_jb;
static __thread cd_probe_result t_probe_res;

static void cd_probe_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  if (!t_probe_jb) {
    char m[256];
    char eb[96];
    int n = snprintf(m, sizeof m, "FAIL PROBE unexpected %s/%s outside a probe: si_addr=%p pc=%#llx far=%#llx %s\n",
                     sig_name(sig), si_code_name(sig, si->si_code), si->si_addr, (unsigned long long)CD_PC(uc),
                     (unsigned long long)CD_FAR(uc), cd_esr_describe(CD_ESR(uc), eb, sizeof eb));
    (void)!write(1, m, (size_t)n);
    _exit(97);
  }
  t_probe_res.sig = sig;
  t_probe_res.code = si->si_code;
  t_probe_res.addr = si->si_addr;
  t_probe_res.pc = CD_PC(uc);
  t_probe_res.far = CD_FAR(uc);
  t_probe_res.esr = CD_ESR(uc);
  siglongjmp(*t_probe_jb, 1);
}

static CD_UNUSED void cd_install_probe_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = cd_probe_handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGTRAP, &sa, NULL);
}

/* Stores v to *p.  Returns 0 when the store worked, otherwise the signal number. */
static CD_UNUSED int cd_probe_store32(volatile uint32_t *p, uint32_t v, cd_probe_result *res) {
  sigjmp_buf jb;
  memset(&t_probe_res, 0, sizeof t_probe_res);
  if (sigsetjmp(jb, 1) == 0) {
    t_probe_jb = &jb;
    *p = v;
    t_probe_jb = NULL;
    return 0;
  }
  t_probe_jb = NULL;
  if (res) *res = t_probe_res;
  return t_probe_res.sig;
}

/* Reads *p.  Returns 0 or the signal number. */
static CD_UNUSED int cd_probe_load32(volatile uint32_t *p, uint32_t *out, cd_probe_result *res) {
  sigjmp_buf jb;
  memset(&t_probe_res, 0, sizeof t_probe_res);
  if (sigsetjmp(jb, 1) == 0) {
    t_probe_jb = &jb;
    uint32_t v = *p;
    t_probe_jb = NULL;
    if (out) *out = v;
    return 0;
  }
  t_probe_jb = NULL;
  if (res) *res = t_probe_res;
  return t_probe_res.sig;
}

/* Calls fn() (a function with signature uint64_t (void)).  Returns 0 or the signal number. */
static CD_UNUSED int cd_probe_call(void *fn, uint64_t *out, cd_probe_result *res) {
  sigjmp_buf jb;
  memset(&t_probe_res, 0, sizeof t_probe_res);
  if (sigsetjmp(jb, 1) == 0) {
    t_probe_jb = &jb;
    uint64_t v = ((uint64_t (*)(void))fn)();
    t_probe_jb = NULL;
    if (out) *out = v;
    return 0;
  }
  t_probe_jb = NULL;
  if (res) *res = t_probe_res;
  return t_probe_res.sig;
}

static CD_UNUSED const char *cd_probe_str(int sig, const cd_probe_result *r, char *buf, size_t n) {
  if (!sig) snprintf(buf, n, "ok");
  else snprintf(buf, n, "%s/%s si_addr=%p pc=%#llx", sig_name(sig), si_code_name(sig, r->code), r->addr,
                (unsigned long long)r->pc);
  return buf;
}

/* ------------------------------------------------------------------------- */
/* Process facts                                                             */
/* ------------------------------------------------------------------------- */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

static CD_UNUSED uint32_t cd_cs_flags(void) {
  uint32_t flags = 0;
  if (csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof flags) != 0) return 0xffffffffu;
  return flags;
}

static CD_UNUSED const char *cd_cs_flags_str(uint32_t f, char *buf, size_t n) {
  static const struct { uint32_t bit; const char *name; } tbl[] = {
      {0x1, "VALID"}, {0x2, "ADHOC"}, {0x4, "GET_TASK_ALLOW"}, {0x8, "INSTALLER"}, {0x10, "FORCED_LV"},
      {0x100, "HARD"}, {0x200, "KILL"}, {0x800, "RESTRICT"}, {0x1000, "ENFORCEMENT"}, {0x2000, "REQUIRE_LV"},
      {0x4000, "ENTITLEMENTS_VALIDATED"}, {0x10000, "RUNTIME"}, {0x20000, "LINKER_SIGNED"},
      {0x10000000, "DEBUGGED"}, {0x20000000, "SIGNED"}, {0x4000000, "PLATFORM_BINARY"}};
  size_t o = 0;
  buf[0] = 0;
  for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
    if (f & tbl[i].bit) o += snprintf(buf + o, o < n ? n - o : 0, "%s%s", o ? "|" : "", tbl[i].name);
  }
  return buf;
}

static CD_UNUSED long long cd_sysctl_ll(const char *name);

/* CTR_EL0 is not readable from user space on this macOS (SIGILL): this returns
 * 0 and sets *sig to the signal number when the read traps. */
static CD_UNUSED uint64_t cd_read_ctr_el0_checked(int *sig) {
  sigjmp_buf jb;
  memset(&t_probe_res, 0, sizeof t_probe_res);
  if (sigsetjmp(jb, 1) == 0) {
    t_probe_jb = &jb;
    uint64_t v;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(v));
    t_probe_jb = NULL;
    if (sig) *sig = 0;
    return v;
  }
  t_probe_jb = NULL;
  if (sig) *sig = t_probe_res.sig;
  return 0;
}

/* Cache line size from sysctl (hw.cachelinesize), 128 on Apple Silicon. */
static CD_UNUSED size_t cd_cache_line(void) {
  static size_t line;
  if (!line) {
    long long v = cd_sysctl_ll("hw.cachelinesize");
    line = v > 0 ? (size_t)v : 64;
  }
  return line;
}

static CD_UNUSED long long cd_sysctl_ll(const char *name) {
  long long v = 0;
  size_t l = sizeof v;
  int i32 = 0;
  size_t l32 = sizeof i32;
  if (sysctlbyname(name, &v, &l, NULL, 0) == 0) {
    if (l == 4) {
      i32 = (int)v;
      (void)l32;
      return i32;
    }
    return v;
  }
  return -1;
}

/* Prints the process header: signing flags, translation, page size. */
static CD_UNUSED void cd_print_header(const char *prog) {
  char fb[256];
  uint32_t f = cd_cs_flags();
  INFO("HDR.proc", "prog=%s pid=%d arch=%s translated=%d pagesize=%ld", prog, (int)getpid(), "arm64",
       (int)cd_sysctl_ll("sysctl.proc_translated"), (long)sysconf(_SC_PAGESIZE));
  INFO("HDR.codesign", "csops(CS_OPS_STATUS)=%#x [%s]", f, cd_cs_flags_str(f, fb, sizeof fb));
  INFO("HDR.jit-support", "pthread_jit_write_protect_supported_np=%d", pthread_jit_write_protect_supported_np());
}

#endif /* CD_COMMON_H */
