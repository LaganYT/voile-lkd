// Witness (s): a long series of short-lived threads (one at a time), as games create and end worker threads all the
// time. Every thread must start and finish; the host must free each thread's resources (regression for request wine-9).
#include "rt.h"
enum { MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20 };
#define CLONE_FLAGS (0x100 | 0x200 | 0x400 | 0x800 | 0x10000 | 0x40000 | 0x100000 | 0x200000)
#define ROUNDS 1500
static volatile i32 g_ctid;
static volatile u64 g_sum;
static void body(u64 arg) { u64 a = arg; for (int i = 0; i < 2000; i++) a = a * 6364136223846793005ull + 1; __atomic_add_fetch(&g_sum, a & 1, __ATOMIC_SEQ_CST); __atomic_add_fetch(&g_sum, 2, __ATOMIC_SEQ_CST); }
static i64 spawn(void (*fn)(u64), u64 arg, u64 stack_top, volatile i32* ctid) {
  u64* sp = (u64*)stack_top; *--sp = arg; *--sp = (u64)fn;
  i64 ret; register i64 r10 __asm__("r10") = (i64)ctid; register i64 r8 __asm__("r8") = 0;
  __asm__ volatile("syscall\n\ttestq %%rax, %%rax\n\tjnz 1f\n\tpopq %%rax\n\tpopq %%rdi\n\tandq $-16, %%rsp\n\t"
                   "call *%%rax\n\tmovl $60, %%eax\n\txorl %%edi, %%edi\n\tsyscall\n\thlt\n1:\n\t"
                   : "=a"(ret) : "a"((i64)SYS_clone), "D"((i64)(CLONE_FLAGS & ~0x80000)), "S"((i64)sp), "d"((i64)ctid), "r"(r10), "r"(r8)
                   : "rcx", "r11", "memory");
  return ret;
}
static int run(void) {
  puts_("w21_threadchurn: 1500 short-lived threads in sequence\n");
  u64 stack = (u64)sys6(SYS_mmap, 0, 128 * 1024, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int started = 0;
  for (int r = 0; r < ROUNDS; r++) {
    g_ctid = 1;
    if (spawn(body, (u64)r, stack + 128 * 1024, &g_ctid) <= 0) break;
    started++;
    for (;;) { i32 v = g_ctid; if (!v) break; sys6(SYS_futex, (i64)&g_ctid, 0, v, 0, 0, 0); }
  }
  check64("all threads started", (u64)started, ROUNDS);
  checkb("every thread ran its body", g_sum >= 2ull * ROUNDS);
  return finish("w21_threadchurn");
}
WITNESS_MAIN(run)
