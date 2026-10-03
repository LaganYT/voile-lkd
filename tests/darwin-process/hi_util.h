/*
 * hi_util.h - helpers shared by the H, I and K witnesses (files h*.c and i*.c).
 * Builds on common.h (PASS/FAIL/INFO/SKIP lines, dp_ns, dp_stat_u64, watchdog, dp_wait_timeout).
 * common.h is shared with the F and G witnesses of another author: it is not modified, this file only adds.
 */
#ifndef HI_UTIL_H
#define HI_UTIL_H

#include "common.h"
#include <dlfcn.h>
#include <libproc.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <mach/thread_status.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <pthread/qos.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/proc_info.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

#define HI_UNUSED __attribute__((unused))
#define HI_PAGE 16384ull

/* ---- reliable fd I/O ------------------------------------------------------ */

static HI_UNUSED int hi_write_full(int fd, const void *p, size_t n)
{
    const char *c = (const char *)p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        c += w; n -= (size_t)w;
    }
    return 0;
}

static HI_UNUSED int hi_read_full(int fd, void *p, size_t n)
{
    char *c = (char *)p;
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = EPIPE; return -1; }
        c += r; n -= (size_t)r;
    }
    return 0;
}

/* Like hi_read_full but gives up after ms milliseconds (errno = ETIMEDOUT). */
static HI_UNUSED int hi_read_timeout(int fd, void *p, size_t n, int ms)
{
    char *c = (char *)p;
    uint64_t t0 = dp_ns();
    while (n) {
        int left = ms - (int)((dp_ns() - t0) / 1000000ull);
        if (left < 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pf = {fd, POLLIN, 0};
        int pr = poll(&pf, 1, left);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) { errno = ETIMEDOUT; return -1; }
        ssize_t r = read(fd, c, n);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) { errno = EPIPE; return -1; }
        c += r; n -= (size_t)r;
    }
    return 0;
}

/* ---- running a blocking call with a deadline --------------------------------
 * Used for task_for_pid: if it ever blocks (an authorization prompt is the only
 * plausible reason), the caller sees a timeout instead of hanging. */
struct hi_deadline_call {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int done;
};

static void *hi_deadline_tramp(void *p)
{
    struct hi_deadline_call *d = (struct hi_deadline_call *)p;
    void *r = d->fn(d->arg);
    pthread_mutex_lock(&d->mu);
    d->ret = r;
    d->done = 1;
    pthread_cond_signal(&d->cv);
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

/* Returns 1 if fn completed within ms (result in *ret), 0 on timeout (the worker is leaked, blocked). */
static HI_UNUSED int hi_call_with_deadline(void *(*fn)(void *), void *arg, int ms, void **ret)
{
    static struct hi_deadline_call d;   /* one at a time is enough for the witnesses */
    memset(&d, 0, sizeof d);
    d.fn = fn; d.arg = arg;
    pthread_mutex_init(&d.mu, NULL);
    pthread_cond_init(&d.cv, NULL);
    pthread_t th;
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, hi_deadline_tramp, &d) != 0) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&d.mu);
    int rc = 0;
    while (!d.done && rc == 0) rc = pthread_cond_timedwait(&d.cv, &d.mu, &ts);
    int done = d.done;
    if (done && ret) *ret = d.ret;
    pthread_mutex_unlock(&d.mu);
    return done;
}

/* ---- child processes talking over two pipes (fd 3 = from parent, fd 4 = to parent) */

struct hi_msg {
    uint32_t cmd;
    int32_t rc;
    uint64_t a[6];
};

struct hi_child {
    pid_t pid;
    int wfd;   /* parent writes commands here */
    int rfd;   /* parent reads replies here */
};

/* Forks and execs this very binary with argv[1]=role (argv[2]=arg2 if not NULL).
 * The child gets the command pipe on fd 3 and the reply pipe on fd 4; stdout/stderr are inherited. */
static HI_UNUSED int hi_spawn_role(struct hi_child *c, const char *role, const char *arg2)
{
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int a = fcntl(p2c[0], F_DUPFD, 20), b = fcntl(c2p[1], F_DUPFD, 21);
        dup2(a, 3);
        dup2(b, 4);
        for (int fd = 5; fd < 64; fd++) close(fd);
        const char *self = dp_self_path();
        if (arg2) execl(self, self, role, arg2, (char *)NULL);
        else execl(self, self, role, (char *)NULL);
        _exit(127);
    }
    close(p2c[0]);
    close(c2p[1]);
    c->pid = pid; c->wfd = p2c[1]; c->rfd = c2p[0];
    dp_track(pid);
    return 0;
}


/* Like hi_spawn_role but also hands keep[0..nkeep-1] to the child as fds 5, 6, ... */
static HI_UNUSED int hi_spawn_role_fds(struct hi_child *c, const char *role, const char *arg2, const int *keep, int nkeep)
{
    int p2c[2], c2p[2];
    if (pipe(p2c) || pipe(c2p)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int hi[16];
        for (int i = 0; i < nkeep; i++) hi[i] = fcntl(keep[i], F_DUPFD, 30);
        int a = fcntl(p2c[0], F_DUPFD, 20), b = fcntl(c2p[1], F_DUPFD, 21);
        dup2(a, 3);
        dup2(b, 4);
        for (int i = 0; i < nkeep; i++) dup2(hi[i], 5 + i);
        for (int fd = 5 + nkeep; fd < 64; fd++) close(fd);
        const char *self = dp_self_path();
        if (arg2) execl(self, self, role, arg2, (char *)NULL);
        else execl(self, self, role, (char *)NULL);
        _exit(127);
    }
    close(p2c[0]);
    close(c2p[1]);
    c->pid = pid; c->wfd = p2c[1]; c->rfd = c2p[0];
    dp_track(pid);
    return 0;
}

/* Sends one command and waits up to timeout_ms for the reply. Returns 0 on success. */
static HI_UNUSED int hi_call(struct hi_child *c, uint32_t cmd, uint64_t a0, uint64_t a1, uint64_t a2, struct hi_msg *reply, int timeout_ms)
{
    struct hi_msg m;
    memset(&m, 0, sizeof m);
    m.cmd = cmd; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2;
    if (hi_write_full(c->wfd, &m, sizeof m)) return -1;
    struct hi_msg r;
    if (hi_read_timeout(c->rfd, &r, sizeof r, timeout_ms)) return -1;
    if (reply) *reply = r;
    return 0;
}

static HI_UNUSED void hi_reap(struct hi_child *c, unsigned ms)
{
    int st;
    close(c->wfd);
    if (dp_wait_timeout(c->pid, &st, ms) == 0) { /* killed on timeout */ }
    close(c->rfd);
}


/* waitpid that also reports stops (WUNTRACED), with a deadline: returns pid, 0 on timeout, -1 on error. */
static HI_UNUSED pid_t hi_wait_stop(pid_t pid, int *status, unsigned ms)
{
    uint64_t t0 = dp_ns();
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG | WUNTRACED);
        if (r == pid) return r;
        if (r < 0 && errno != EINTR) return -1;
        if ((dp_ns() - t0) / 1000000ull > ms) return 0;
        usleep(100);
    }
}

/* ---- numbers ------------------------------------------------------------------ */

static HI_UNUSED void hi_qos_user_interactive(void)
{
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

/* Latency series helper: prints "label n= min p50 p90 p99 p99.9 max mean" in microseconds. */
static HI_UNUSED void hi_report_us(const char *id, const char *label, uint64_t *ns, size_t n)
{
    dp_stats s = dp_stat_u64(ns, n);
    INFO_STATS_US(id, label, s);
}

static HI_UNUSED const char *hi_kr_str(kern_return_t kr)
{
    static __thread char buf[160];
    snprintf(buf, sizeof buf, "kr=%d (%s)", kr, mach_error_string(kr));
    return buf;
}

/* Futex-like wait/wake on a 32-bit word, process-shared. */
static HI_UNUSED int hi_futex_wait(volatile uint32_t *addr, uint32_t expected)
{
    return os_sync_wait_on_address((void *)addr, expected, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED);
}
static HI_UNUSED int hi_futex_wake(volatile uint32_t *addr)
{
    return os_sync_wake_by_address_any((void *)addr, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

#endif
