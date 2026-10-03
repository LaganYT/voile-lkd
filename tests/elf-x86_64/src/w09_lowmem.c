// Witness (j): guest addresses below 4 GiB. A native arm64 macOS process cannot map host memory there, so these
// accesses only work when the host runs the guest through a guest base offset (FEXDARWIN_GUEST_BASE).
// Covers plain loads/stores, push/pop and call/ret on a low stack, atomics, rep movs/stos, and code executed from a low
// page, at the addresses Wine needs (0x7ffe0000 for USER_SHARED_DATA, 0x10000 for the DOS area).
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20 };
#define K16 16384UL

static i64 mmap_(u64 addr, u64 len, int prot, int flags) { return sys6(SYS_mmap, (i64)addr, (i64)len, prot, flags, -1, 0); }

static int run(void) {
  puts_("w09_lowmem: guest memory below 4 GiB\n");

  // USER_SHARED_DATA page of Wine.
  i64 usd = mmap_(0x7ffe0000, K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
  check64("mmap MAP_FIXED at 0x7ffe0000", (u64)usd, 0x7ffe0000);
  volatile u32* tick = (volatile u32*)0x7ffe0320;
  *tick = 0xabcd1234;
  check64("store then load at 0x7ffe0320", *tick, 0xabcd1234);
  check64("absolute disp32 load at 0x7ffe0320", ({ u32 v; __asm__ volatile("movl 0x7ffe0320, %0" : "=r"(v)); v; }), 0xabcd1234);

  // DOS area.
  i64 dos = mmap_(0x10000, 4 * K16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
  check64("mmap MAP_FIXED at 0x10000", (u64)dos, 0x10000);
  {
    u64* q = (u64*)0x10000;
    for (u64 i = 0; i < 4 * K16 / 8; i++) q[i] = i ^ 0x5555;
    u64 s = 0, w = 0;
    for (u64 i = 0; i < 4 * K16 / 8; i++) { s += q[i]; w += i ^ 0x5555; }
    check64("fill and sum 64 KiB at 0x10000", s, w);
  }

  // Atomics on a low address.
  {
    volatile u64* a = (volatile u64*)0x10008;
    *a = 5;
    u64 old = 7;
    __asm__ volatile("lock xaddq %0, %1" : "+r"(old), "+m"(*a) : : "memory");
    check64("lock xadd returns the previous value", old, 5);
    check64("lock xadd stored the sum", *a, 12);
    u64 expect = 12, desired = 99;
    __asm__ volatile("lock cmpxchgq %2, %1" : "+a"(expect), "+m"(*a) : "r"(desired) : "memory", "cc");
    check64("lock cmpxchg succeeded", *a, 99);
  }

  // rep stos / rep movs.
  {
    u8* dst = (u8*)0x12000;
    u64 n = 777;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(0x3c) : "memory");
    for (u64 i = 0; i < 1024; i++) ((volatile u8*)0x13000)[i] = 0; // the 64 KiB fill above wrote here
    u8* src = (u8*)0x12000;
    u8* d2 = (u8*)0x13000;
    n = 777;
    __asm__ volatile("rep movsb" : "+S"(src), "+D"(d2), "+c"(n) : : "memory");
    check64("rep movsb copied from rep stosb", ((volatile u8*)0x13000)[776], 0x3c);
    check64("rep movsb did not overrun", ((volatile u8*)0x13000)[777], 0);
  }

  // Code at a low address: mov eax, 0x1234 ; ret.
  {
    u8* code = (u8*)0x14000;
    code[0] = 0xb8; code[1] = 0x34; code[2] = 0x12; code[3] = 0; code[4] = 0; code[5] = 0xc3;
    u32 r = ((u32(*)(void))code)();
    check64("call into code at 0x14000", r, 0x1234);
  }

  // A stack at a low address: push, pop, call and ret through it.
  {
    u64 out;
    __asm__ volatile(
      "movq %%rsp, %%r12\n\t"
      "movq $0x1c000, %%rsp\n\t"
      "pushq $0x11\n\t"
      "pushq $0x22\n\t"
      "popq %%rax\n\t"
      "popq %%rbx\n\t"
      "shlq $8, %%rbx\n\t"
      "orq %%rbx, %%rax\n\t"
      "call 1f\n\t"
      "jmp 2f\n\t"
      "1: addq $0x100, %%rax\n\t"
      "ret\n\t"
      "2: movq %%r12, %%rsp\n\t"
      : "=a"(out) : : "rbx", "r12", "memory", "cc");
    check64("push/pop/call/ret on a stack at 0x1c000", out, 0x1222);
  }

  // Rarer memory forms at low addresses: non-temporal store/load, prefetch, clflush, cmpxchg16b, fxsave/fxrstor, xsave,
  // maskmovdqu. Each is translated by a different JIT path.
  {
    u8* lo = (u8*)0x16000;
    for (int i = 0; i < 256; i++) lo[i] = (u8)i;
    u64 a[2];
    __asm__ volatile("movdqu (%1), %%xmm1\n\tmovntdq %%xmm1, 64(%1)\n\tsfence\n\tmovntdqa 64(%1), %%xmm2\n\tmovdqu %%xmm2, %0"
                     : "=m"(a) : "r"(lo) : "xmm1", "xmm2", "memory");
    check64("movntdq/movntdqa round trip low", a[0], 0x0706050403020100ull);
    __asm__ volatile("prefetcht0 (%0)\n\tprefetchnta 128(%0)\n\tclflush 64(%0)\n\tmfence" : : "r"(lo) : "memory");
    check64("byte readable after clflush", lo[65], 1);
    volatile u64* pair = (volatile u64*)0x16100; // 16-byte aligned
    pair[0] = 1; pair[1] = 2;
    u64 lo_e = 1, hi_e = 2, lo_n = 0x11, hi_n = 0x22;
    u8 ok;
    __asm__ volatile("lock cmpxchg16b %1\n\tsete %0" : "=q"(ok), "+m"(*(volatile __int128*)pair), "+a"(lo_e), "+d"(hi_e) : "b"(lo_n), "c"(hi_n) : "memory", "cc");
    check64("cmpxchg16b at 0x16100", ok ? pair[0] + (pair[1] << 8) : 0, 0x2211);
    u8* area = (u8*)0x17000; // 512-byte fxsave area, 16-aligned
    double d = 2.5;
    __asm__ volatile("fldl %0" : : "m"(d));
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
    __asm__ volatile("fstp %%st(0)\n\tfxrstor (%0)\n\tfstpl %1" : : "r"(area), "m"(d) : "memory");
    check64("fxsave then fxrstor restores st0 (2.5)", *(volatile u64*)&d, 0x4004000000000000ull);
    check64("fxsave stored fcw", *(volatile u16*)area, 0x37f);
    u8* xarea = (u8*)0x18000; // 64-aligned xsave area
    for (int i = 0; i < 1024; i++) xarea[i] = 0;
    __asm__ volatile("movdqu (%0), %%xmm3" : : "r"(lo) : "xmm3");
    __asm__ volatile("xsave (%0)" : : "r"(xarea), "a"(7), "d"(0) : "memory");
    check64("xsave stored xmm3 low", *(volatile u64*)(xarea + 160 + 3 * 16), 0x0706050403020100ull);
    u8* mdst = (u8*)0x16200;
    for (int i = 0; i < 16; i++) mdst[i] = 0xee;
    __asm__ volatile("movdqu (%0), %%xmm4\n\tpcmpeqb %%xmm5, %%xmm5\n\tpsrlw $8, %%xmm5\n\tmovq %1, %%rdi\n\tmaskmovdqu %%xmm5, %%xmm4"
                     : : "r"(lo), "r"(mdst) : "rdi", "xmm4", "xmm5", "memory");
    check64("maskmovdqu writes only masked bytes", *(volatile u64*)mdst, 0xee06ee04ee02ee00ull);
  }
  return finish("w09_lowmem");
}
WITNESS_MAIN(run)
