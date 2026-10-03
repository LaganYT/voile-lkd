// Witness (a): hello through write and exit_group.
#include "rt.h"
static int run(void) {
  static const char msg[] = "hello from x86-64 under FEXCore on macOS arm64\n";
  write_all(1, msg, sizeof(msg) - 1);
  return 0;
}
WITNESS_MAIN(run)
