// Witness: code that keeps writing the host page it runs from (ELDEN RING's EAC launcher module). A store storm moves
// the page from write tracking to per-instruction checks (fexdarwin CodeValidation, FEXCore NeedsCodeValidation). Every
// way of changing the code afterwards must still take effect: rewrites between calls, a store into the running block,
// a hot loop that patches its callee, an instruction that starts in a tracked page and ends in the switched one, code
// written by the kernel (read from a pipe), and a fetch from the page once it is no longer executable (SIGSEGV with
// si_addr, never a retranslation loop). Expected values are what the bytes give on real hardware.
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20 };
enum { SYS_pipe2_ = 293 };
#define K16 16384UL
#define SIGSEGV_ 11
#define SA_SIGINFO_ 4
#define SA_RESTORER_ 0x04000000
typedef i64 (*fn_t)(void);

struct ksigaction { u64 handler, flags, restorer, mask; };
struct stack_t_ { u64 sp; i32 flags; i32 pad; u64 size; };
struct sigcontext_ { u64 r8, r9, r10, r11, r12, r13, r14, r15, rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags; u16 cs, gs, fs, ss; u64 err, trapno, oldmask, cr2, fpstate, reserved[8]; };
struct ucontext_ { u64 flags, link; struct stack_t_ stack; struct sigcontext_ mc; u64 sigmask; };
struct siginfo_ { i32 signo, errno_, code, pad; u64 addr; u8 rest[104]; };

static volatile struct { int count; u64 addr, rip; } seen;
static volatile u64 g_resume;
static void handler(int sig, struct siginfo_* si, struct ucontext_* uc) {
  (void)sig;
  seen.count++;
  seen.addr = si->addr;
  seen.rip = uc->mc.rip;
  uc->mc.rip = g_resume;
}
__asm__(".text\n.globl restorer_rt\nrestorer_rt:\n\tmovq $15, %rax\n\tsyscall\n\thlt\n");
extern void restorer_rt(void);

static u8* map_rwx(u64 len) { i64 r = sys6(SYS_mmap, 0, (i64)len, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); return (r < 0 && r > -4096) ? 0 : (u8*)r; }
static void put32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
// mov eax, imm32 ; ret
static void emit_ret_imm(u8* p, u32 v) { p[0] = 0xb8; put32(p + 1, v); p[5] = 0xc3; }
static u64 now_ns(void) { i64 ts[2]; sys2(SYS_clock_gettime, 1, (i64)ts); return (u64)ts[0] * 1000000000ULL + (u64)ts[1]; }

static int run(void) {
  puts_("w28_smcstorm: code that keeps writing the page it runs from\n");
  u8* p = map_rwx(2 * K16);
  checkb("RWX mmap of two host pages", p != 0);
  if (!p) return finish("w28_smcstorm");
  u8* b = p + K16; // the page of the storm

  // 0. The storm: a store into the code's own host page before each call, 20000 times.
  u8* fn = b + 0x100;
  emit_ret_imm(fn, 7);
  volatile u64* data = (volatile u64*)(b + 0x2000);
  u64 sum = 0, t0 = now_ns();
  for (u32 i = 0; i < 20000; i++) { *data = i; sum += (u64)((fn_t)fn)(); }
  u64 ms = (now_ns() - t0) / 1000000;
  check64("storm: 20000 calls return the code's value", sum, 20000ULL * 7);
  check64("storm: the last store landed", *data, 19999);
  puts_("info storm took "); put_dec((i64)ms); puts_(" ms\n");

  // 1. Rewrites between calls.
  emit_ret_imm(fn, 8);
  check64("rewrite between calls after the storm", (u64)((fn_t)fn)(), 8);
  { int ok = 1; for (u32 i = 0; i < 500; i++) { *data = i; emit_ret_imm(fn, 3000 + i); if ((u32)((fn_t)fn)() != 3000 + i) ok = 0; }
    checkb("500 rewrite/execute rounds all return the new value", ok); }
  { u8 c[] = {0x48, 0xc7, 0xc0, 0x07, 0x00, 0x00, 0x00, 0x48, 0x83, 0xc0, 0x05, 0xc3}; for (u32 i = 0; i < sizeof c; i++) fn[i] = c[i]; /* mov rax,7 ; add rax,5 ; ret */
    check64("different instructions at the same address", (u64)((fn_t)fn)(), 12); }

  // 2. A store into the next instruction of the running block.
  { u8* q = b + 0x400;
    // movabs rax, <q+14> ; mov byte [rax], 0x2b ; mov eax, 0x11 ; ret
    q[0] = 0x48; q[1] = 0xb8; u64 target = (u64)(q + 14); for (int i = 0; i < 8; i++) q[2 + i] = (u8)(target >> (8 * i));
    q[10] = 0xc6; q[11] = 0x00; q[12] = 0x2b;
    q[13] = 0xb8; q[14] = 0x11; q[15] = 0; q[16] = 0; q[17] = 0; q[18] = 0xc3;
    check64("store into the next instruction of the running block", (u64)((fn_t)q)(), 0x2b);
    q[14] = 0x11; check64("same self-patching code, second run", (u64)((fn_t)q)(), 0x2b); }

  // 3. A hot loop that patches its callee halfway: 51 calls return 1, then 49 return 2.
  { u8* c = b + 0x800; u8* f2 = b + 0x900; emit_ret_imm(f2, 1);
    u32 n = 0;
    c[n++] = 0x53;                                                              /* push rbx (callee-saved) */
    c[n++] = 0x48; c[n++] = 0x31; c[n++] = 0xdb;                                /* xor rbx, rbx */
    c[n++] = 0xb9; put32(c + n, 100); n += 4;                                  /* mov ecx, 100 */
    u32 loop = n;
    c[n++] = 0x51;                                                              /* push rcx */
    c[n++] = 0xe8; put32(c + n, (u32)((i32)((f2 - c) - (n + 4)))); n += 4;     /* call f2 */
    c[n++] = 0x59;                                                              /* pop rcx */
    c[n++] = 0x48; c[n++] = 0x01; c[n++] = 0xc3;                               /* add rbx, rax */
    c[n++] = 0x83; c[n++] = 0xf9; c[n++] = 50;                                 /* cmp ecx, 50 */
    c[n++] = 0x75; u32 jne = n++;                                               /* jne skip */
    c[n++] = 0x48; c[n++] = 0xba; u64 imm = (u64)(f2 + 1); for (int i = 0; i < 8; i++) c[n++] = (u8)(imm >> (8 * i)); /* movabs rdx, f2+1 */
    c[n++] = 0xc6; c[n++] = 0x02; c[n++] = 0x02;                               /* mov byte [rdx], 2 */
    c[jne] = (u8)(n - (jne + 1));
    c[n++] = 0xe2; c[n] = (u8)(i8)(loop - (n + 1)); n++;                        /* loop L */
    c[n++] = 0x48; c[n++] = 0x89; c[n++] = 0xd8;                               /* mov rax, rbx */
    c[n++] = 0x5b;                                                              /* pop rbx */
    c[n++] = 0xc3;
    check64("hot loop that patches its callee halfway", (u64)((fn_t)c)(), 51 + 49 * 2);
    emit_ret_imm(f2, 1);
    check64("same loop again", (u64)((fn_t)c)(), 51 + 49 * 2); }

  // 4. An instruction that starts in the tracked page and ends in the storm page: mov eax, imm32 at K16 - 3.
  { u8* x = b - 3; x[0] = 0xb8; put32(x + 1, 0x11223344); x[5] = 0xc3;
    check64("instruction across the two host pages", (u64)((fn_t)x)(), 0x11223344);
    x[4] = 0x55; /* high byte of the immediate, in the storm page */
    check64("its immediate changed in the storm page", (u64)((fn_t)x)(), 0x55223344);
    x[1] = 0x66; /* low byte, in the tracked page */
    check64("its immediate changed in the tracked page", (u64)((fn_t)x)(), 0x55223366); }

  // 5. Code written by the kernel: read() from a pipe over code that already ran.
  { u8* k = b + 0x600; emit_ret_imm(k, 70);
    check64("code before the kernel writes it", (u64)((fn_t)k)(), 70);
    i32 fds[2] = {-1, -1};
    check64("pipe2", (u64)sys2(SYS_pipe2_, (i64)fds, 0), 0);
    u8 code[6]; emit_ret_imm(code, 77);
    check64("write the new code into the pipe", (u64)sys3(SYS_write, fds[1], (i64)code, 6), 6);
    check64("read it over the old code", (u64)sys3(SYS_read, fds[0], (i64)k, 6), 6);
    check64("the code the kernel wrote runs", (u64)((fn_t)k)(), 77); }

  // 6. The page made non-executable: a call into it faults with si_addr = the target (no retranslation loop).
  { struct ksigaction sa = {(u64)handler, SA_SIGINFO_ | SA_RESTORER_, (u64)restorer_rt, 0};
    check64("rt_sigaction SIGSEGV", (u64)sys4(SYS_rt_sigaction, SIGSEGV_, (i64)&sa, 0, 8), 0);
    check64("mprotect the storm page RW", (u64)sys3(SYS_mprotect, (i64)b, K16, PROT_READ | PROT_WRITE), 0);
    seen.count = 0;
    __asm__ volatile(
      "leaq 1f(%%rip), %%rcx\n\t"
      "movq %%rcx, %0\n\t"
      "movq %%rsp, %%rbx\n\t"
      "call *%1\n\t"
      "1: movq %%rbx, %%rsp\n\t"
      : "=m"(g_resume) : "r"(fn) : "rax", "rbx", "rcx", "rdx", "memory", "cc");
    check64("call into the non-executable page: one SIGSEGV", seen.count, 1);
    check64("si_addr is the target", seen.addr, (u64)fn);
    check64("rip is the target", seen.rip, (u64)fn);
    check64("mprotect it RWX again", (u64)sys3(SYS_mprotect, (i64)b, K16, PROT_READ | PROT_WRITE | PROT_EXEC), 0);
    emit_ret_imm(fn, 9);
    check64("and its code runs again", (u64)((fn_t)fn)(), 9); }

  // 7. The other page still tracks writes: rewrite code there between calls.
  { u8* a = p + 0x100; int ok = 1;
    for (u32 i = 0; i < 50; i++) { emit_ret_imm(a, 600 + i); if ((u32)((fn_t)a)() != 600 + i) ok = 0; }
    checkb("50 rewrites in the other page", ok); }
  return finish("w28_smcstorm");
}
WITNESS_MAIN(run)
