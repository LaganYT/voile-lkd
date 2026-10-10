// w33_forkwx: a forked child must run the engine's code right after the fork (request wine-5).
// macOS gives the child of a fork the parent thread's MAP_JIT write state as saved at that thread's last context
// switch, not the state the thread has when it forks. A parent switched out while the engine was writing JIT code
// (pthread_jit_write_protect_np(0)) and back in execute mode since then has a child that dies with SIGBUS on the
// first instruction of the engine's dispatcher, before its first guest instruction.
// The witness makes such a switch likely just before each fork. A caller with thousands of direct calls to 256 leaf
// functions is translated once. Each round maps the leaves at the same address, runs the caller (the engine translates
// the leaves and links every call site), unmaps the leaves (the engine undoes every one of those links at once, each
// with its own short JIT write, while spinner threads load every CPU), then forks a child that exits at once.
// Arguments: rounds (600), call sites (60000), spinner threads (2 per CPU).
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define LEAVES 256
#define LEAF_SLOT 80
#define LEAVES_BYTES (32 * 1024) // two 16 KiB host pages, apart from the caller's
#define HOST_PAGE (16 * 1024)

static atomic_int stop;

static void* spin(void* arg) {
  (void)arg;
  volatile uint64_t n = 0;
  while (!atomic_load_explicit(&stop, memory_order_relaxed)) n++;
  return NULL;
}

// Leaf f of round `seed` at its fixed slot: mov eax, imm32; 1 to 13 x add eax, imm32; ret. Stores its result.
static void write_leaves(uint8_t* leaves, uint32_t seed, uint32_t* values) {
  for (int f = 0; f < LEAVES; f++) {
    uint8_t* q = leaves + f * LEAF_SLOT;
    uint32_t value = seed * 7919u + (uint32_t)f;
    *q++ = 0xb8;
    memcpy(q, &value, 4);
    q += 4;
    int adds = 1 + (int)((seed + (uint32_t)f) % 13);
    for (int a = 0; a < adds; a++) {
      uint32_t k = (uint32_t)(f * 31 + a);
      *q++ = 0x05;
      memcpy(q, &k, 4);
      q += 4;
      value += k;
    }
    *q = 0xc3;
    values[f] = value;
  }
}

// push rbx; xor ebx, ebx; calls x {call leaf (c % LEAVES); add ebx, eax}; mov eax, ebx; pop rbx; ret
static void write_caller(uint8_t* q, const uint8_t* leaves, int calls) {
  *q++ = 0x53;
  *q++ = 0x31;
  *q++ = 0xdb;
  for (int c = 0; c < calls; c++) {
    *q++ = 0xe8;
    int32_t rel = (int32_t)(leaves + (c % LEAVES) * LEAF_SLOT - (q + 4));
    memcpy(q, &rel, 4);
    q += 4;
    *q++ = 0x01;
    *q++ = 0xc3;
  }
  *q++ = 0x89;
  *q++ = 0xd8;
  *q++ = 0x5b;
  *q = 0xc3;
}

int main(int argc, char** argv) {
  int rounds = argc > 1 ? atoi(argv[1]) : 600;
  int calls = argc > 2 ? atoi(argv[2]) : 60000;
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  int spinners = argc > 3 ? atoi(argv[3]) : (int)(2 * (ncpu > 0 ? ncpu : 4));
  if (spinners > 256) spinners = 256;

  const size_t caller_bytes = ((size_t)calls * 7 + 16 + HOST_PAGE - 1) & ~(size_t)(HOST_PAGE - 1);
  uint8_t* base = mmap(NULL, HOST_PAGE + LEAVES_BYTES + caller_bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) {
    perror("W33 FAIL: mmap");
    return 1;
  }
  uint8_t* leaves = (uint8_t*)(((uintptr_t)base + HOST_PAGE - 1) & ~(uintptr_t)(HOST_PAGE - 1));
  uint8_t* caller = leaves + LEAVES_BYTES;
  uint32_t values[LEAVES];
  write_leaves(leaves, 0, values);
  write_caller(caller, leaves, calls);

  pthread_t th[256];
  for (int i = 0; i < spinners; i++) {
    if (pthread_create(&th[i], NULL, spin, NULL) != 0) {
      fprintf(stderr, "W33 FAIL: pthread_create\n");
      return 1;
    }
  }

  int wrong = 0, dead = 0, errors = 0;
  for (int r = 0; r < rounds; r++) {
    if (r > 0) {
      if (mmap(leaves, LEAVES_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
          leaves) {
        perror("W33 FAIL: mmap of the leaves");
        return 1;
      }
      write_leaves(leaves, (uint32_t)r, values);
    }
    uint32_t expect = 0;
    for (int c = 0; c < calls; c++) expect += values[c % LEAVES];
    if (((uint32_t(*)(void))caller)() != expect) wrong++;
    munmap(leaves, LEAVES_BYTES);

    pid_t child = fork();
    if (child < 0) {
      errors++;
      continue;
    }
    if (child == 0) _exit(0);
    int st = 0;
    if (waitpid(child, &st, 0) != child) {
      errors++;
    } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      dead++;
      if (dead <= 5)
        printf("W33 round %d: child status 0x%x (%s %d)\n", r, st, WIFSIGNALED(st) ? "signal" : "exit",
               WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
    }
  }

  atomic_store(&stop, 1);
  for (int i = 0; i < spinners; i++) pthread_join(th[i], NULL);
  printf("W33 %d rounds of %d call sites, %d spinners: %d children died, %d wrong sums, %d fork or wait errors\n",
         rounds, calls, spinners, dead, wrong, errors);
  if (dead || wrong || errors) {
    printf("W33 FAIL\n");
    return 1;
  }
  printf("W33 PASS\n");
  return 0;
}
