// Witness (o): asynchronous signals that reach a thread while it runs translated code without any system call.
// A spinner thread loops in pure registers (no memory traffic except a stop flag) while the main thread sends it
// SIGUSR1 with tgkill. Linux interrupts the loop at an instruction boundary, runs the handler on the spinner and resumes
// it with every register intact. Then the roles swap: the main thread spins and a helper sends kill() to the process.
// Needs a kernel that interrupts running code (fexdarwin LKD mode); at most 2 guest threads.
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20 };
#define SYS_sched_yield_ 24
#define SYS_kill_ 62
#define SYS_tgkill_ 234
#define SIGUSR1_ 10
#define SIGUSR2_ 12
#define SA_SIGINFO_ 4
#define SA_RESTORER_ 0x04000000
#define SI_USER_ 0
#define SI_TKILL_ (-6)
#define CLONE_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000)
#define STACK (256 * 1024)
#define ROUNDS 5

struct ksigaction { u64 handler, flags, restorer, mask; };
struct siginfo_ { i32 signo, errno_, code, pad; i32 pid, uid; u8 rest[104]; };

static volatile i32 g_hits[64];
static volatile i32 g_code[64];
static volatile i32 g_sender[64];
static volatile i32 g_handler_tid[64];
static volatile i32 g_stop;
static volatile i32 g_started;
static volatile u64 g_spins;
static volatile u64 g_regs_ok;
static volatile i32 g_tid_spinner;
static volatile i32 g_ctid;

static i64 futex(volatile i32* a, int op, int v) { return sys6(SYS_futex, (i64)a, op | 128, v, 0, 0, 0); }
static i64 now_ms(void) { i64 t[2]; sys2(SYS_clock_gettime, 1, (i64)t); return t[0] * 1000 + t[1] / 1000000; }

static void handler(int sig, struct siginfo_* si, void* uc) {
  (void)uc;
  g_code[sig] = si->code;
  g_sender[sig] = si->pid;
  g_handler_tid[sig] = (i32)sys0(SYS_gettid);
  // clobber caller-saved registers that the interrupted loop keeps live: the kernel must restore them
  __asm__ volatile("movq $0x1111, %%rax\n\tmovq $0x2222, %%rcx\n\tmovq $0x3333, %%rdx\n\tmovq $0x4444, %%rsi\n\t"
                   "movq $0x5555, %%r8\n\tmovq $0x6666, %%r9\n\tmovq $0x7777, %%r10\n\tmovq $0x8888, %%r11\n\t"
                   "pxor %%xmm0, %%xmm0\n\tpxor %%xmm5, %%xmm5\n\t"
                   ::: "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "xmm0", "xmm5", "memory");
  __atomic_add_fetch(&g_hits[sig], 1, __ATOMIC_SEQ_CST);
}
__asm__(".text\n.globl restorer_rt\nrestorer_rt:\n\tmovq $15, %rax\n\tsyscall\n\thlt\n");
extern void restorer_rt(void);

// Spins without system calls until *stop is set. Keeps a running sum in rax/rcx/rdx/rsi/r8..r11 and xmm0/xmm5 that
// must stay consistent if every register survives the interruptions. Returns 1 if the invariants hold.
static u64 spin_until(volatile i32* stop) {
  u64 ok, n;
  __asm__ volatile(
    "xorl %%eax, %%eax\n\t"           // rax = i
    "movq $0x0123456789abcdef, %%rcx\n\t"
    "movq %%rcx, %%rdx\n\t"           // rdx tracks rcx
    "xorl %%esi, %%esi\n\t"           // rsi = 3 * i
    "movq $7, %%r8\n\t"
    "movq $7, %%r9\n\t"               // r9 tracks r8
    "movq $-1, %%r10\n\t"
    "movq $-1, %%r11\n\t"             // r11 tracks r10
    "movq %%rcx, %%xmm0\n\t"
    "movq %%rcx, %%xmm5\n\t"          // xmm5 tracks xmm0
    "1:\n\t"
    "addq $1, %%rax\n\t"
    "addq $3, %%rsi\n\t"
    "rolq $5, %%rcx\n\t"
    "rolq $5, %%rdx\n\t"
    "imulq $3, %%r8, %%r8\n\t"
    "imulq $3, %%r9, %%r9\n\t"
    "subq $1, %%r10\n\t"
    "subq $1, %%r11\n\t"
    "paddq %%xmm0, %%xmm0\n\t"
    "paddq %%xmm5, %%xmm5\n\t"
    "cmpl $0, (%[stop])\n\t"
    "je 1b\n\t"
    // invariants
    "movq %%rax, %[n]\n\t"
    "xorl %%edi, %%edi\n\t"
    "cmpq %%rcx, %%rdx\n\t setne %%dil\n\t"
    "movq %%rdi, %[ok]\n\t"
    "cmpq %%r8, %%r9\n\t setne %%dil\n\t orq %%rdi, %[ok]\n\t"
    "cmpq %%r10, %%r11\n\t setne %%dil\n\t orq %%rdi, %[ok]\n\t"
    "leaq (%%rax,%%rax,2), %%rdi\n\t cmpq %%rdi, %%rsi\n\t setne %%dil\n\t movzbq %%dil, %%rdi\n\t orq %%rdi, %[ok]\n\t"
    "movq %%rax, %%rdi\n\t notq %%rdi\n\t cmpq %%rdi, %%r10\n\t setne %%dil\n\t movzbq %%dil, %%rdi\n\t orq %%rdi, %[ok]\n\t"
    "pcmpeqq %%xmm5, %%xmm0\n\t movq %%xmm0, %%rdi\n\t notq %%rdi\n\t orq %%rdi, %[ok]\n\t"
    : [ok] "=&r"(ok), [n] "=&r"(n)
    : [stop] "r"(stop)
    : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "xmm0", "xmm5", "memory", "cc");
  g_spins = n;
  return ok == 0;
}

static void spinner(u64 arg) {
  (void)arg;
  g_tid_spinner = (i32)sys0(SYS_gettid);
  __atomic_store_n(&g_started, 1, __ATOMIC_SEQ_CST);
  g_regs_ok = spin_until(&g_stop);
}

static void helper(u64 pid) {
  // wait until the main thread spins, then signal the whole process; the helper blocks SIGUSR2 so only the main
  // thread can take it
  u64 mask = 1ULL << (SIGUSR2_ - 1);
  sys4(SYS_rt_sigprocmask, 0, (i64)&mask, 0, 8);
  while (!__atomic_load_n(&g_started, __ATOMIC_SEQ_CST)) sys0(SYS_sched_yield_);
  for (int r = 0; r < ROUNDS; r++) {
    sys2(SYS_kill_, (i64)pid, SIGUSR2_);
    i64 t0 = now_ms();
    while (g_hits[SIGUSR2_] < r + 1 && now_ms() - t0 < 2000) sys0(SYS_sched_yield_);
  }
  __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
}

static i64 spawn(void (*fn)(u64), u64 arg, u64 stack_top, volatile i32* ctid) {
  u64* sp = (u64*)stack_top;
  *--sp = arg;
  *--sp = (u64)fn;
  i64 ret;
  register i64 r10 __asm__("r10") = (i64)ctid;
  register i64 r8 __asm__("r8") = 0;
  __asm__ volatile("syscall\n\ttestq %%rax, %%rax\n\tjnz 1f\n\tpopq %%rax\n\tpopq %%rdi\n\tandq $-16, %%rsp\n\t"
                   "call *%%rax\n\tmovl $60, %%eax\n\txorl %%edi, %%edi\n\tsyscall\n\thlt\n1:\n\t"
                   : "=a"(ret)
                   : "a"((i64)SYS_clone), "D"((i64)(CLONE_FLAGS & ~0x80000)), "S"((i64)sp), "d"((i64)ctid), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return ret;
}
static void join(volatile i32* tid) {
  for (;;) { i32 v = *tid; if (v == 0) return; futex(tid, 0, v); }
}

static int run(void) {
  puts_("w15_async: signals that interrupt translated code\n");
  struct ksigaction sa = {(u64)handler, SA_SIGINFO_ | SA_RESTORER_, (u64)restorer_rt, 0};
  check64("rt_sigaction SIGUSR1", (u64)sys4(SYS_rt_sigaction, SIGUSR1_, (i64)&sa, 0, 8), 0);
  check64("rt_sigaction SIGUSR2", (u64)sys4(SYS_rt_sigaction, SIGUSR2_, (i64)&sa, 0, 8), 0);
  const i32 pid = (i32)sys0(SYS_getpid);
  u64 stacks = (u64)sys6(SYS_mmap, 0, 2 * STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  checkb("stacks mapped", (i64)stacks > 0);

  // 1. tgkill to a spinning thread
  g_stop = 0; g_started = 0;
  i64 t = spawn(spinner, 0, stacks + STACK, &g_ctid);
  checkb("clone", t > 0);
  while (!__atomic_load_n(&g_started, __ATOMIC_SEQ_CST)) sys0(SYS_sched_yield_);
  int delivered = 0;
  i64 worst = 0;
  for (int r = 0; r < ROUNDS; r++) {
    i64 t0 = now_ms();
    sys3(SYS_tgkill_, pid, g_tid_spinner, SIGUSR1_);
    while (g_hits[SIGUSR1_] < r + 1 && now_ms() - t0 < 2000) sys0(SYS_sched_yield_);
    if (g_hits[SIGUSR1_] == r + 1) delivered++;
    if (now_ms() - t0 > worst) worst = now_ms() - t0;
  }
  __atomic_store_n(&g_stop, 1, __ATOMIC_SEQ_CST);
  join(&g_ctid);
  check64("tgkill: every signal reached the spinning thread", (u64)delivered, ROUNDS);
  check64("tgkill: handler count", (u64)g_hits[SIGUSR1_], ROUNDS);
  check64("tgkill: si_code SI_TKILL", (u64)(i64)g_code[SIGUSR1_], (u64)(i64)SI_TKILL_);
  check64("tgkill: si_pid", (u64)g_sender[SIGUSR1_], (u64)pid);
  check64("tgkill: handler ran on the spinner", (u64)g_handler_tid[SIGUSR1_], (u64)g_tid_spinner);
  check64("tgkill: spinner registers intact after the interruptions", g_regs_ok, 1);
  checkb("tgkill: the spinner made progress", g_spins > 1000);
  puts_("  worst delivery latency ms="); put_dec(worst); puts_("\n");

  // 2. kill() to the process while the main thread spins
  g_stop = 0; g_started = 0; g_ctid = 0;
  t = spawn(helper, (u64)pid, stacks + 2 * STACK, &g_ctid);
  checkb("clone helper", t > 0);
  __atomic_store_n(&g_started, 1, __ATOMIC_SEQ_CST);
  u64 ok = spin_until(&g_stop);
  join(&g_ctid);
  check64("kill: handler count", (u64)g_hits[SIGUSR2_], ROUNDS);
  check64("kill: si_code SI_USER", (u64)(i64)g_code[SIGUSR2_], SI_USER_);
  check64("kill: handler ran on the main thread", (u64)g_handler_tid[SIGUSR2_], (u64)pid);
  check64("kill: main thread registers intact", ok, 1);
  return finish("w15_async");
}
WITNESS_MAIN(run)
