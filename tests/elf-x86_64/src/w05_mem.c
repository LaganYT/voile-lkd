// Witness (e): anonymous mmap, mprotect, munmap, brk and madvise with sizes that are multiples of 16 KiB.
#include "rt.h"

enum { PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_PRIVATE = 2, MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20,
       MAP_FIXED_NOREPLACE = 0x100000 };
#define K16 16384UL

static i64 mmap_(u64 addr, u64 len, int prot, int flags) { return sys6(SYS_mmap, (i64)addr, (i64)len, prot, flags, -1, 0); }
static int is_err(i64 r) { return r < 0 && r > -4096; }

static int run(void) {
  puts_("w05_mem: mmap, mprotect, munmap, brk, madvise\n");
  // basic anonymous mapping
  i64 a = mmap_(0, 4 * K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
  checkb("mmap anon returns an address", !is_err(a));
  // Linux guarantees PAGE_SIZE (4 KiB) alignment; the old built-in host happened to return 16 KiB aligned addresses.
  check64("mmap result is page (4 KiB) aligned", (u64)a & 4095, 0);
  { u8* p = (u8*)a; int zero = 1; for (u64 i = 0; i < 4 * K16; i += 4093) zero &= p[i] == 0; zero &= p[4 * K16 - 1] == 0; checkb("fresh anonymous memory is zero", zero); }
  { u64* q = (u64*)a; for (u64 i = 0; i < 4 * K16 / 8; i++) q[i] = i * 0x0101010101010101ULL; u64 s = 0; for (u64 i = 0; i < 4 * K16 / 8; i++) s ^= q[i];
    u64 want = 0; for (u64 i = 0; i < 4 * K16 / 8; i++) want ^= i * 0x0101010101010101ULL; check64("write then read the whole mapping", s, want); }

  // a second mapping must not overlap the first
  i64 b = mmap_(0, 2 * K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS);
  checkb("second mmap is a different range", !is_err(b) && ((u64)b + 2 * K16 <= (u64)a || (u64)b >= (u64)a + 4 * K16));

  // mprotect to read-only and back, contents preserved
  { *(volatile u64*)a = 0x1234; check64("mprotect PROT_READ", (u64)sys3(SYS_mprotect, a, K16, PROT_READ), 0);
    check64("read-only page still readable", *(volatile u64*)a, 0x1234);
    check64("mprotect back to RW", (u64)sys3(SYS_mprotect, a, K16, PROT_READ | PROT_WRITE), 0);
    *(volatile u64*)a = 0x5678; check64("writable again", *(volatile u64*)a, 0x5678); }
  check64("mprotect on an unmapped range fails with ENOMEM", (u64)sys3(SYS_mprotect, 0x100000000000ULL, K16, PROT_READ), (u64)-12);
  check64("mprotect with a misaligned address fails with EINVAL", (u64)sys3(SYS_mprotect, a + 8, K16, PROT_READ), (u64)-22);

  // partial munmap in the middle splits the mapping, the rest stays usable
  { check64("munmap middle page", (u64)sys2(SYS_munmap, a + K16, K16), 0);
    *(volatile u64*)a = 0xaa; *(volatile u64*)(a + 2 * K16) = 0xbb; check64("first part still mapped", *(volatile u64*)a, 0xaa); check64("last part still mapped", *(volatile u64*)(a + 2 * K16), 0xbb); }

  // MAP_FIXED over an existing mapping replaces it with zero pages
  { *(volatile u64*)(a + 2 * K16) = 0xdeadbeef;
    i64 r = mmap_(a + 2 * K16, K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED);
    check64("MAP_FIXED returns the requested address", (u64)r, a + 2 * K16);
    check64("MAP_FIXED replaces the old contents with zeros", *(volatile u64*)(a + 2 * K16), 0); }
  // MAP_FIXED_NOREPLACE refuses an occupied range
  check64("MAP_FIXED_NOREPLACE on an occupied range fails with EEXIST", (u64)mmap_(a, K16, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE), (u64)-17);
  // a hole left by munmap can be mapped again at the same address, zero filled
  { i64 r = mmap_(a + K16, K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE);
    check64("MAP_FIXED_NOREPLACE in the hole succeeds at the address", (u64)r, a + K16); check64("hole is zero filled", *(volatile u64*)(a + K16), 0); }

  // PROT_NONE mapping and its reuse
  { i64 r = mmap_(0, K16, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS); checkb("PROT_NONE mmap", !is_err(r));
    check64("mprotect PROT_NONE -> RW", (u64)sys3(SYS_mprotect, r, K16, PROT_READ | PROT_WRITE), 0); *(volatile u64*)r = 1; check64("usable after mprotect", *(volatile u64*)r, 1);
    check64("munmap", (u64)sys2(SYS_munmap, r, K16), 0); }

  // big mapping, sparse touch
  { i64 r = mmap_(0, 64UL << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS); checkb("64 MiB mapping", !is_err(r));
    *(volatile u64*)r = 11; *(volatile u64*)(r + (63UL << 20)) = 22; check64("sparse touch low", *(volatile u64*)r, 11); check64("sparse touch high", *(volatile u64*)(r + (63UL << 20)), 22);
    check64("munmap 64 MiB", (u64)sys2(SYS_munmap, r, 64UL << 20), 0); }

  // madvise(MADV_DONTNEED) zeroes private anonymous memory
  { i64 r = mmap_(0, K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS); *(volatile u64*)r = 0x77; *(volatile u64*)(r + K16 - 8) = 0x88;
    check64("madvise DONTNEED", (u64)sys3(28, r, K16, 4), 0); check64("DONTNEED zeroed first word", *(volatile u64*)r, 0); check64("DONTNEED zeroed last word", *(volatile u64*)(r + K16 - 8), 0); }

  // brk: grows, is zeroed, shrinks, queries
  { i64 cur = sys1(SYS_brk, 0); checkb("brk(0) returns the current break", cur > 0 && !is_err(cur));
    i64 n = sys1(SYS_brk, cur + 3 * K16 + 100); check64("brk grows", (u64)n, (u64)cur + 3 * K16 + 100);
    *(volatile u64*)(cur + 2 * K16) = 0x42; check64("heap memory is usable", *(volatile u64*)(cur + 2 * K16), 0x42);
    check64("brk below the start leaves the break unchanged", (u64)sys1(SYS_brk, 1), (u64)n);
    i64 s = sys1(SYS_brk, cur); check64("brk shrinks back", (u64)s, (u64)cur);
    i64 g = sys1(SYS_brk, cur + K16); check64("brk regrows", (u64)g, (u64)cur + K16); check64("regrown heap is zero", *(volatile u64*)(cur), 0);
    sys1(SYS_brk, cur); }

  // unmapped addresses give EFAULT from the kernel side
  check64("write from an unmapped buffer returns EFAULT", (u64)sys3(SYS_write, 1, 0x100000000000ULL, 16), (u64)-14);
  check64("munmap length 0 fails with EINVAL", (u64)sys2(SYS_munmap, a, 0), (u64)-22);
  return finish("w05_mem");
}
WITNESS_MAIN(run)
