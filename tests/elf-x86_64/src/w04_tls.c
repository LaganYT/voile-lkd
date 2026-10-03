// Witness (d): thread-local storage through arch_prctl(ARCH_SET_FS / ARCH_SET_GS) and %fs: / %gs: addressing.
#include "rt.h"

static u64 fs_block[64] __attribute__((aligned(64)));
static u64 gs_block[64] __attribute__((aligned(64)));
enum { ARCH_SET_GS = 0x1001, ARCH_SET_FS = 0x1002, ARCH_GET_FS = 0x1003, ARCH_GET_GS = 0x1004 };

static int run(void) {
  puts_("w04_tls: arch_prctl and segment-relative addressing\n");
  for (int i = 0; i < 64; i++) { fs_block[i] = 0x1000 + i; gs_block[i] = 0x2000 + i; }

  // Before ARCH_SET_FS the base is zero (the kernel gives a zero FS base to a new process).
  { u64 base = 1; check64("ARCH_GET_FS before set", (u64)sys2(SYS_arch_prctl, ARCH_GET_FS, (i64)&base), 0); check64("fs base initially 0", base, 0); }

  check64("ARCH_SET_FS returns 0", (u64)sys2(SYS_arch_prctl, ARCH_SET_FS, (i64)fs_block), 0);
  check64("ARCH_SET_GS returns 0", (u64)sys2(SYS_arch_prctl, ARCH_SET_GS, (i64)gs_block), 0);
  { u64 base = 0; sys2(SYS_arch_prctl, ARCH_GET_FS, (i64)&base); check64("ARCH_GET_FS reads back", base, (u64)fs_block); }
  { u64 base = 0; sys2(SYS_arch_prctl, ARCH_GET_GS, (i64)&base); check64("ARCH_GET_GS reads back", base, (u64)gs_block); }

  // Reads of every width.
  { u64 v; __asm__ volatile("movq %%fs:0x8, %0" : "=r"(v)); check64("fs:8 qword", v, 0x1001); }
  { u64 v; __asm__ volatile("movl %%fs:0x10, %k0" : "=r"(v)); check64("fs:0x10 dword", v, 0x1002); }
  { u64 v; __asm__ volatile("movzwl %%fs:0x18, %k0" : "=r"(v)); check64("fs:0x18 word", v, 0x1003); }
  { u64 v; __asm__ volatile("movzbl %%fs:0x20, %k0" : "=r"(v)); check64("fs:0x20 byte", v, 0x04 /* 0x1004 low byte */);}
  { u64 v; __asm__ volatile("movq %%gs:0x8, %0" : "=r"(v)); check64("gs:8 qword", v, 0x2001); }
  // Index and scale forms.
  { u64 i = 5, v; __asm__ volatile("movq %%fs:(,%1,8), %0" : "=r"(v) : "r"(i)); check64("fs:(,idx,8)", v, 0x1005); }
  { u64 i = 3, v; __asm__ volatile("movq %%fs:0x10(,%1,8), %0" : "=r"(v) : "r"(i)); check64("fs:0x10(,idx,8)", v, 0x1005); }
  { u64 b = 0x28, v; __asm__ volatile("movq %%fs:(%1), %0" : "=r"(v) : "r"(b)); check64("fs:(base reg)", v, 0x1005); }
  // Writes land in the block, through the segment base and through the plain pointer alike.
  { u64 x = 0x1122334455667788ULL; __asm__ volatile("movq %0, %%fs:0x30" : : "r"(x) : "memory"); check64("store via fs visible in memory", fs_block[6], 0x1122334455667788ULL); }
  { fs_block[7] = 0xa5a5a5a5a5a5a5a5ULL; u64 v; __asm__ volatile("movq %%fs:0x38, %0" : "=r"(v)); check64("store via memory visible via fs", v, 0xa5a5a5a5a5a5a5a5ULL); }
  { u64 v = 7; __asm__ volatile("addq $5, %%fs:0x40" : : : "memory"); __asm__ volatile("movq %%fs:0x40, %0" : "=r"(v)); check64("read-modify-write on fs memory", v, 0x1008 + 5); }
  { u64 old = 0x1009, nw = 0x7777, rax = old; __asm__ volatile("lock cmpxchgq %2, %%fs:0x48" : "+a"(rax) : "m"(fs_block[9]), "r"(nw) : "cc", "memory"); check64("lock cmpxchg on fs memory", fs_block[9], 0x7777); }
  // Self pointer idiom: glibc keeps the thread pointer at %fs:0.
  fs_block[0] = (u64)fs_block;
  { u64 tp; __asm__ volatile("movq %%fs:0, %0" : "=r"(tp)); check64("thread pointer idiom fs:0", tp, (u64)fs_block); }
  // Stack protector canary idiom.
  fs_block[5] = 0xdeadbeefcafef00dULL; // fs:0x28
  { u64 c1, c2; __asm__ volatile("movq %%fs:0x28, %0" : "=r"(c1)); c2 = c1; __asm__ volatile("xorq %%fs:0x28, %0" : "+r"(c2)); check64("canary xor is zero", c2, 0); check64("canary value", c1, 0xdeadbeefcafef00dULL); }
  // Changing the base takes effect immediately.
  { static u64 other[8] __attribute__((aligned(64))) = {0x9999, 0x8888};
    sys2(SYS_arch_prctl, ARCH_SET_FS, (i64)other); u64 v; __asm__ volatile("movq %%fs:8, %0" : "=r"(v)); check64("fs base switched", v, 0x8888);
    sys2(SYS_arch_prctl, ARCH_SET_FS, (i64)fs_block); __asm__ volatile("movq %%fs:8, %0" : "=r"(v)); check64("fs base restored", v, 0x1001); }
  // Segment selector reads: the 64-bit user selectors of Linux are cs=0x33, ss=0x2b, ds=es=fs=gs=0.
  { u64 cs = 0, ss = 0; __asm__ volatile("movw %%cs, %w0" : "=r"(cs)); __asm__ volatile("movw %%ss, %w0" : "=r"(ss)); puts_("  info: cs=0x"); put_hex(cs & 0xffff, 4); puts_(" ss=0x"); put_hex(ss & 0xffff, 4); puts_("\n"); }
  // rdfsbase / wrfsbase if CPUID.7.0:EBX.FSGSBASE
  { u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (b & 1) {
      u64 base; __asm__ volatile("rdfsbase %0" : "=r"(base)); check64("rdfsbase", base, (u64)fs_block);
      static u64 third[4] __attribute__((aligned(64))) = {0x5555};
      u64 nb = (u64)third; __asm__ volatile("wrfsbase %0" : : "r"(nb)); u64 v; __asm__ volatile("movq %%fs:0, %0" : "=r"(v)); check64("wrfsbase takes effect", v, 0x5555);
      u64 back = (u64)fs_block; __asm__ volatile("wrfsbase %0" : : "r"(back));
    } else puts_("  fsgsbase not advertised by cpuid: rdfsbase/wrfsbase skipped\n"); }
  return finish("w04_tls");
}
WITNESS_MAIN(run)
