/* trace.c: logging and syscall tracing. Owner: core.
 * Lines are formatted into a stack buffer and written with one write(2), so they never interleave and
 * the code needs no lock. Guest memory is never read here: pointers print as hex values. */
#include <stdarg.h>
#include <unistd.h>

#include "lkd_internal.h"
#include "task.h"

static _Atomic int g_trace;     /* 0 off, 1 completion lines, 2 entry and completion lines */
static _Atomic int g_log_enosys;

int lkd_trace_level(void) { return atomic_load_explicit(&g_trace, memory_order_relaxed); }
int lkd_enosys_logging(void) { return atomic_load_explicit(&g_log_enosys, memory_order_relaxed); }
void lkd_trace_enable(int on) { atomic_store(&g_trace, on); }
void lkd_trace_set_enosys_logging(int on) { atomic_store(&g_log_enosys, on); }

void lkd_trace_init_from_env(void)
{
    const char *v = getenv("LKD_TRACE");
    if (v && *v && strcmp(v, "0")) atomic_store(&g_trace, atoi(v) >= 2 ? 2 : 1);
    v = getenv("LKD_LOG_ENOSYS");
    if (v && *v && strcmp(v, "0")) atomic_store(&g_log_enosys, 1);
}

static void emit(const char *buf, int n)
{
    if (n <= 0) return;
    if (n > 1000) n = 1000;
    ssize_t r;
    do { r = write(2, buf, (size_t)n); } while (r < 0 && errno == EINTR);
}

void lkd_log(const char *fmt, ...)
{
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "lkd: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    emit(buf, n);
}

static int fmt_args(char *buf, size_t cap, const lkd_sysreq *rq)
{
    int last = -1;
    for (int i = 0; i < 6; i++) if (rq->a[i]) last = i;
    int n = 0;
    buf[0] = 0;
    for (int i = 0; i <= last; i++)
        n += snprintf(buf + n, cap - (size_t)n, "%s0x%llx", i ? ", " : "", (unsigned long long)rq->a[i]);
    return n;
}

void lkd_trace_entry(lkd_task *t, const lkd_sysreq *rq)
{
    char args[200], line[320];
    fmt_args(args, sizeof args, rq);
    const char *name = lkd_syscall_name(rq->nr);
    int n = snprintf(line, sizeof line, "[%d] -> %s(%s)\n", t ? t->tid : 0, name ? name : "?", args);
    emit(line, n);
}

void lkd_trace_exit(lkd_task *t, const lkd_sysreq *rq, int64_t ret)
{
    char args[200], res[64], line[360];
    fmt_args(args, sizeof args, rq);
    const char *name = lkd_syscall_name(rq->nr);
    if (ret < 0 && ret >= -LX_MAX_ERRNO) {
        const char *en = lkd_errno_name((int)-ret);
        snprintf(res, sizeof res, "%lld (%s)", (long long)ret, en ? en : "?");
    } else if (ret >= 0 && ret < (1ll << 32)) {
        snprintf(res, sizeof res, "%lld", (long long)ret);
    } else {
        snprintf(res, sizeof res, "0x%llx", (unsigned long long)ret);
    }
    char nm[24];
    if (!name) { snprintf(nm, sizeof nm, "syscall_%llu", (unsigned long long)rq->nr); name = nm; }
    int n = snprintf(line, sizeof line, "[%d] %s(%s) = %s\n", t ? t->tid : 0, name, args, res);
    emit(line, n);
}
