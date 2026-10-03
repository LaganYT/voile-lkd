// Witness (r): fork() while another thread keeps generating new translated code (self-modifying code in a loop), as
// in Wine where some threads compile while another forks. Every child must run and exit 7.
#include "rt.h"
#define SYS_fork_ 57
#define SYS_wait4_ 61
#define ROUNDS 120
enum { PROT_RWX = 7, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20 };
#define CLONE_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000)

static volatile i32 g_stop, g_ctid;
static u8* g_code;

static void churn(u64 arg) {
  (void)arg;
  u32 k = 1;
  while (!g_stop) {
    // mov eax, imm32 ; ret  -- a new immediate each time, so the block is re-translated
    g_code[0] = 0xb8; *(volatile u32*)(g_code + 1) = k; g_code[5] = 0xc3;
    u32 (*fn)(void) = (u32 (*)(void))g_code;
    if (fn() != k) { g_stop = 2; break; }
    k++;
  }
}

static i64 spawn(void (*fn)(u64), u64 stack_top, volatile i32* ctid) {
  u64* sp = (u64*)stack_top; *--sp = 0; *--sp = (u64)fn;
  i64 ret; register i64 r10 __asm__("r10") = (i64)ctid; register i64 r8 __asm__("r8") = 0;
  __asm__ volatile("syscall\n\ttestq %%rax, %%rax\n\tjnz 1f\n\tpopq %%rax\n\tpopq %%rdi\n\tandq $-16, %%rsp\n\t"
                   "call *%%rax\n\tmovl $60, %%eax\n\txorl %%edi, %%edi\n\tsyscall\n\thlt\n1:\n\t"
                   : "=a"(ret) : "a"((i64)SYS_clone), "D"((i64)(CLONE_FLAGS & ~0x80000)), "S"((i64)sp), "d"((i64)ctid), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return ret;
}

static u64 work(u64 n) { u64 a = 1; for (u64 i = 0; i < n; i++) a = a * 6364136223846793005ull + i; return a; }

static int run(void) {
  puts_("w18_forkjit: fork while another thread generates code\n");
  g_code = (u8*)sys6(SYS_mmap, 0, 4096, PROT_RWX, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  u64 stack = (u64)sys6(SYS_mmap, 0, 256 * 1024, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  spawn(churn, stack + 256 * 1024, &g_ctid);
  u64 ref = work(5000);
  int ok = 0;
  for (int r = 0; r < ROUNDS && g_stop == 0; r++) {
    i64 pid = sys0(SYS_fork_);
    if (pid == 0) exit_group(work(5000) == ref ? 7 : 9);
    int st = 0;
    if (sys4(SYS_wait4_, pid, (i64)&st, 0, 0) == pid && (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 7) ok++;
    else { puts_("round "); put_dec(r); puts_(" status "); put_hex((u64)st, 8); puts_("\n"); }
  }
  i32 churn_state = g_stop;
  g_stop = 1;
  for (;;) { i32 v = g_ctid; if (!v) break; sys6(SYS_futex, (i64)&g_ctid, 0, v, 0, 0, 0); }
  check64("churn thread saw correct results", (u64)churn_state, 0);
  check64("every child exited 7", (u64)ok, ROUNDS);
  return finish("w18_forkjit");
}
WITNESS_MAIN(run)
