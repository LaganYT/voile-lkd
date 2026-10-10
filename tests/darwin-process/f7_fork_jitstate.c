/*
 * f7_fork_jitstate.c - F: the MAP_JIT write state of the child of a fork (request wine-5).
 *
 * pthread_jit_write_protect_np() switches the calling thread between "MAP_JIT writable" and "MAP_JIT executable"
 * without entering the kernel. Questions answered here:
 *   - which state does the single thread of a fork child start in: the parent thread's state at the fork, or the
 *     state saved at that thread's last context switch?
 *   - which events save the state (blocking, preemption, a system call, a page fault, a signal)?
 *   - does a new thread or another thread of the parent see it?
 *   - does pthread_jit_write_protect_np(1) in the child fix it?
 * Each case forks N children. A child runs one instruction from a MAP_JIT page under a temporary SIGBUS handler and
 * exits 0 (ran), 10 (SIGBUS) or 11 (wrong result): no child crashes, so the OS writes no crash report.
 */
#include "fg_util.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>

#define N 40

typedef int (*jit_fn)(void);
static jit_fn jit;
static sigjmp_buf child_jb;

static void child_bus(int sig) { siglongjmp(child_jb, sig); }

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void spin_ns(uint64_t ns) { uint64_t t = now_ns(); while (now_ns() - t < ns) {} }

/* In the child: optionally reset the state, then run the JIT function under a SIGBUS handler. */
static int child_run(int reset)
{
    if (reset) pthread_jit_write_protect_np(1);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = child_bus;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    if (sigsetjmp(child_jb, 1) != 0) return 10;
    return jit() == 42 ? 0 : 11;
}

/* Forks after prepare() and returns how many of N children could not run the JIT code. */
static int forks_failing(void (*prepare)(void), int reset)
{
    int bad = 0;
    for (int i = 0; i < N; i++) {
        prepare();
        pid_t pid = fork();
        if (pid == 0) _exit(child_run(reset));
        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
    }
    return bad;
}

static void exec_blocked(void) { usleep(200); }
static void write_blocked(void) { pthread_jit_write_protect_np(0); usleep(200); pthread_jit_write_protect_np(1); }
static void write_syscall(void) { pthread_jit_write_protect_np(0); (void)getpid(); pthread_jit_write_protect_np(1); }
static void write_spin(void) { pthread_jit_write_protect_np(0); spin_ns(30000000); pthread_jit_write_protect_np(1); }

static unsigned char *fault_area;
static int fault_next;
static void write_fault(void)
{
    pthread_jit_write_protect_np(0);
    fault_area[(size_t)(fault_next++) * FG_PAGE] = 1; /* first touch of an ordinary page */
    pthread_jit_write_protect_np(1);
}

static void on_usr1(int sig) { (void)sig; pthread_jit_write_protect_np(0); usleep(200); pthread_jit_write_protect_np(1); }
static void handler_blocked(void) { raise(SIGUSR1); }

static _Atomic int writers_stop;
static void *writer(void *a)
{
    (void)a;
    while (!atomic_load(&writers_stop)) {
        pthread_jit_write_protect_np(0);
        usleep(100);
        pthread_jit_write_protect_np(1);
    }
    return NULL;
}

static void *thread_runs(void *a) { (void)a; return (void *)(intptr_t)jit(); }

int main(void)
{
    dp_watchdog(240);
    fg_load_info("F7.env");
    void *p = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { FAIL("F7.setup", "mmap MAP_JIT: %s", ERRS(errno)); dp_summary("f7_fork_jitstate"); return 1; }
    static const uint32_t code[] = {0x52800540 /* mov w0, #42 */, 0xd65f03c0 /* ret */};
    pthread_jit_write_protect_np(0);
    memcpy(p, code, sizeof code);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(p, sizeof code);
    jit = (jit_fn)p;
    fault_area = mmap(NULL, (size_t)(N + 1) * FG_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    signal(SIGUSR1, on_usr1);

    int n = forks_failing(exec_blocked, 0);
    CHECK(n == 0, "F7.control", "parent blocked in execute mode, then fork: %d/%d children cannot run MAP_JIT code", n, N);
    n = forks_failing(write_blocked, 0);
    CHECK(n > N / 2, "F7.stale", "parent blocked (usleep) in write mode, back in execute mode without a system call, then fork: "
          "%d/%d children get SIGBUS on MAP_JIT code (the child has the state of the parent's last switch)", n, N);
    n = forks_failing(write_blocked, 1);
    CHECK(n == 0, "F7.reset", "same, the child calls pthread_jit_write_protect_np(1) first: %d/%d children fail", n, N);
    n = forks_failing(write_spin, 0);
    INFO("F7.preempt", "parent spins 30 ms in write mode (preempted), then fork: %d/%d children get SIGBUS", n, N);
    n = forks_failing(write_syscall, 0);
    INFO("F7.syscall", "parent makes a system call (getpid) in write mode, then fork: %d/%d children get SIGBUS", n, N);
    n = forks_failing(write_fault, 0);
    INFO("F7.fault", "parent takes a page fault (first touch) in write mode, then fork: %d/%d children get SIGBUS", n, N);
    n = forks_failing(handler_blocked, 0);
    INFO("F7.signal", "a signal handler blocks in write mode, then fork: %d/%d children get SIGBUS", n, N);

    pthread_t w[4];
    for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, writer, NULL);
    n = forks_failing(exec_blocked, 0);
    atomic_store(&writers_stop, 1);
    for (int i = 0; i < 4; i++) pthread_join(w[i], NULL);
    CHECK(n == 0, "F7.other", "4 other threads keep blocking in write mode, the forking thread does not: %d/%d children fail", n, N);

    int bad = 0;
    for (int i = 0; i < N; i++) {
        write_blocked();
        pthread_t t;
        void *r = NULL;
        pthread_create(&t, NULL, thread_runs, NULL);
        pthread_join(t, &r);
        if ((intptr_t)r != 42) bad++;
    }
    CHECK(bad == 0, "F7.thread", "a thread created after its creator blocked in write mode runs MAP_JIT code: %d/%d failed", bad, N);

    dp_summary("f7_fork_jitstate");
    return 0;
}
