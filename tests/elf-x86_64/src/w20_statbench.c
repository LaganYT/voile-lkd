// Witness (menu profile, lead): cost of getrusage(RUSAGE_THREAD/SELF) and of reading /proc/self/status with a
// game-like address space (1000 small mappings and 256 MiB of touched anonymous memory). Prints the mean cost per
// call in ns and checks that the values stay truthful (thread CPU time grows, VmRSS covers the touched memory).
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20, SYS_open = 2, SYS_close = 3,
       SYS_getrusage = 98, O_RDONLY = 0 };
struct timeval_ { i64 s, us; };
struct rusage_ { struct timeval_ ut, st; i64 rest[14]; };

static i64 now_ns(void) { i64 ts[2]; sys3(SYS_clock_gettime, 1, (i64)ts, 0); return ts[0] * 1000000000 + ts[1]; }
static void report(const char* what, i64 total_ns, i64 n) { puts_("MEASURE "); puts_(what); puts_(": "); put_dec(total_ns / n); puts_(" ns per call\n"); }

static char buf[8192];
static i64 status_vmrss_kb(void) {
  i64 fd = sys3(SYS_open, (i64)"/proc/self/status", O_RDONLY, 0);
  if (fd < 0) return -1;
  i64 n = sys3(SYS_read, fd, (i64)buf, sizeof buf - 1);
  sys1(SYS_close, fd);
  if (n <= 0) return -1;
  buf[n] = 0;
  for (i64 i = 0; i + 6 < n; i++)
    if (buf[i] == 'V' && buf[i + 1] == 'm' && buf[i + 2] == 'R' && buf[i + 3] == 'S' && buf[i + 4] == 'S' && buf[i + 5] == ':') {
      i64 v = 0, j = i + 6; while (buf[j] == ' ' || buf[j] == '\t') j++;
      while (buf[j] >= '0' && buf[j] <= '9') v = v * 10 + (buf[j++] - '0');
      return v;
    }
  return -1;
}

static int run(void) {
  puts_("w20_statbench: getrusage and /proc/self/status costs\n");
  for (int i = 0; i < 1000; i++) {   // many VMAs, alternate protections so they do not merge
    i64 m = sys6(SYS_mmap, 0, 0x2000, (i & 1) ? PROT_READ : PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!(i & 1) && m > 0) *(volatile u8*)m = 1;
  }
  u64 big = 256ull << 20;
  u8* p = (u8*)sys6(SYS_mmap, 0, (i64)big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  for (u64 o = 0; o < big; o += 4096) p[o] = 1;
  struct rusage_ r0, r1;
  sys2(SYS_getrusage, 1, (i64)&r0);
  i64 t0 = now_ns(); const i64 N = 200000;
  for (i64 i = 0; i < N; i++) sys2(SYS_getrusage, 1, (i64)&r1);
  report("getrusage(RUSAGE_THREAD)", now_ns() - t0, N);
  checkb("thread CPU time grows", r1.ut.s * 1000000 + r1.ut.us + r1.st.s * 1000000 + r1.st.us > r0.ut.s * 1000000 + r0.ut.us + r0.st.s * 1000000 + r0.st.us);
  t0 = now_ns();
  for (i64 i = 0; i < N / 4; i++) sys2(SYS_getrusage, 0, (i64)&r1);
  report("getrusage(RUSAGE_SELF)", now_ns() - t0, N / 4);
  t0 = now_ns(); const i64 M = 2000; i64 rss = 0;
  for (i64 i = 0; i < M; i++) rss = status_vmrss_kb();
  report("read of /proc/self/status", now_ns() - t0, M);
  puts_("VmRSS "); put_dec(rss); puts_(" kB\n");
  checkb("VmRSS covers the 256 MiB touched", rss >= 256 * 1024);
  return finish("w20_statbench");
}
WITNESS_MAIN(run)
