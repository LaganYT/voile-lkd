/* test_ptrace_util.h: shared helpers of the ptrace tests. Owner: ptrace.
 *
 * Roles. The tracer is the main task of the test process (TR). A tracee is either a fake task run by a
 * thread of this process (cheap, used for the request semantics) or a real forked guest process (the EAC
 * sequence, memory of another host process, death of a host process). A tracee plays the engine: it keeps
 * its registers in the fake CPU, enters LKD with lkd_syscall (so the ptrace syscall hooks run) and runs
 * the engine's safe-point loop after every call. */
#ifndef LKD_TEST_PTRACE_UTIL_H
#define LKD_TEST_PTRACE_UTIL_H

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task.h"
#include "../src/mm/lkd_mm_ext.h"

#pragma clang diagnostic ignored "-Wunused-function"

static lkd_task *TR;
static const char *g_filter;
#define RUN(name) do { if (!g_filter || !strcmp(g_filter, #name)) LT_RUN(name); } while (0)

static int allow_all(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }
#define P(x) ((uint64_t)(uintptr_t)(x))

static int64_t sct(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &rq);
}
/* ptrace(2) called by the tracer */
static int64_t PT(int64_t req, int32_t pid, uint64_t addr, uint64_t data)
{
    return sct(TR, LKD_NR_ptrace, (uint64_t)req, (uint64_t)(int64_t)pid, addr, data, 0, 0);
}

/* ---- the engine side of a tracee --------------------------------------------------------------- */

#define CODE_BASE 0x401000ull
static void cpu_init(lkd_task *t)
{
    struct fake_cpu *c = fake_cpu_of(t);
    memset(&c->regs, 0, sizeof c->regs);
    c->regs.rip = CODE_BASE;
    c->regs.rsp = 0x7ffffff0000ull;
    c->regs.cs = 0x33; c->regs.ss = 0x2b;
    c->regs.eflags = 0x246;
    c->regs.fs_base = 0x7f0000001000ull;
    c->regs.rbx = 0x1111; c->regs.rbp = 0x2222; c->regs.r12 = 0x3333;
    memset(&c->fp, 0, sizeof c->fp);
    c->fp.mxcsr = 0x1f80; c->fp.mxcr_mask = 0xffff;
    c->fp.cwd = 0x37f;
    memset(c->xsave, 0, sizeof c->xsave);
    memcpy(c->xsave, &c->fp, sizeof c->fp);
    c->xsave_len = 832;
}

/* The safe point after a call, a kick or a fault: deliver signals until none is left. -1 if the task died. */
static int safe_point(lkd_task *t)
{
    while (lkd_signal_pending(t)) {
        int r = lkd_signal_next(t);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    return 0;
}

/* One guest syscall as the engine does it: RIP moves past SYSCALL (2 bytes), RAX = number, RCX and R11
 * are garbage (the ptrace view must patch them). Returns RAX; sets *died when the task must die. */
static int64_t gsys(lkd_task *t, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, int *died)
{
    struct fake_cpu *c = fake_cpu_of(t);
    c->regs.rip += 2;
    c->regs.rax = nr; c->regs.orig_rax = nr;
    c->regs.rdi = a0; c->regs.rsi = a1; c->regs.rdx = a2; c->regs.r10 = a3; c->regs.r8 = a4; c->regs.r9 = a5;
    c->regs.rcx = 0xdeadbeef; c->regs.r11 = 0xfeedface;
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, a5 } };
    int64_t rax = 0;
    lkd_disp d = lkd_syscall(t, &rq, &rax);
    if (died) *died = 0;
    switch (d) {
    case LKD_RETURN: c->regs.rax = (uint64_t)rax; break;
    case LKD_RESTART: c->regs.rip -= 2; c->regs.rax = (uint64_t)rax; break;
    case LKD_EXIT_THREAD: case LKD_EXIT_GROUP: if (died) *died = 1; return 0;
    default: break;
    }
    if (safe_point(t) < 0 && died) *died = 1;
    return rax;
}

/* tgkill(self, self, sig) then the safe point */
static int self_signal(lkd_task *t, int sig)
{
    int died = 0;
    (void)gsys(t, LKD_NR_tgkill, (uint64_t)t->proc->pid, (uint64_t)t->tid, (uint64_t)sig, 0, 0, 0, &died);
    return died;
}

/* ---- tracee threads ---------------------------------------------------------------------------- */

struct tracee {
    lkd_task *t;
    pthread_t th;
    void (*body)(struct tracee *);
    void *arg;
    volatile int started, done;
    int exit_code;
    int exit_status;             /* what lkd_task_release reported: exit code, or 128 + signal */
    volatile int64_t last_ret;   /* result of the last syscall the loop made */
    volatile uint64_t calls;
    volatile int hold;           /* tracee waits here while set (lets a test hold it between calls) */
    int rd[2], wr[2];     /* tracer -> tracee and tracee -> tracer pipes */
    int32_t pid, tid;
};

static void *tracee_main(void *a)
{
    struct tracee *e = a;
    lkd_set_current(e->t);
    e->started = 1;
    e->body(e);
    e->done = 1;
    return NULL;
}

static struct tracee *tracee_start(void (*body)(struct tracee *), void *arg)
{
    struct tracee *e = calloc(1, sizeof *e);
    lkd_task *t = lkd_task_alloc(NULL, 0);
    lkd_task_publish(t);
    cpu_init(t);
    e->t = t; e->body = body; e->arg = arg; e->pid = t->proc->pid; e->tid = t->tid;
    if (pipe(e->rd) || pipe(e->wr)) abort();
    fcntl(e->rd[0], F_SETFL, O_NONBLOCK);
    if (pthread_create(&e->th, NULL, tracee_main, e)) abort();
    return e;
}

/* The tracee ends with exit_group like a guest process does. */
static void tracee_exit(struct tracee *e, int code)
{
    int died;
    (void)gsys(e->t, LKD_NR_exit_group, (uint64_t)code, 0, 0, 0, 0, 0, &died);
    int st = 0;
    (void)lkd_task_release(e->t, &st);
    e->exit_code = code;
    e->exit_status = st;
}

static void tracee_join(struct tracee *e)
{
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

/* The standard tracee: installs handlers for SIGUSR1 and SIGUSR2, then makes getpid() calls with a short
 * sleep between them until a byte arrives on rd[0]; reports 1 on wr[1] and exits with code 3. */
static void put64(int fd, int64_t v);
static void install_handler(lkd_task *t, int sig)
{
    struct lx_sigaction act;
    memset(&act, 0, sizeof act);
    act.handler = 0x500000;
    act.flags = 0x04000000; /* SA_RESTORER */
    act.restorer = 0x500100;
    int died;
    (void)gsys(t, LKD_NR_rt_sigaction, (uint64_t)sig, P(&act), 0, 8, 0, 0, &died);
}

static void body_loop(struct tracee *e)
{
    char c;
    int died = 0;
    install_handler(e->t, 10);
    install_handler(e->t, 12);
    install_handler(e->t, 14);
    install_handler(e->t, 15);
    for (int i = 0; i < 40000 && !died; i++) {
        while (e->hold && !died) usleep(500);
        e->last_ret = gsys(e->t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died);
        e->calls++;
        if (read(e->rd[0], &c, 1) == 1) break;
        usleep(300);
    }
    put64(e->wr[1], 1);
    tracee_exit(e, 3);
}

static void tracee_quit(struct tracee *e)
{
    char q = 1;
    (void)!write(e->rd[1], &q, 1);
}

static void put64(int fd, int64_t v) { (void)!write(fd, &v, sizeof v); }
static int64_t get64(int fd)
{
    int64_t v = -777777;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    if (poll(&p, 1, 20000) <= 0) return -777777;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -777777;
    return v;
}

/* ---- the tracer side --------------------------------------------------------------------------- */

/* wait4(pid) with a deadline; returns the pid (or the wait4 error) and the raw Linux status. */
static int64_t wait_pid(int32_t pid, int *status, int options)
{
    int st = -1;
    int64_t r = sct(TR, LKD_NR_wait4, (uint64_t)(int64_t)pid, P(&st), (uint64_t)options, 0, 0, 0);
    if (status) *status = st;
    return r;
}

/* get the stop (blocking) and check that it is stopped with this exact wait status */
static int expect_stop(int32_t pid, int status)
{
    int st = 0;
    int64_t r = wait_pid(pid, &st, 0);
    return r == pid && st == status;
}

static void get_regs216(int32_t pid, struct lx_user_regs *out, int64_t *ret)
{
    struct lx_iovec iov = { P(out), sizeof *out };
    int64_t r = PT(LX_PTRACE_GETREGSET, pid, LX_NT_PRSTATUS, P(&iov));
    if (ret) *ret = r == 0 ? (int64_t)iov.iov_len : r;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}
static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static void test_setup(int with_mm)
{
    LT_EQ(fake_engine_init(), 0);
    lkd_usermem_set_check_fn(allow_all);
    TR = fake_make_task();
    if (with_mm) {
        TR->mm = lkd_mm_create();
        LT_CHECK(TR->mm != NULL);
        LT_EQ(mm_remote_start(), 0);
    }
    alarm(100);
}

#endif
