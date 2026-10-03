// Witness (q): many sequential fork()s, one child at a time. Each child runs a little translated code (a loop the
// parent also ran, so its translation already exists before the fork) and exits with a code the parent checks.
// Regression test for an intermittent SIGBUS in the FEX dispatcher of the child after a host fork.
#include "rt.h"
#define SYS_fork_ 57
#define SYS_wait4_ 61
#define ROUNDS 150

static u64 work(u64 n) { u64 a = 1; for (u64 i = 0; i < n; i++) a = a * 6364136223846793005ull + i; return a; }

static int run(void) {
  puts_("w17_forkloop: sequential fork/exit/wait\n");
  u64 ref = work(20000);
  int ok = 0;
  for (int r = 0; r < ROUNDS; r++) {
    i64 pid = sys0(SYS_fork_);
    if (pid == 0) {
      u64 v = work(20000);
      exit_group(v == ref ? 7 : 9);
    }
    if (pid < 0) break;
    int st = 0;
    i64 w = sys4(SYS_wait4_, pid, (i64)&st, 0, 0);
    if (w == pid && (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 7) ok++;
    else { puts_("round "); put_dec(r); puts_(" status "); put_hex((u64)st, 8); puts_("\n"); }
  }
  check64("every child ran its code and exited 7", (u64)ok, ROUNDS);
  return finish("w17_forkloop");
}
WITNESS_MAIN(run)
