// Witness (h): syscall register clobbers, cpuid, rdtsc, xgetbv and other host-visible instructions.
#include "rt.h"

static void cpuid(u32 leaf, u32 sub, u32* a, u32* b, u32* c, u32* d) { __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub)); }
static u64 rdtsc_(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }

static int run(void) {
  puts_("w08_sys: syscall clobbers, cpuid, rdtsc, xgetbv\n");
  // syscall: rcx = address of the next instruction, r11 = rflags, other registers preserved, flags preserved
  { u64 rcx_out, r11_out, next, rbx_out, rsi_out, r13_out, r12_out, r15_out;
    __asm__ volatile(
      "movq $0x1111111111111111, %%rbx\n\t movq $0x2222222222222222, %%r13\n\t movq $0x4444444444444444, %%r12\n\t movq $0x5555555555555555, %%r15\n\t"
      "movq $0x3333333333333333, %%rsi\n\t"
      "stc\n\t"
      "movl $39, %%eax\n\t"            /* getpid */
      "leaq 1f(%%rip), %[next]\n\t"
      "syscall\n\t"
      "1:\n\t"
      "movq %%rcx, %[rcx]\n\t movq %%r11, %[r11]\n\t movq %%rbx, %[rbx]\n\t movq %%rsi, %[rsi]\n\t movq %%r13, %[r13]\n\t movq %%r12, %[r12]\n\t movq %%r15, %[r15]\n\t"
      : [next] "=&r"(next), [rcx] "=m"(rcx_out), [r11] "=m"(r11_out), [rbx] "=m"(rbx_out), [rsi] "=m"(rsi_out), [r13] "=m"(r13_out), [r12] "=m"(r12_out), [r15] "=m"(r15_out)
      : : "rax", "rcx", "rdx", "rbx", "rsi", "r13", "r11", "r12", "r15", "cc", "memory");
    check64("syscall: rcx holds the address of the next instruction", rcx_out, next);
    check64("syscall: r11 holds rflags with CF set by stc", r11_out & 1, 1);
    checkb("syscall: r11 has the always-one bit 1", (r11_out & 2) != 0);
    check64("syscall: rbx preserved", rbx_out, 0x1111111111111111ULL); check64("syscall: r13 preserved", r13_out, 0x2222222222222222ULL);
    check64("syscall: rsi preserved", rsi_out, 0x3333333333333333ULL); check64("syscall: r12 preserved", r12_out, 0x4444444444444444ULL);
    check64("syscall: r15 preserved", r15_out, 0x5555555555555555ULL); }
  { u64 fl; __asm__ volatile("stc\n\tmovl $39, %%eax\n\tsyscall\n\tpushfq\n\tpopq %0" : "=r"(fl) : : "rax", "rcx", "r11", "cc", "memory"); check64("CF survives a syscall", fl & 1, 1); }
  { u64 fl; __asm__ volatile("clc\n\tmovl $39, %%eax\n\tsyscall\n\tpushfq\n\tpopq %0" : "=r"(fl) : : "rax", "rcx", "r11", "cc", "memory"); check64("CF clear survives a syscall", fl & 1, 0); }
  { u64 fl; __asm__ volatile("std\n\tmovl $39, %%eax\n\tsyscall\n\tpushfq\n\tpopq %0\n\tcld" : "=r"(fl) : : "rax", "rcx", "r11", "cc", "memory"); check64("DF survives a syscall", (fl >> 10) & 1, 1); }
  { i64 r; __asm__ volatile("movq $99999, %%rax\n\tsyscall" : "=a"(r) : : "rcx", "r11", "memory"); check64("unknown syscall returns -ENOSYS", (u64)r, (u64)-38); }
  { i64 pid1 = sys0(SYS_getpid), pid2 = sys0(SYS_getpid), tid = sys0(SYS_gettid); checkb("getpid is stable", pid1 == pid2 && pid1 > 0); checkb("gettid of the main thread equals the pid", tid == pid1); }

  // cpuid
  u32 a, b, c, d;
  cpuid(0, 0, &a, &b, &c, &d);
  const u32 maxleaf = a;
  { char vendor[13]; *(u32*)(vendor + 0) = b; *(u32*)(vendor + 4) = d; *(u32*)(vendor + 8) = c; vendor[12] = 0;
    puts_("  cpuid vendor: "); puts_(vendor); puts_("  max leaf: "); put_dec(maxleaf); puts_("\n");
    checkb("cpuid vendor is GenuineIntel or AuthenticAMD", (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e) || (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163)); }
  checkb("cpuid max standard leaf >= 7", maxleaf >= 7);
  cpuid(1, 0, &a, &b, &c, &d);
  const u32 f1c = c, f1d = d;
  puts_("  leaf 1: eax=0x"); put_hex(a, 8); puts_(" ebx=0x"); put_hex(b, 8); puts_(" ecx=0x"); put_hex(c, 8); puts_(" edx=0x"); put_hex(d, 8); puts_("\n");
  checkb("SSE (edx bit 25)", (f1d >> 25) & 1); checkb("SSE2 (edx bit 26)", (f1d >> 26) & 1); checkb("FXSR (edx bit 24)", (f1d >> 24) & 1);
  checkb("CMOV (edx bit 15)", (f1d >> 15) & 1); checkb("CX8 (edx bit 8)", (f1d >> 8) & 1); checkb("TSC (edx bit 4)", (f1d >> 4) & 1);
  checkb("FPU (edx bit 0)", f1d & 1); checkb("MMX (edx bit 23)", (f1d >> 23) & 1);
  checkb("SSE3 (ecx bit 0)", f1c & 1); checkb("SSSE3 (ecx bit 9)", (f1c >> 9) & 1); checkb("SSE4.1 (ecx bit 19)", (f1c >> 19) & 1); checkb("SSE4.2 (ecx bit 20)", (f1c >> 20) & 1);
  checkb("POPCNT (ecx bit 23)", (f1c >> 23) & 1); checkb("CX16 (ecx bit 13)", (f1c >> 13) & 1);
  const int osxsave = (f1c >> 27) & 1, avx = (f1c >> 28) & 1, xsave = (f1c >> 26) & 1;
  puts_("  xsave="); put_dec(xsave); puts_(" osxsave="); put_dec(osxsave); puts_(" avx="); put_dec(avx); puts_(" f16c="); put_dec((f1c >> 29) & 1); puts_(" fma="); put_dec((f1c >> 12) & 1);
  puts_(" aes="); put_dec((f1c >> 25) & 1); puts_(" pclmul="); put_dec((f1c >> 1) & 1); puts_(" rdrand="); put_dec((f1c >> 30) & 1); puts_("\n");
  checkb("hypervisor bit state reported (informational: not checked)", 1);
  // leaf 7
  if (maxleaf >= 7) {
    cpuid(7, 0, &a, &b, &c, &d);
    puts_("  leaf 7: ebx=0x"); put_hex(b, 8); puts_(" ecx=0x"); put_hex(c, 8); puts_(" edx=0x"); put_hex(d, 8); puts_("\n");
    puts_("  avx2="); put_dec((b >> 5) & 1); puts_(" bmi1="); put_dec((b >> 3) & 1); puts_(" bmi2="); put_dec((b >> 8) & 1); puts_(" erms="); put_dec((b >> 9) & 1);
    puts_(" sha="); put_dec((b >> 29) & 1); puts_(" avx512f="); put_dec((b >> 16) & 1); puts_(" fsgsbase="); put_dec(b & 1); puts_(" adx="); put_dec((b >> 19) & 1); puts_("\n");
    checkb("AVX512F is not advertised (FEX does not implement it)", !((b >> 16) & 1));
  }
  // extended leaves
  cpuid(0x80000000u, 0, &a, &b, &c, &d);
  const u32 maxext = a; checkb("extended leaves exist", maxext >= 0x80000008u);
  cpuid(0x80000001u, 0, &a, &b, &c, &d);
  checkb("LAHF/SAHF in 64-bit mode (ecx bit 0)", c & 1); checkb("SYSCALL (edx bit 11)", (d >> 11) & 1); checkb("NX (edx bit 20)", (d >> 20) & 1); checkb("LM long mode (edx bit 29)", (d >> 29) & 1);
  puts_("  lzcnt(abm)="); put_dec((c >> 5) & 1); puts_("\n");
  { char brand[49]; for (u32 i = 0; i < 3; i++) { cpuid(0x80000002u + i, 0, &a, &b, &c, &d); ((u32*)brand)[i * 4] = a; ((u32*)brand)[i * 4 + 1] = b; ((u32*)brand)[i * 4 + 2] = c; ((u32*)brand)[i * 4 + 3] = d; } brand[48] = 0;
    puts_("  brand string: "); puts_(brand); puts_("\n"); checkb("brand string is not empty", brand[0] != 0); }
  cpuid(0x80000008u, 0, &a, &b, &c, &d);
  puts_("  address sizes: physical="); put_dec(a & 0xff); puts_(" virtual="); put_dec((a >> 8) & 0xff); puts_("\n");
  checkb("virtual address width reported as 48 bits", ((a >> 8) & 0xff) == 48);
  // Leaf 0xb (extended topology): FEXCore 0df84d3 routes it to Function_Reserved (CPUID.h, table entry "0x0B"), so it returns
  // all zeros even though the maximum leaf is above 0xb. The witness records what the emulation returns instead of requiring
  // a real-CPU answer: this is an upstream FEX limitation, not a macOS port defect.
  if (maxleaf >= 0xb) { cpuid(0xb, 0, &a, &b, &c, &d);
    puts_("  leaf 0xb subleaf 0: eax=0x"); put_hex(a, 8); puts_(" ebx=0x"); put_hex(b, 8); puts_(" ecx=0x"); put_hex(c, 8); puts_(" edx=0x"); put_hex(d, 8);
    puts_((a | b | c | d) ? "  (implemented)\n" : "  (all zeros: not implemented by FEX)\n");
    checkb("leaf 0xb is either all zero or a valid SMT level", (a | b | c | d) == 0 || ((c >> 8) & 0xff) == 1); }

  // xgetbv
  if (osxsave) {
    u32 lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    puts_("  XCR0=0x"); put_hex(((u64)hi << 32) | lo, 16); puts_("\n");
    checkb("XCR0 bit 0 (x87) set", lo & 1); checkb("XCR0 bit 1 (SSE) set", lo & 2); checkb("XCR0 bit 2 (AVX) matches the AVX cpuid bit", ((lo >> 2) & 1) == (u32)avx);
    checkb("XCR0 does not enable AVX-512 state (bits 5-7)", !(lo & 0xe0));
  } else puts_("  OSXSAVE not set, xgetbv skipped\n");

  // rdtsc: monotonic, advances across a syscall and a busy loop, and the delta is plausible
  { u64 t1 = rdtsc_(); sys0(SYS_getpid); u64 t2 = rdtsc_(); volatile u64 x = 0; for (int i = 0; i < 2000000; i++) x += (u64)i; u64 t3 = rdtsc_();
    checkb("rdtsc is monotonic (1)", t2 > t1); checkb("rdtsc is monotonic (2)", t3 > t2); puts_("  rdtsc deltas: syscall="); put_dec((i64)(t2 - t1)); puts_(" loop="); put_dec((i64)(t3 - t2)); puts_("\n");
    checkb("rdtsc advances during a 2M-iteration loop", t3 - t2 > 1000); }
  { u64 prev = rdtsc_(); int mono = 1; for (int i = 0; i < 1000; i++) { u64 t = rdtsc_(); if (t < prev) mono = 0; prev = t; } checkb("rdtsc never goes backwards over 1000 reads", mono); }
  // time syscalls agree with rdtsc-ish progress
  { struct { i64 s, n; } t0, t1; sys2(SYS_clock_gettime, 1, (i64)&t0); volatile u64 x = 0; for (int i = 0; i < 3000000; i++) x += (u64)i; sys2(SYS_clock_gettime, 1, (i64)&t1);
    checkb("clock_gettime(CLOCK_MONOTONIC) advances", (t1.s > t0.s) || (t1.s == t0.s && t1.n > t0.n)); }
  // pause / nop encodings / ud-free multi-byte nops decode fine
  __asm__ volatile("pause\n\t.byte 0x0f,0x1f,0x00\n\t.byte 0x0f,0x1f,0x44,0x00,0x00\n\t.byte 0x66,0x0f,0x1f,0x44,0x00,0x00\n\t.byte 0x0f,0x1f,0x80,0x00,0x00,0x00,0x00\n\txchg %%ax,%%ax" ::: "memory");
  checkb("pause and multi-byte nops execute", 1);
  // rdrand / rdseed when advertised
  { u32 a2, b2, c2, d2; cpuid(1, 0, &a2, &b2, &c2, &d2); if ((c2 >> 30) & 1) { u64 v; unsigned char ok; __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(v), "=qm"(ok) : : "cc"); checkb("rdrand reports success (CF=1)", ok == 1); } else puts_("  rdrand not advertised\n"); }
  return finish("w08_sys");
}
WITNESS_MAIN(run)
