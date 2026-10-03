// Witness (k): control transfer to an unmapped address, as in the EAC injection sequence (a call returns to address 9).
// Linux delivers SIGSEGV with si_code SEGV_MAPERR, si_addr = target, and the interrupted RIP = target; the page-fault error
// code has the user and instruction-fetch bits (0x14). The handler moves RIP back to a recovery label.
#include "rt.h"

#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_RESTORER_ 0x04000000

struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
struct siginfo_ { i32 signo, errno_, code, pad; u64 addr; u8 rest[104]; };

static volatile struct { int count, signo, code; u64 addr, rip, err, trapno, cr2, rsp, rbx; } seen;
static volatile u64 g_resume;

static void handler(int sig, struct siginfo_* si, struct ucontext_* uc) {
  seen.count++;
  seen.signo = sig; seen.code = si->code; seen.addr = si->addr;
  seen.rip = uc->mc.rip; seen.err = uc->mc.err; seen.trapno = uc->mc.trapno; seen.cr2 = uc->mc.cr2; seen.rsp = uc->mc.rsp;
  seen.rbx = uc->mc.rbx;
  uc->mc.rip = g_resume;
}
__asm__(".text\n.globl restorer_rt\nrestorer_rt:\n\tmovq $15, %rax\n\tsyscall\n\thlt\n");
extern void restorer_rt(void);

static int run(void) {
  puts_("w11_badjump: jumps and returns to unmapped addresses\n");
  struct ksigaction sa = {(u64)handler, SA_SIGINFO_ | SA_RESTORER_, (u64)restorer_rt, 0};
  check64("rt_sigaction SIGSEGV", (u64)sys4(SYS_rt_sigaction, SIGSEGV_, (i64)&sa, 0, 8), 0);

  // 1. call *%rax with rax = 9 (an indirect call to an unmapped low address)
  seen.count = 0;
  __asm__ volatile(
    "leaq 1f(%%rip), %%rcx\n\t"
    "movq %%rcx, %0\n\t"
    "movq %%rsp, %%rbx\n\t"
    "movq $9, %%rax\n\t"
    "call *%%rax\n\t"
    "1: movq %%rbx, %%rsp\n\t"
    : "=m"(g_resume) : : "rax", "rbx", "rcx", "memory", "cc");
  check64("call to 9: one signal", seen.count, 1);
  check64("call to 9: SIGSEGV", seen.signo, SIGSEGV_);
  check64("call to 9: si_code SEGV_MAPERR", seen.code, 1);
  check64("call to 9: si_addr", seen.addr, 9);
  check64("call to 9: rip in the context", seen.rip, 9);
  check64("call to 9: trapno 14 (#PF)", seen.trapno, 14);
  check64("call to 9: error code user|ifetch", seen.err, 0x14);
  check64("call to 9: cr2", seen.cr2, 9);

  // 2. ret to 9, the exact EAC pattern: push 9 then ret
  seen.count = 0;
  __asm__ volatile(
    "leaq 1f(%%rip), %%rcx\n\t"
    "movq %%rcx, %0\n\t"
    "movq %%rsp, %%rbx\n\t"
    "pushq $9\n\t"
    "ret\n\t"
    "1: movq %%rbx, %%rsp\n\t"
    : "=m"(g_resume) : : "rax", "rbx", "rcx", "memory", "cc");
  check64("ret to 9: one signal", seen.count, 1);
  check64("ret to 9: si_addr", seen.addr, 9);
  check64("ret to 9: rip in the context", seen.rip, 9);
  check64("ret to 9: rsp after the ret (popped)", seen.rsp, seen.rbx);

  // 3. jump to a high unmapped address
  seen.count = 0;
  __asm__ volatile(
    "leaq 1f(%%rip), %%rcx\n\t"
    "movq %%rcx, %0\n\t"
    "movq $0x5ffff0000000, %%rax\n\t"
    "jmp *%%rax\n\t"
    "1:\n\t"
    : "=m"(g_resume) : : "rax", "rcx", "memory", "cc");
  check64("jmp to an unmapped high address: si_addr", seen.addr, 0x5ffff0000000ULL);
  check64("jmp to an unmapped high address: rip", seen.rip, 0x5ffff0000000ULL);
  return finish("w11_badjump");
}
WITNESS_MAIN(run)
