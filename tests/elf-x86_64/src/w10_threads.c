// Witness (i): guest threads created with clone(CLONE_VM | CLONE_THREAD | ...), each with its own stack and FS base,
// a shared counter updated with lock xadd, a futex ping-pong between two threads, and joins through CLONE_CHILD_CLEARTID.
// At most 8 guest threads (machine safety rule of this experiment).
#include "rt.h"

enum { PROT_READ = 1, PROT_WRITE = 2, MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20 };
#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_SIGHAND 0x800
#define CLONE_THREAD 0x10000
#define CLONE_SYSVSEM 0x40000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE 128
#define NTHREADS 8
#define STACK (256 * 1024)

static i64 futex(volatile i32* a, int op, int v) { return sys6(SYS_futex, (i64)a, op | FUTEX_PRIVATE, v, 0, 0, 0); }

struct tls_block {
  struct tls_block* self; // fs:0 points to itself like a glibc TCB
  u64 id;
  u64 result;
};

static volatile u64 g_counter;
static volatile i32 g_tids[NTHREADS];
static struct tls_block g_tls[NTHREADS];
static volatile i32 g_ball;   // futex ping-pong word
static volatile u64 g_pongs;

static u64 read_fs_id(void) { u64 v; __asm__ volatile("movq %%fs:8, %0" : "=r"(v)); return v; }

static void worker(u64 id) {
  // per-thread TLS must be visible through %fs
  u64 mine = read_fs_id();
  u64 sum = 0;
  for (u64 i = 0; i < 200000; i++) {
    __asm__ volatile("lock addq $1, %0" : "+m"(g_counter));
    sum += i * (id + 1);
  }
  g_tls[id].result = (mine == id ? 1 : 0) + (sum == 19999900000ULL * (id + 1) ? 2 : 0);
}

static void pingpong(u64 id) {
  // thread 0 waits for odd values and writes even ones, thread 1 the reverse
  for (int round = 0; round < 1000; round++) {
    i32 want = (i32)(round * 2 + (int)id);
    while (g_ball != want) futex(&g_ball, FUTEX_WAIT, g_ball);
    g_ball = want + 1;
    futex(&g_ball, FUTEX_WAKE, 1);
    __asm__ volatile("lock incq %0" : "+m"(g_pongs));
  }
}

// Raw clone: the child starts on its own stack and calls fn(arg), then exits the thread.
static i64 spawn(void (*fn)(u64), u64 arg, u64 stack_top, u64 tls, volatile i32* ctid) {
  u64* sp = (u64*)stack_top;
  *--sp = arg;
  *--sp = (u64)fn;
  i64 flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
              CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID;
  i64 ret;
  register i64 r10 __asm__("r10") = (i64)ctid;
  register i64 r8 __asm__("r8") = (i64)tls;
  __asm__ volatile(
    "syscall\n\t"
    "testq %%rax, %%rax\n\t"
    "jnz 1f\n\t"
    "popq %%rax\n\t"      // fn
    "popq %%rdi\n\t"      // arg
    "andq $-16, %%rsp\n\t"
    "call *%%rax\n\t"
    "movl $60, %%eax\n\t"
    "xorl %%edi, %%edi\n\t"
    "syscall\n\t"
    "hlt\n\t"
    "1:\n\t"
    : "=a"(ret)
    : "a"((i64)SYS_clone), "D"(flags), "S"((i64)sp), "d"((i64)ctid), "r"(r10), "r"(r8)
    : "rcx", "r11", "memory");
  return ret;
}

static void join(volatile i32* tid) {
  for (;;) {
    i32 v = *tid;
    if (v == 0) return;
    futex(tid, FUTEX_WAIT, v);
  }
}

static int run(void) {
  puts_("w10_threads: clone, TLS, atomics, futex\n");
  u64 stacks = (u64)sys6(SYS_mmap, 0, NTHREADS * STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  checkb("stacks mapped", (i64)stacks > 0);
  for (int i = 0; i < NTHREADS; i++) {
    g_tls[i].self = &g_tls[i];
    g_tls[i].id = (u64)i;
    i64 t = spawn(worker, (u64)i, stacks + (u64)(i + 1) * STACK, (u64)&g_tls[i], &g_tids[i]);
    checkb("clone returned a thread id", t > 0);
  }
  for (int i = 0; i < NTHREADS; i++) join(&g_tids[i]);
  check64("all 8 threads finished, counter", g_counter, NTHREADS * 200000ULL);
  int all = 1;
  for (int i = 0; i < NTHREADS; i++) all &= g_tls[i].result == 3;
  checkb("each thread saw its own FS base and computed its sum", all);

  // futex ping-pong between two threads
  g_tids[0] = g_tids[1] = 0;
  spawn(pingpong, 0, stacks + 1 * STACK, (u64)&g_tls[0], &g_tids[0]);
  spawn(pingpong, 1, stacks + 2 * STACK, (u64)&g_tls[1], &g_tids[1]);
  join(&g_tids[0]);
  join(&g_tids[1]);
  check64("2000 futex hand-offs", g_pongs, 2000);
  check64("final ball value", (u64)g_ball, 2000);
  return finish("w10_threads");
}
WITNESS_MAIN(run)
