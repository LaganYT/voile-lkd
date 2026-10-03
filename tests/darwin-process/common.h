/*
 * common.h - shared helpers for the darwin-process witnesses.
 *
 * Every test prints lines of the form
 *     PASS <id> <text>     the expectation held
 *     FAIL <id> <text>     the expectation did not hold (this is data, not always a bug)
 *     INFO <id> <text>     a measured value or a plain observation
 *     SKIP <id> <text>     not testable here, with the reason
 * Output goes through write(2) in one call per line, so forked children and
 * spawned children can share the same stdout without stdio buffer duplication.
 */
#ifndef DP_COMMON_H
#define DP_COMMON_H

#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *dp_role = "";          /* set to "child" etc. in subprocesses */
static int dp_npass, dp_nfail, dp_ninfo, dp_nskip;

__attribute__((format(printf, 3, 4), unused))
static void dp_emit(const char *tag, const char *id, const char *fmt, ...)
{
    char buf[3072];
    int n = snprintf(buf, sizeof buf, "%s %s%s%s ", tag, id, dp_role[0] ? "@" : "", dp_role);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    ssize_t w = write(1, buf, n);
    (void)w;
}

#define PASS(id, ...) do { dp_npass++; dp_emit("PASS", id, __VA_ARGS__); } while (0)
#define FAIL(id, ...) do { dp_nfail++; dp_emit("FAIL", id, __VA_ARGS__); } while (0)
#define INFO(id, ...) do { dp_ninfo++; dp_emit("INFO", id, __VA_ARGS__); } while (0)
#define SKIP(id, ...) do { dp_nskip++; dp_emit("SKIP", id, __VA_ARGS__); } while (0)
#define CHECK(cond, id, ...) do { if (cond) { PASS(id, __VA_ARGS__); } else { FAIL(id, __VA_ARGS__); } } while (0)

/* ---- time ---------------------------------------------------------------- */

static inline uint64_t dp_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

typedef struct {
    size_t n;
    double min, p50, p90, p99, p999, max, mean;
} dp_stats;

static int dp_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Sorts v in place. Values are in whatever unit the caller used (ns usually). */
__attribute__((unused))
static dp_stats dp_stat_u64(uint64_t *v, size_t n)
{
    dp_stats s;
    memset(&s, 0, sizeof s);
    if (!n) return s;
    qsort(v, n, sizeof v[0], dp_cmp_u64);
    s.n = n;
    s.min = (double)v[0];
    s.max = (double)v[n - 1];
    s.p50 = (double)v[n / 2];
    s.p90 = (double)v[(n * 90) / 100];
    s.p99 = (double)v[(n * 99) / 100];
    s.p999 = (double)v[(n * 999) / 1000 < n ? (n * 999) / 1000 : n - 1];
    long double sum = 0;
    for (size_t i = 0; i < n; i++) sum += v[i];
    s.mean = (double)(sum / n);
    return s;
}

/* Prints a stats line in microseconds from nanosecond samples. */
#define INFO_STATS_US(id, label, st) \
    INFO(id, "%s n=%zu min=%.2f p50=%.2f p90=%.2f p99=%.2f p99.9=%.2f max=%.2f mean=%.2f (us)", \
         label, (st).n, (st).min / 1e3, (st).p50 / 1e3, (st).p90 / 1e3, (st).p99 / 1e3, (st).p999 / 1e3, \
         (st).max / 1e3, (st).mean / 1e3)

/* ---- children bookkeeping and watchdog ---------------------------------- */

#define DP_MAX_TRACKED 4096
static volatile pid_t dp_tracked[DP_MAX_TRACKED];
static volatile int dp_ntracked;

__attribute__((unused))
static void dp_track(pid_t p)
{
    int i = dp_ntracked;
    if (i < DP_MAX_TRACKED) { dp_tracked[i] = p; dp_ntracked = i + 1; }
}

__attribute__((unused))
static void dp_kill_tracked(void)
{
    for (int i = 0; i < dp_ntracked; i++)
        if (dp_tracked[i] > 1) kill(dp_tracked[i], SIGKILL);
}

static void dp_watchdog_handler(int sig)
{
    (void)sig;
    dp_kill_tracked();
    static const char m[] = "FAIL WATCHDOG program exceeded its time budget, killed\n";
    ssize_t w = write(1, m, sizeof m - 1);
    (void)w;
    _exit(124);
}

/* Call at the start of main() of every witness (and every child role). */
__attribute__((unused))
static void dp_watchdog(unsigned seconds)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = dp_watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    alarm(seconds);
}

/* waitpid with a deadline; returns pid on exit/signal, 0 on timeout (child then killed). */
__attribute__((unused))
static pid_t dp_wait_timeout(pid_t pid, int *status, unsigned ms)
{
    uint64_t t0 = dp_ns();
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) return r;
        if (r < 0 && errno != EINTR) return -1;
        if ((dp_ns() - t0) / 1000000ull > ms) {
            kill(pid, SIGKILL);
            waitpid(pid, status, 0);
            return 0;
        }
        usleep(500);
    }
}

__attribute__((unused))
static const char *dp_status_str(int st, char *buf, size_t n)
{
    if (WIFEXITED(st)) snprintf(buf, n, "exit(%d)", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(buf, n, "signal(%d %s)%s", WTERMSIG(st), strsignal(WTERMSIG(st)),
                                       WCOREDUMP(st) ? " core" : "");
    else if (WIFSTOPPED(st)) snprintf(buf, n, "stopped(%d %s)", WSTOPSIG(st), strsignal(WSTOPSIG(st)));
    else snprintf(buf, n, "status(0x%x)", st);
    return buf;
}

/* Absolute path of the running executable, for self re-exec. */
#include <mach-o/dyld.h>
__attribute__((unused))
static const char *dp_self_path(void)
{
    static char path[4096];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) strcpy(path, "/nonexistent");
    return path;
}

/* Scratch directory: $TMPDIR (per-user temp) or /private/tmp as a last resort. */
__attribute__((unused))
static const char *dp_tmpdir(void)
{
    const char *t = getenv("DP_TMPDIR");
    if (!t || !*t) t = getenv("TMPDIR");
    if (!t || !*t) t = "/private/tmp";
    return t;
}

__attribute__((unused))
static int dp_summary(const char *prog)
{
    char b[256];
    int n = snprintf(b, sizeof b, "SUMMARY %s pass=%d fail=%d info=%d skip=%d\n", prog, dp_npass, dp_nfail, dp_ninfo, dp_nskip);
    ssize_t w = write(1, b, n);
    (void)w;
    return 0;
}

#endif
