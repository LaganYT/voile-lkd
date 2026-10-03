/*
 * fg_util.h - helpers shared by the f_ and g_ witnesses (process, IPC, synchronization).
 * Builds on common.h (PASS/FAIL/INFO/SKIP lines, watchdog, timing). Header-only, static functions.
 */
#ifndef FG_UTIL_H
#define FG_UTIL_H

#include "common.h"
#include <dlfcn.h>
#include <setjmp.h>
#include <poll.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/un.h>

extern char **environ;

/* ---- errno names --------------------------------------------------------- */
__attribute__((unused))
static const char *fg_errname(int e)
{
    switch (e) {
#define E(x) case x: return #x;
    E(EPERM) E(ENOENT) E(ESRCH) E(EINTR) E(EIO) E(ENXIO) E(E2BIG) E(ENOEXEC) E(EBADF) E(ECHILD)
    E(EDEADLK) E(ENOMEM) E(EACCES) E(EFAULT) E(EBUSY) E(EEXIST) E(EXDEV) E(ENODEV) E(ENOTDIR)
    E(EISDIR) E(EINVAL) E(ENFILE) E(EMFILE) E(ENOTTY) E(ETXTBSY) E(EFBIG) E(ENOSPC) E(ESPIPE)
    E(EROFS) E(EMLINK) E(EPIPE) E(EDOM) E(ERANGE) E(EAGAIN) E(EINPROGRESS) E(EALREADY)
    E(ENOTSOCK) E(EDESTADDRREQ) E(EMSGSIZE) E(EPROTOTYPE) E(ENOPROTOOPT) E(EPROTONOSUPPORT)
    E(ESOCKTNOSUPPORT) E(ENOTSUP) E(EPFNOSUPPORT) E(EAFNOSUPPORT) E(EADDRINUSE) E(EADDRNOTAVAIL)
    E(ENETDOWN) E(ENETUNREACH) E(ECONNABORTED) E(ECONNRESET) E(ENOBUFS) E(EISCONN) E(ENOTCONN)
    E(ESHUTDOWN) E(ETIMEDOUT) E(ECONNREFUSED) E(ELOOP) E(ENAMETOOLONG) E(EHOSTUNREACH)
    E(ENOTEMPTY) E(EDQUOT) E(ESTALE) E(ENOLCK) E(ENOSYS) E(EOVERFLOW) E(ECANCELED) E(EILSEQ)
    E(EBADEXEC) E(EBADARCH) E(ESHLIBVERS) E(EBADMACHO) E(EOPNOTSUPP) E(ENOATTR) E(ENOPOLICY)
    E(EAUTH) E(ENEEDAUTH) E(EPWROFF) E(EDEVERR) E(ENOTRECOVERABLE) E(EOWNERDEAD)
#undef E
    case 0: return "0";
    default: return "E?";
    }
}

/* "EINVAL(22)" in a rotating static buffer, usable several times inside one printf. */
__attribute__((unused))
static const char *fg_err(int e)
{
    static char bufs[8][40];
    static int idx;
    char *b = bufs[idx++ & 7];
    snprintf(b, 40, "%s(%d)", fg_errname(e), e);
    return b;
}

#define ERRS(e) fg_err(e)

/* ---- load average, to judge how noisy a latency run was ------------------- */
__attribute__((unused))
static void fg_load_info(const char *id)
{
    double l[3] = {0, 0, 0};
    if (getloadavg(l, 3) < 0) l[0] = l[1] = l[2] = -1;
    INFO(id, "load average at start 1m=%.2f 5m=%.2f 15m=%.2f (other work may be running on this machine)", l[0], l[1], l[2]);
}

/* ---- fork a helper child, run fn there, wait with a deadline --------------- */
typedef int (*fg_child_fn)(void *);
enum { FG_OK = 0, FG_BADEXIT = 1, FG_SIGNALED = 2, FG_HUNG = 3, FG_FORKFAIL = 4 };

/*
 * Forks; the child sets dp_role="child", runs fn(arg) and _exit()s with its return value.
 * The parent waits up to timeout_ms. Returns an FG_* code; *status_out receives the raw wait status
 * (0 on hang or fork failure). A hung child is killed with SIGKILL by dp_wait_timeout.
 * Prints nothing itself: the caller decides how to describe the outcome.
 */
__attribute__((unused))
static int fg_run_in_child(fg_child_fn fn, void *arg, unsigned timeout_ms, int *status_out)
{
    int st = 0;
    pid_t pid = fork();
    if (pid < 0) { if (status_out) *status_out = 0; return FG_FORKFAIL; }
    if (pid == 0) {
        dp_role = "child";
        _exit(fn(arg) & 0xff);
    }
    dp_track(pid);
    pid_t r = dp_wait_timeout(pid, &st, timeout_ms);
    if (status_out) *status_out = st;
    if (r == 0) return FG_HUNG;
    if (r < 0) return FG_FORKFAIL;
    if (WIFSIGNALED(st)) return FG_SIGNALED;
    return WEXITSTATUS(st) == 0 ? FG_OK : FG_BADEXIT;
}

__attribute__((unused))
static const char *fg_outcome_str(int code, int st, char *buf, size_t n)
{
    char s[96];
    switch (code) {
    case FG_OK: snprintf(buf, n, "completed (exit 0)"); break;
    case FG_BADEXIT: snprintf(buf, n, "completed with %s", dp_status_str(st, s, sizeof s)); break;
    case FG_SIGNALED: snprintf(buf, n, "killed by %s", dp_status_str(st, s, sizeof s)); break;
    case FG_HUNG: snprintf(buf, n, "HUNG (no exit before the deadline, then SIGKILL)"); break;
    default: snprintf(buf, n, "fork failed"); break;
    }
    return buf;
}

/* ---- spawn this very binary with a role -------------------------------------- */
__attribute__((unused))
static pid_t fg_spawn_self(char *const argv[], const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr, int *err)
{
    pid_t pid = -1;
    int r = posix_spawn(&pid, dp_self_path(), fa, attr, argv, environ);
    if (err) *err = r;
    if (r != 0) return -1;
    dp_track(pid);
    return pid;
}

/* ---- misc ---------------------------------------------------------------- */
#define FG_PAGE 16384ul

__attribute__((unused))
static int fg_fd_is_open(int fd) { return fcntl(fd, F_GETFD) != -1; }

__attribute__((unused))
static void fg_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* stride-fill and stride-check a region, one 64-bit word every `stride` bytes. */
__attribute__((unused))
static void fg_stride_fill(void *p, size_t len, size_t stride, uint64_t seed)
{
    for (size_t off = 0; off + 8 <= len; off += stride) *(volatile uint64_t *)((char *)p + off) = seed ^ (uint64_t)off;
}

/* returns the number of words that do not match */
__attribute__((unused))
static size_t fg_stride_check(const void *p, size_t len, size_t stride, uint64_t seed)
{
    size_t bad = 0;
    for (size_t off = 0; off + 8 <= len; off += stride)
        if (*(volatile const uint64_t *)((const char *)p + off) != (seed ^ (uint64_t)off)) bad++;
    return bad;
}

/* temporary file name under the scratch dir; caller frees with free() */
__attribute__((unused))
static char *fg_tmpname(const char *tag)
{
    char *s = NULL;
    if (asprintf(&s, "%s/fg-%s-%d-XXXXXX", dp_tmpdir(), tag, (int)getpid()) < 0) return NULL;
    return s;
}


/* ---- safe memory probe: returns 1 if the byte can be read (or read+written), 0 on SIGSEGV/SIGBUS ---------- */
static sigjmp_buf fg_probe_jb;
static volatile sig_atomic_t fg_probe_active;
static void fg_probe_handler(int sig)
{
    if (fg_probe_active) siglongjmp(fg_probe_jb, sig);
    _exit(99);
}

__attribute__((unused))
static int fg_probe(const void *p, int write)
{
    struct sigaction sa, os, ob;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fg_probe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &os);
    sigaction(SIGBUS, &sa, &ob);
    int ok = 1;
    fg_probe_active = 1;
    if (sigsetjmp(fg_probe_jb, 1) == 0) {
        volatile unsigned char *q = (volatile unsigned char *)p;
        unsigned char c = *q;
        if (write) *q = c;
    } else ok = 0;
    fg_probe_active = 0;
    sigaction(SIGSEGV, &os, NULL);
    sigaction(SIGBUS, &ob, NULL);
    return ok;
}

#endif
