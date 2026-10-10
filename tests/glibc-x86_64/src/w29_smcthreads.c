/* w29_smcthreads: code rewritten by one thread and run by another while a third keeps storing data into the same
 * 16 KiB host page. The store storm makes fexdarwin stop write-protecting the page and check the code's bytes before
 * running them instead (fexdarwin CodeValidation); a 100 ms warm-up lets that happen first. Then, round r: the writer
 * patches the immediate of `mov eax, imm32; ret` to r and publishes r; the runner, which keeps calling the function the
 * whole time, waits for r, serializes (cpuid, as the Intel SDM asks of cross-modifying code) and must get r back.
 * Checks: every round returns its value, no deadlock (watchdog). Usage: w29_smcthreads [rounds] (default 20000).
 * Under plain write tracking some rounds return the old value: a page is write-protected only after its code was
 * translated, and a store that lands in between is not seen. */
#define _GNU_SOURCE
#include <cpuid.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

typedef int (*fn_t)(void);
static unsigned char *page;
static fn_t fn;
static volatile unsigned long *data;
static atomic_int stop, published, acked;
static long rounds, bad, calls, first_bad[8];

static void *storm(void *a) {
  (void)a;
  for (unsigned long i = 0; !atomic_load_explicit(&stop, memory_order_relaxed); i++) data[i & 255] = i;
  return 0;
}

static void *runner(void *a) {
  (void)a;
  for (int r = 1; r <= rounds; r++) {
    while (atomic_load_explicit(&published, memory_order_acquire) < r) {
      fn();
      calls++;
    }
    unsigned x0, x1, x2, x3;
    __cpuid(0, x0, x1, x2, x3);
    if (fn() != r && bad++ < 8) first_bad[bad - 1] = r;
    atomic_store_explicit(&acked, r, memory_order_release);
  }
  return 0;
}

static void *watchdog(void *a) {
  (void)a;
  sleep(50);
  printf("FAIL: no progress (round %d acked)\n", atomic_load(&acked));
  fflush(stdout);
  _exit(3);
}

int main(int argc, char **argv) {
  rounds = argc > 1 ? atol(argv[1]) : 20000;
  unsigned char *p = mmap(0, 3 * 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  page = (unsigned char *)(((unsigned long)p + 16383) & ~16383ul); /* one 16 KiB aligned host page */
  unsigned char *code = page + 0x100;
  code[0] = 0xb8; /* mov eax, 0 ; ret */
  memset(code + 1, 0, 4);
  code[5] = 0xc3;
  fn = (fn_t)code;
  data = (volatile unsigned long *)(page + 0x2000);
  pthread_t s, r, w;
  pthread_create(&w, 0, watchdog, 0);
  pthread_create(&s, 0, storm, 0);
  pthread_create(&r, 0, runner, 0);
  struct timespec warm = {0, 100 * 1000 * 1000}, t0, t1;
  nanosleep(&warm, 0); /* the runner calls the code and the storm writes next to it */
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int k = 1; k <= rounds; k++) {
    memcpy(code + 1, &k, 4);
    atomic_store_explicit(&published, k, memory_order_release);
    while (atomic_load_explicit(&acked, memory_order_acquire) < k) {
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  atomic_store(&stop, 1);
  pthread_join(r, 0);
  pthread_join(s, 0);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  printf("info %ld rounds in %ld ms, %ld calls while waiting\n", rounds, ms, calls);
  if (bad) {
    printf("FAIL: %ld rounds returned an old value (first rounds:", bad);
    for (int i = 0; i < 8 && i < bad; i++) printf(" %ld", first_bad[i]);
    printf(")\n");
    return 1;
  }
  printf("W29 PASS\n");
  return 0;
}
