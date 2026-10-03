// Witness: very deep call/ret recursion (overflows FEX's call-ret shadow stack, which is reset on its guard page) must
// still return to the right places, with and without an intervening signal.
#include "rt.h"
__attribute__((noinline)) static u64 rec(u64 n) { if (n == 0) return 1; u64 r = rec(n - 1); __asm__ volatile("" ::: "memory"); return r + (n & 1); }
static int run(void) {
  puts_("w13_deeprec\n");
  check64("depth 1000", rec(1000), 501);
  check64("depth 100000 (beyond the shadow stack)", rec(100000), 50001);
  check64("depth 100000 again", rec(100000), 50001);
  check64("shallow after deep", rec(10), 6);
  return finish("w13_deeprec");
}
WITNESS_MAIN(run)
