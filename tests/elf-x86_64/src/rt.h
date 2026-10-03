// Freestanding runtime for the witnesses: raw x86-64 Linux system calls, no libc.
// With -DWITNESS_HOSTED the same test code builds as a macOS x86_64 program (run under Rosetta 2 as an independent
// oracle for the expected values); only the output and exit primitives differ.
#pragma once
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;
typedef signed char i8;

#ifdef WITNESS_HOSTED
#include <stdlib.h>
#include <unistd.h>
static inline i64 write_all(int fd, const void* p, u64 n) { return write(fd, p, n); }
static inline void exit_group(int code) { _exit(code); }
#else
static inline i64 sys0(i64 n) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory"); return r; }
static inline i64 sys1(i64 n, i64 a) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a) : "rcx", "r11", "memory"); return r; }
static inline i64 sys2(i64 n, i64 a, i64 b) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory"); return r; }
static inline i64 sys3(i64 n, i64 a, i64 b, i64 c) { i64 r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static inline i64 sys4(i64 n, i64 a, i64 b, i64 c, i64 d) {
  register i64 r10 __asm__("r10") = d; i64 r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory"); return r; }
static inline i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
  register i64 r10 __asm__("r10") = d; register i64 r8 __asm__("r8") = e; register i64 r9 __asm__("r9") = f; i64 r;
  __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory"); return r; }

enum { SYS_read = 0, SYS_write = 1, SYS_mmap = 9, SYS_mprotect = 10, SYS_munmap = 11, SYS_brk = 12, SYS_rt_sigaction = 13,
       SYS_rt_sigprocmask = 14, SYS_rt_sigreturn = 15, SYS_writev = 20, SYS_nanosleep = 35, SYS_getpid = 39, SYS_clone = 56,
       SYS_exit = 60, SYS_uname = 63, SYS_gettimeofday = 96, SYS_sigaltstack = 131, SYS_arch_prctl = 158, SYS_gettid = 186,
       SYS_futex = 202, SYS_set_tid_address = 218, SYS_clock_gettime = 228, SYS_exit_group = 231, SYS_getrandom = 318 };

static inline i64 write_all(int fd, const void* p, u64 n) { return sys3(SYS_write, fd, (i64)p, (i64)n); }
static inline void exit_group(int code) { for (;;) sys1(SYS_exit_group, code); }
#endif

#ifndef WITNESS_HOSTED
// The compiler may lower struct copies and large zeroing to calls even with -ffreestanding.
void* memset(void* d, int c, u64 n) __attribute__((used));
void* memcpy(void* d, const void* s, u64 n) __attribute__((used));
void* memmove(void* d, const void* s, u64 n) __attribute__((used));
void* memset(void* d, int c, u64 n) { void* r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
void* memcpy(void* d, const void* s, u64 n) { void* r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }
void* memmove(void* d, const void* s, u64 n) {
  u8* dd = (u8*)d; const u8* ss = (const u8*)s;
  if (dd == ss || n == 0) return d;
  if (dd < ss || dd >= ss + n) return memcpy(d, s, n);
  for (u64 i = n; i > 0; i--) dd[i - 1] = ss[i - 1];
  return d;
}
#endif
static inline u64 slen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static inline void puts_(const char* s) { write_all(1, s, slen(s)); }
static inline void put_hex(u64 v, int digits) {
  char b[17]; const char* h = "0123456789abcdef";
  for (int i = 0; i < digits; i++) b[i] = h[(v >> (4 * (digits - 1 - i))) & 15];
  b[digits] = 0; write_all(1, b, (u64)digits);
}
static inline void put_dec(i64 v) {
  char b[24]; int i = 23; b[i] = 0; int neg = v < 0; u64 u = neg ? (u64)(-v) : (u64)v;
  if (!u) b[--i] = '0'; while (u) { b[--i] = (char)('0' + u % 10); u /= 10; }
  if (neg) b[--i] = '-'; write_all(1, b + i, (u64)(23 - i));
}
static int g_fail = 0;
static int g_total = 0;
// Verbose mode prints every passing check too. Failures are always printed.
#ifndef QUIET_OK
#define QUIET_OK 0
#endif
static inline void check64(const char* name, u64 got, u64 want) {
  g_total++;
  if (got == want) { if (!QUIET_OK) { puts_("ok   "); puts_(name); puts_(" = 0x"); put_hex(got, 16); puts_("\n"); } }
  else { g_fail++; puts_("FAIL "); puts_(name); puts_(" got=0x"); put_hex(got, 16); puts_(" want=0x"); put_hex(want, 16); puts_("\n"); }
}
static inline void checkb(const char* name, int cond) {
  g_total++;
  if (cond) { if (!QUIET_OK) { puts_("ok   "); puts_(name); puts_("\n"); } }
  else { g_fail++; puts_("FAIL "); puts_(name); puts_("\n"); }
}
static inline int finish(const char* suite) {
  puts_(suite); puts_(g_fail ? ": FAILED " : ": PASS "); put_dec(g_total - g_fail); puts_("/"); put_dec(g_total); puts_("\n");
  return g_fail ? 1 : 0;
}

// Program entry. The kernel enters _start with rsp 16-byte aligned (pointing at argc); a C function expects rsp = 8 mod 16
// at its first instruction, so the stub aligns the stack and calls the C entry point.
#ifdef WITNESS_HOSTED
#define WITNESS_MAIN(fn) int main(void) { return fn(); }
#else
#define WITNESS_MAIN(fn) \
  __attribute__((used, noreturn)) static void start_c(u64* initial_sp) { (void)initial_sp; exit_group(fn()); __builtin_unreachable(); } \
  __asm__(".text\n.globl _start\n_start:\n\txor %ebp,%ebp\n\tmov %rsp,%rdi\n\tand $-16,%rsp\n\tcall start_c\n\thlt\n");
#endif
