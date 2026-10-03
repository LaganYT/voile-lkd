// Witness: recover from a guest stack overflow (SIGSEGV on the alternate stack, handler moves RIP to a recovery label
// that restores RSP), then keep calling and returning normally. Reduced from the end of w07_fault, which hangs.
#include "rt.h"
#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_ONSTACK_ 0x08000000
#define SA_RESTORER_ 0x04000000
struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
static volatile u64 g_resume, g_hits;
static void handler(int sig, void* si, struct ucontext_* uc) { (void)sig; (void)si; g_hits++; uc->mc.rip = g_resume; }
__asm__(".text\n.globl restorer_rt\nrestorer_rt:\n\tmovq $15, %rax\n\tsyscall\n\thlt\n");
extern void restorer_rt(void);
__asm__(".text\n.globl recurse_entry\nrecurse_entry:\n\tsubq $4080, %rsp\n\tmovq $1, (%rsp)\n\tcall recurse_entry\n\taddq $4080, %rsp\n\tret\n");
__attribute__((noinline)) static u64 leaf(u64 x) { return x * 3 + 1; }
__attribute__((noinline)) static u64 mid(u64 x) { return leaf(x) + leaf(x + 1); }
__attribute__((noinline)) static void overflow(void) {
  __asm__ volatile("leaq 1f(%%rip), %%rcx\n\tmovq %%rcx, %0\n\tmovq %%rsp, %%rbx\n\tcall recurse_entry\n\t1: movq %%rbx, %%rsp\n\t"
                   : "=m"(g_resume) : : "rax", "rbx", "rcx", "rdx", "memory", "cc");
}
static int run(void) {
  static u8 alt[65536] __attribute__((aligned(64)));
  struct stack_t_ ss = {(u64)alt, 0, 0, sizeof alt};
  check64("sigaltstack", (u64)sys2(131, (i64)&ss, 0), 0);
  struct ksigaction sa = {(u64)handler, SA_SIGINFO_ | SA_ONSTACK_ | SA_RESTORER_, (u64)restorer_rt, 0};
  check64("sigaction", (u64)sys4(SYS_rt_sigaction, SIGSEGV_, (i64)&sa, 0, 8), 0);
  check64("calls before", mid(5), 35);
  overflow();
  check64("one overflow signal", g_hits, 1);
  check64("calls after the overflow", mid(7), 47);
  overflow();
  check64("second overflow", g_hits, 2);
  check64("calls after the second overflow", mid(9), 59);
  return finish("w14_overflow");
}
WITNESS_MAIN(run)
