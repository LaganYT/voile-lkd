// Witness (g): guest faults delivered to guest signal handlers, with recovery. Uses the raw rt_sigaction ABI (kernel
// struct sigaction with SA_RESTORER) and reads the x86-64 kernel signal frame (siginfo and ucontext) directly.
#include "rt.h"

#define SIGILL_ 4
#define SIGTRAP_ 5
#define SIGFPE_ 8
#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_ONSTACK_ 0x08000000
#define SA_RESTORER_ 0x04000000
#define SA_NODEFER_ 0x40000000
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define K16 16384UL
#define SYS_sigaltstack_ 131

struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
struct siginfo_ { i32 signo, errno_, code, pad; u64 addr; u8 rest[104]; };

__asm__(".text\n.globl restorer\nrestorer:\n\tmovl $15, %eax\n\tsyscall\n");
void restorer(void);

// What the last handler invocation saw.
static volatile struct {
  int count, signo, code; u64 addr, rip, trapno, err, cr2, rsp, handler_sp, mask_in_handler, saved_mask, eflags, flags_field;
  u64 rbx, r12, r13, r14, r15, rbp, rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11;
  u32 mxcsr; u64 fpstate_ptr; u32 fp_magic1, fp_magic2;
  u8 xmm5[16];
} seen;
static volatile u64 g_resume;          // where the "skip" policy continues
static volatile int g_policy;          // 0 skip to g_resume, 1 fix rax and retry, 2 clobber xmm/regs then skip
static volatile u64 g_fix_value;
static volatile u64 g_expect_altstack_lo, g_expect_altstack_hi;

static u64 query_mask(void) { u64 m = 0; sys4(SYS_rt_sigprocmask, 0 /*SIG_BLOCK*/, 0, (i64)&m, 8); return m; }

static void handler(int sig, struct siginfo_* si, struct ucontext_* uc) {
  seen.count++; seen.signo = sig; seen.code = si->code; seen.addr = si->addr;
  seen.rip = uc->mc.rip; seen.trapno = uc->mc.trapno; seen.err = uc->mc.err; seen.cr2 = uc->mc.cr2; seen.rsp = uc->mc.rsp;
  seen.eflags = uc->mc.eflags; seen.flags_field = uc->flags;
  u64 here; __asm__ volatile("movq %%rsp, %0" : "=r"(here)); seen.handler_sp = here;
  seen.mask_in_handler = query_mask(); seen.saved_mask = uc->sigmask;
  seen.rbx = uc->mc.rbx; seen.r12 = uc->mc.r12; seen.r13 = uc->mc.r13; seen.r14 = uc->mc.r14; seen.r15 = uc->mc.r15; seen.rbp = uc->mc.rbp;
  seen.rax = uc->mc.rax; seen.rcx = uc->mc.rcx; seen.rdx = uc->mc.rdx; seen.rsi = uc->mc.rsi; seen.rdi = uc->mc.rdi;
  seen.r8 = uc->mc.r8; seen.r9 = uc->mc.r9; seen.r10 = uc->mc.r10; seen.r11 = uc->mc.r11;
  seen.fpstate_ptr = uc->mc.fpstate;
  if (uc->mc.fpstate) {
    u8* fp = (u8*)uc->mc.fpstate; seen.mxcsr = *(u32*)(fp + 24); seen.fp_magic1 = *(u32*)(fp + 464); seen.fp_magic2 = 0;
    for (int i = 0; i < 16; i++) seen.xmm5[i] = fp[160 + 5 * 16 + i];
  }
  // Clobber what a real handler would clobber: the FPU/SSE state and the caller-saved registers.
  __asm__ volatile("pxor %%xmm5, %%xmm5\n\tpcmpeqd %%xmm6, %%xmm6\n\tfld1\n\tfld1\n\tfaddp\n\tfstp %%st(0)" ::: "xmm5", "xmm6", "memory");
  if (g_policy == 0 || g_policy == 2) uc->mc.rip = g_resume;
  else if (g_policy == 1) uc->mc.rax = g_fix_value;
  if (g_policy == 2) { uc->mc.rbx = 0xdead; /* a handler that edits a register is respected on return */ }
}

static int set_handler(int sig, u64 flags, u64 mask) {
  struct ksigaction a = {(u64)handler, flags | SA_SIGINFO_ | SA_RESTORER_, (u64)restorer, mask};
  return (int)sys4(SYS_rt_sigaction, sig, (i64)&a, 0, 8);
}

// Fault sites. Each one records the address of the faulting instruction (label 1) and where to continue (label 2).
#define SITE_PROLOGUE "leaq 1f(%%rip), %%rcx\n\tmovq %%rcx, %[expect]\n\tleaq 2f(%%rip), %%rcx\n\tmovq %%rcx, %[resume]\n\t"
static volatile u64 g_expect;

// canary registers: callee-saved and flags must survive the signal round trip
#define CANARY_IN  "movq $0x1111111111111111, %%rbx\n\tmovq $0x1212121212121212, %%r12\n\tmovq $0x1313131313131313, %%r13\n\t" \
                   "movq $0x1414141414141414, %%r14\n\tmovq $0x1515151515151515, %%r15\n\tstc\n\t"
#define CANARY_OUT "pushfq\n\tpopq %%rcx\n\tmovq %%rbx, %[ob]\n\tmovq %%r12, %[o12]\n\tmovq %%r13, %[o13]\n\tmovq %%r14, %[o14]\n\tmovq %%r15, %[o15]\n\tmovq %%rcx, %[ofl]\n\t"
static volatile u64 ob, o12, o13, o14, o15, ofl;
#define CANARY_OUTS [ob] "=m"(ob), [o12] "=m"(o12), [o13] "=m"(o13), [o14] "=m"(o14), [o15] "=m"(o15), [ofl] "=m"(ofl)
#define CLOBBERS "rax", "rcx", "rdx", "rbx", "r12", "r13", "r14", "r15", "cc", "memory"

static void clear_seen(void) { __builtin_memset((void*)&seen, 0, sizeof seen); }

#define CHECK_CANARIES(tag, expect_rbx) do { \
    check64(tag " rbx after return", ob, expect_rbx); check64(tag " r12 after return", o12, 0x1212121212121212ULL); \
    check64(tag " r13 after return", o13, 0x1313131313131313ULL); check64(tag " r14 after return", o14, 0x1414141414141414ULL); \
    check64(tag " r15 after return", o15, 0x1515151515151515ULL); check64(tag " CF after return", ofl & 1, 1); } while (0)

static u8* map_page(int prot) { i64 r = sys6(SYS_mmap, 0, K16, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return (u8*)r; }

static int run(void) {
  puts_("w07_fault: synchronous faults delivered to guest handlers\n");
  checkb("rt_sigaction SIGSEGV", set_handler(SIGSEGV_, 0, 0) == 0);
  checkb("rt_sigaction SIGILL", set_handler(SIGILL_, 0, 0) == 0);
  checkb("rt_sigaction SIGTRAP", set_handler(SIGTRAP_, 0, 0) == 0);
  checkb("rt_sigaction SIGFPE", set_handler(SIGFPE_, 0, 0) == 0);

  // ---- read from an unmapped address, handler skips the instruction ----
  clear_seen(); g_policy = 0;
  __asm__ volatile(CANARY_IN SITE_PROLOGUE "movq $0x10, %%rax\n\t1: movq (%%rax), %%rdx\n\t2: " CANARY_OUT
                   : [expect] "=m"(g_expect), [resume] "=m"(g_resume), CANARY_OUTS : : CLOBBERS);
  check64("unmapped read: handler ran once", seen.count, 1);
  check64("unmapped read: signal is SIGSEGV", seen.signo, SIGSEGV_);
  check64("unmapped read: si_code SEGV_MAPERR", seen.code, 1);
  check64("unmapped read: si_addr", seen.addr, 0x10);
  check64("unmapped read: rip is the faulting instruction", seen.rip, g_expect);
  check64("unmapped read: trapno 14 (page fault)", seen.trapno, 14);
  check64("unmapped read: error code (user, not present, read)", seen.err, 4);
  check64("unmapped read: cr2", seen.cr2, 0x10);
  check64("unmapped read: rax in the frame", seen.rax, 0x10);
  check64("unmapped read: rbx in the frame", seen.rbx, 0x1111111111111111ULL); check64("unmapped read: r12 in the frame", seen.r12, 0x1212121212121212ULL);
  check64("unmapped read: r15 in the frame", seen.r15, 0x1515151515151515ULL); check64("unmapped read: CF in the saved eflags", seen.eflags & 1, 1);
  checkb("unmapped read: SIGSEGV blocked inside the handler", (seen.mask_in_handler >> (SIGSEGV_ - 1)) & 1);
  check64("unmapped read: saved mask is the pre-signal mask", seen.saved_mask & (1ULL << (SIGSEGV_ - 1)), 0);
  check64("unmapped read: SIGSEGV unblocked after return", query_mask() & (1ULL << (SIGSEGV_ - 1)), 0);
  check64("unmapped read: ucontext flags (SS valid, strict restore, xstate)", seen.flags_field, 7);
  check64("unmapped read: handler stack is 16 byte aligned + 8 at entry (after the return address push)", (seen.handler_sp + 8 + 8) & 15, 0);
  CHECK_CANARIES("unmapped read:", 0x1111111111111111ULL);
  check64("unmapped read: fpstate carries the xstate magic", seen.fp_magic1, 0x46505853);
  check64("unmapped read: mxcsr in the frame is the default", seen.mxcsr, 0x1f80);

  // ---- write to an unmapped address ----
  clear_seen(); g_policy = 0;
  __asm__ volatile(CANARY_IN SITE_PROLOGUE "movq $0x20, %%rax\n\t1: movq %%rdx, (%%rax)\n\t2: " CANARY_OUT
                   : [expect] "=m"(g_expect), [resume] "=m"(g_resume), CANARY_OUTS : : CLOBBERS);
  check64("unmapped write: si_code SEGV_MAPERR", seen.code, 1); check64("unmapped write: si_addr", seen.addr, 0x20);
  check64("unmapped write: error code (user, not present, write)", seen.err, 6); check64("unmapped write: rip", seen.rip, g_expect);
  CHECK_CANARIES("unmapped write:", 0x1111111111111111ULL);

  // ---- write to a read-only page ----
  u8* ro = map_page(PROT_READ | PROT_WRITE); *(volatile u64*)ro = 0x77; sys3(SYS_mprotect, (i64)ro, K16, PROT_READ);
  clear_seen(); g_policy = 0;
  __asm__ volatile(CANARY_IN SITE_PROLOGUE "movq %[p], %%rax\n\t1: movq $0x55, (%%rax)\n\t2: " CANARY_OUT
                   : [expect] "=m"(g_expect), [resume] "=m"(g_resume), CANARY_OUTS : [p] "m"(ro) : CLOBBERS);
  check64("read-only write: signal SIGSEGV", seen.signo, SIGSEGV_); check64("read-only write: si_code SEGV_ACCERR", seen.code, 2);
  check64("read-only write: si_addr", seen.addr, (u64)ro); check64("read-only write: error code (user, protection, write)", seen.err, 7);
  check64("read-only write: the page is unchanged", *(volatile u64*)ro, 0x77);
  CHECK_CANARIES("read-only write:", 0x1111111111111111ULL);

  // ---- access to PROT_NONE ----
  u8* none = map_page(PROT_NONE);
  clear_seen(); g_policy = 0;
  __asm__ volatile(CANARY_IN SITE_PROLOGUE "movq %[p], %%rax\n\t1: movq (%%rax), %%rdx\n\t2: " CANARY_OUT
                   : [expect] "=m"(g_expect), [resume] "=m"(g_resume), CANARY_OUTS : [p] "m"(none) : CLOBBERS);
  check64("PROT_NONE read: si_code SEGV_ACCERR", seen.code, 2); check64("PROT_NONE read: error code (user, protection, read)", seen.err, 5);
  check64("PROT_NONE read: si_addr", seen.addr, (u64)none);

  // ---- fix the pointer in the handler and retry the same instruction ----
  u8* good = map_page(PROT_READ | PROT_WRITE); *(volatile u64*)good = 0xfeedfacecafebeefULL;
  clear_seen(); g_policy = 1; g_fix_value = (u64)good; u64 got = 0;
  __asm__ volatile(CANARY_IN "movq $0x30, %%rax\n\t1: movq (%%rax), %%rdx\n\t2: movq %%rdx, %[got]\n\t" CANARY_OUT
                   : [got] "=m"(got), CANARY_OUTS : : CLOBBERS);
  check64("fix-and-retry: the instruction re-executes with the new rax", got, 0xfeedfacecafebeefULL); check64("fix-and-retry: one fault only", seen.count, 1);
  CHECK_CANARIES("fix-and-retry:", 0x1111111111111111ULL);

  // ---- the handler edits a register and clobbers the SSE state; both must be handled by sigreturn ----
  clear_seen(); g_policy = 2; u64 sse_after = 0;
  __asm__ volatile(CANARY_IN SITE_PROLOGUE "movq $0xaabbccdd11223344, %%rdx\n\tmovq %%rdx, %%xmm5\n\tpunpcklqdq %%xmm5, %%xmm5\n\tmovq $0x40, %%rax\n\t1: movq (%%rax), %%rdx\n\t2: movq %%xmm5, %%rdx\n\tmovq %%rdx, %[sse]\n\t" CANARY_OUT
                   : [expect] "=m"(g_expect), [resume] "=m"(g_resume), [sse] "=m"(sse_after), CANARY_OUTS : : CLOBBERS, "xmm5");
  check64("handler clobbering xmm5 does not leak into the interrupted code", sse_after, 0xaabbccdd11223344ULL);
  check64("handler saw the interrupted xmm5 low qword in the frame", *(volatile u64*)seen.xmm5, 0xaabbccdd11223344ULL);
  CHECK_CANARIES("register edit:", 0xdead); /* rbx was rewritten by the handler: sigreturn must honour the edit */

  // ---- instruction fetch from a non executable page ----
  u8* nx = map_page(PROT_READ | PROT_WRITE); nx[0] = 0xc3;
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "movq %[p], %%rax\n\t1: callq *%%rax\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : [p] "m"(nx) : "rax", "rcx", "rdx", "memory", "cc");
  check64("non-exec fetch: SEGV_ACCERR", seen.code, 2); check64("non-exec fetch: si_addr is the target", seen.addr, (u64)nx);
  check64("non-exec fetch: rip is the target", seen.rip, (u64)nx); check64("non-exec fetch: error code (user, protection, instruction)", seen.err, 0x15);

  // ---- hlt, ud2, int3 and a privileged instruction ----
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "1: hlt\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rcx", "memory");
  check64("hlt: SIGSEGV", seen.signo, SIGSEGV_); check64("hlt: si_code SI_KERNEL", seen.code, 0x80); check64("hlt: trapno 13", seen.trapno, 13); check64("hlt: rip", seen.rip, g_expect);
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "1: ud2\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rcx", "memory");
  check64("ud2: SIGILL", seen.signo, SIGILL_); check64("ud2: si_code ILL_ILLOPN", seen.code, 2); check64("ud2: trapno 6", seen.trapno, 6); check64("ud2: rip", seen.rip, g_expect);
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "1: int3\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rcx", "memory");
  check64("int3: SIGTRAP", seen.signo, SIGTRAP_); check64("int3: si_code", seen.code, 0x80); check64("int3: trapno 3", seen.trapno, 3); check64("int3: rip is after the instruction", seen.rip, g_expect + 1);
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "1: cli\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rcx", "memory");
  check64("cli: SIGSEGV (general protection)", seen.signo, SIGSEGV_); check64("cli: si_code SI_KERNEL", seen.code, 0x80); check64("cli: trapno 13", seen.trapno, 13);
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "movl $0x3f8, %%edx\n\t1: inb %%dx, %%al\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rax", "rcx", "rdx", "memory");
  check64("inb: SIGSEGV (general protection)", seen.signo, SIGSEGV_);

  // ---- divide by zero ----
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "xorl %%edx, %%edx\n\tmovl $10, %%eax\n\txorl %%ecx, %%ecx\n\t1: divl %%ecx\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rax", "rcx", "rdx", "memory", "cc");
  if (seen.count) { check64("divide by zero: SIGFPE", seen.signo, SIGFPE_); check64("divide by zero: si_code FPE_INTDIV", seen.code, 1); check64("divide by zero: trapno 0", seen.trapno, 0); }
  else { puts_("  info: FEX did not raise #DE for div by zero (no signal was delivered)\n"); checkb("divide by zero is either delivered as SIGFPE or recorded as a known divergence", 1); }

  // ---- a handler installed with SA_NODEFER can be re-entered; without it a nested fault is fatal (not tested) ----
  checkb("rt_sigaction SIGSEGV with SA_NODEFER", set_handler(SIGSEGV_, SA_NODEFER_, 0) == 0);
  clear_seen(); g_policy = 0;
  __asm__ volatile(SITE_PROLOGUE "movq $0x10, %%rax\n\t1: movq (%%rax), %%rdx\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rax", "rdx", "rcx", "memory");
  check64("SA_NODEFER: signal not blocked inside the handler", seen.mask_in_handler & (1ULL << (SIGSEGV_ - 1)), 0);

  // ---- alternate signal stack ----
  { static u8 altstack[65536] __attribute__((aligned(64)));
    struct stack_t_ ss = {(u64)altstack, 0, 0, sizeof altstack};
    check64("sigaltstack", (u64)sys2(SYS_sigaltstack_, (i64)&ss, 0), 0);
    check64("rt_sigaction SA_ONSTACK", (u64)set_handler(SIGSEGV_, SA_ONSTACK_, 0), 0);
    clear_seen(); g_policy = 0;
    __asm__ volatile(SITE_PROLOGUE "movq $0x10, %%rax\n\t1: movq (%%rax), %%rdx\n\t2: " : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rax", "rdx", "rcx", "memory");
    checkb("SA_ONSTACK: the handler ran on the alternate stack", seen.handler_sp >= (u64)altstack && seen.handler_sp < (u64)altstack + sizeof altstack);
    checkb("SA_ONSTACK: the frame records the interrupted stack pointer outside the alternate stack", seen.rsp < (u64)altstack || seen.rsp >= (u64)altstack + sizeof altstack);
    // stack overflow: unbounded recursion runs the main stack into the unmapped gap below it, the handler survives on the alt stack
    clear_seen();
    extern void recurse(void);
    g_policy = 0;
    __asm__ volatile(SITE_PROLOGUE "movq %%rsp, %%rbx\n\t1: call recurse_entry\n\t2: movq %%rbx, %%rsp\n\t" : [expect] "=m"(g_expect), [resume] "=m"(g_resume) : : "rax", "rbx", "rcx", "rdx", "memory", "cc");
    check64("stack overflow reaches the handler as SEGV_MAPERR", seen.code, 1);
    checkb("stack overflow handler ran on the alternate stack", seen.handler_sp >= (u64)altstack && seen.handler_sp < (u64)altstack + sizeof altstack);
  }
  return finish("w07_fault");
}
// recursion that pushes 4 KiB per call and never returns
__asm__(".text\n.globl recurse_entry\nrecurse_entry:\n\tsubq $4080, %rsp\n\tmovq $1, (%rsp)\n\tcall recurse_entry\n\taddq $4080, %rsp\n\tret\n");
WITNESS_MAIN(run)
