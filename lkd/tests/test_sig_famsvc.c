/* test_sig_famsvc.c: the family IPC service (src/include/famsvc.h) and cross-process signals.
 * Linux behaviour modelled for the signal cases: kill(2) and tgkill(2) reach a process or thread of
 * another process with an SI_USER/SI_TKILL siginfo carrying the sender's pid and uid
 * (kernel/signal.c prepare_kill_siginfo, do_tkill); kill to a pid that does not exist is ESRCH.
 * The service itself has no Linux counterpart: its cases check the contract of famsvc.h. */
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "famsvc.h"
#include "sig.h"

static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq r = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &r);
}
#define P(x) ((uint64_t)(uintptr_t)(x))

static int ping(pid_t pid, const char *payload, char *out, size_t cap, int timeout_ms)
{
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_PING;
    req.data = (void *)payload;
    req.len = payload ? strlen(payload) + 1 : 0;
    int r = lkd_famsvc_call((int32_t)pid, &req, &resp, timeout_ms);
    if (out && cap) {
        out[0] = 0;
        if (resp.data && resp.len <= cap) memcpy(out, resp.data, resp.len);
    }
    lkd_famsvc_msg_free(&resp);
    return r;
}

/* ---- handlers used by the tests (opcodes in the test range) ---------------------------------- */
static int32_t seen_caller_pid, seen_caller_tid, seen_host_pid, seen_target_pid;
static int nested_result = 12345;
static void op_who(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    seen_caller_pid = req->caller_pid;
    seen_caller_tid = req->caller_tid;
    seen_host_pid = req->caller_host_pid;
    seen_target_pid = req->target_pid;
    lkd_famsvc_msg r2, q2;
    memset(&q2, 0, sizeof q2);
    q2.op = LKD_FAMSVC_OP_PING;
    nested_result = lkd_famsvc_call(getpid(), &q2, &r2, 100); /* forbidden on the service thread */
    resp->status = 7;
}
static void op_slow(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    (void)req;
    usleep(300000);
    resp->status = 1;
}
static void op_big(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    size_t n = req->len == 8 ? (size_t)*(uint64_t *)req->data : 0;
    resp->data = malloc(n);
    for (size_t i = 0; i < n; i++) ((unsigned char *)resp->data)[i] = (unsigned char)(i * 7);
    resp->len = n;
    resp->status = 0;
}

LT_TEST(ping_self_and_payload)
{
    char out[64];
    LT_CHECK(lkd_famsvc_running());
    LT_CHECK(lkd_famsvc_dir() != NULL);
    LT_EQ(ping(getpid(), "hello", out, sizeof out, 0), getpid());
    LT_STREQ(out, "hello");
    LT_EQ(ping(getpid(), NULL, NULL, 0, 0), getpid());
    char path[256];
    snprintf(path, sizeof path, "%s/%d.s", lkd_famsvc_dir(), (int)getpid());
    struct stat st;
    LT_EQ(stat(path, &st), 0);
    LT_CHECK(S_ISSOCK(st.st_mode));
    LT_EQ(stat(lkd_famsvc_dir(), &st), 0);
    LT_EQ(st.st_mode & 0777, 0700);
}

LT_TEST(large_payload_both_ways)
{
    uint64_t n = 3u << 20;
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_TEST_FIRST + 2;
    req.data = &n;
    req.len = sizeof n;
    LT_EQ(lkd_famsvc_call(getpid(), &req, &resp, 0), 0);
    LT_EQ(resp.len, n);
    int ok = resp.data != NULL;
    for (size_t i = 0; ok && i < n; i++) ok = ((unsigned char *)resp.data)[i] == (unsigned char)(i * 7);
    LT_CHECK(ok);
    lkd_famsvc_msg_free(&resp);
    /* request payload of 6 MiB echoed by PING */
    size_t m = 6u << 20;
    char *big = malloc(m);
    for (size_t i = 0; i < m; i++) big[i] = (char)(i ^ (i >> 9));
    req.op = LKD_FAMSVC_OP_PING;
    req.data = big;
    req.len = m;
    LT_EQ(lkd_famsvc_call(getpid(), &req, &resp, 0), getpid());
    LT_EQ(resp.len, m);
    LT_CHECK(resp.data && memcmp(resp.data, big, m) == 0);
    lkd_famsvc_msg_free(&resp);
    req.len = LKD_FAMSVC_MAX_PAYLOAD + 1;
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 0), LX_E2BIG);
    free(big);
}

LT_TEST(descriptor_passing)
{
    int p[2];
    LT_EQ(pipe(p), 0);
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_PING;
    req.fds[0] = p[1];
    req.nfds = 1;
    LT_EQ(lkd_famsvc_call(getpid(), &req, &resp, 0), getpid());
    LT_EQ(resp.nfds, 1);
    LT_CHECK(resp.fds[0] >= 0 && resp.fds[0] != p[1]);
    LT_EQ(fcntl(resp.fds[0], F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
    LT_EQ(write(resp.fds[0], "z", 1), 1);
    char c = 0;
    LT_EQ(read(p[0], &c, 1), 1);
    LT_EQ(c, 'z');
    lkd_famsvc_msg_free(&resp);
    LT_EQ(resp.nfds, 0);
    req.nfds = LKD_FAMSVC_MAX_FDS + 1;
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 0), LX_E2BIG);
    close(p[0]);
    close(p[1]);
}

LT_TEST(identity_unknown_op_and_nesting)
{
    lkd_task *t = fake_make_task();
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_TEST_FIRST;
    req.target_pid = 4242;
    LT_EQ(lkd_famsvc_call(getpid(), &req, &resp, 0), 7);
    LT_EQ(seen_caller_pid, t->proc->pid);
    LT_EQ(seen_caller_tid, t->tid);
    LT_EQ(seen_host_pid, getpid());
    LT_EQ(seen_target_pid, 4242);
    LT_ERRNO(nested_result, LX_EINVAL);
    req.op = LKD_FAMSVC_OP_TEST_FIRST + 5; /* no handler */
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 0), LX_ENOSYS);
    req.op = 0;
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 0), LX_EINVAL);
    req.op = LKD_FAMSVC_OP_MAX;
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 0), LX_EINVAL);
    LT_ERRNO(lkd_famsvc_register(LKD_FAMSVC_OP_PING, op_slow), LX_EEXIST);
    LT_ERRNO(lkd_famsvc_register(0, op_slow), LX_EINVAL);
}

LT_TEST(no_service_is_esrch)
{
    char out[8];
    LT_ERRNO(ping(1, "x", out, sizeof out, 500), LX_ESRCH);
    /* a child that exited: its pid has no socket */
    pid_t c = fork();
    if (c == 0) _exit(0);
    waitpid(c, NULL, 0);
    LT_ERRNO(ping(c, "x", out, sizeof out, 500), LX_ESRCH);
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_PING;
    LT_ERRNO(lkd_famsvc_call_pid(987654, &req, &resp, 0), LX_ESRCH);
}

LT_TEST(timeout_then_recovers)
{
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_TEST_FIRST + 1;
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(lkd_famsvc_call(getpid(), &req, &resp, 100), LX_ETIMEDOUT);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 95000000 && dt < 280000000);
    struct lkd_famsvc_stats st;
    lkd_famsvc_get_stats(&st);
    LT_CHECK(st.timeouts >= 1);
    char out[8];
    LT_EQ(ping(getpid(), "ok", out, sizeof out, 2000), getpid()); /* served after the slow handler */
    LT_STREQ(out, "ok");
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static void measure(const char *what, pid_t pid, int n, int64_t *median_ns)
{
    int64_t *v = malloc(sizeof *v * (size_t)n);
    for (int i = 0; i < n; i++) {
        int64_t t0 = lkd_host_mono_ns();
        (void)ping(pid, "rtt", NULL, 0, 0);
        v[i] = lkd_host_mono_ns() - t0;
    }
    qsort(v, (size_t)n, sizeof *v, cmp_i64);
    printf("MEASURE famsvc %s: n=%d p50=%.2f us p90=%.2f us p99=%.2f us\n", what, n, v[n / 2] / 1000.0,
           v[n * 9 / 10] / 1000.0, v[n * 99 / 100] / 1000.0);
    *median_ns = v[n / 2];
    free(v);
}

/* ---- cross-process: lkd_fork_host child serves its own socket; signals both ways ---------------- */
struct xp { int to_child[2], to_parent[2]; };

LT_TEST(fork_child_service_and_cross_signals)
{
    lkd_task *t = fake_make_task();
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER | LX_SA_SIGINFO, 0x2000, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&h), 0, 8, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR2, P(&h), 0, 8, 0, 0), 0);
    lkd_sig_set_mask(t, LX_SIGBIT(LX_SIGUSR1) | LX_SIGBIT(LX_SIGUSR2));
    int32_t ppid_guest = t->proc->pid;
    int32_t ptid = t->tid;
    struct xp x;
    LT_EQ(pipe(x.to_child), 0);
    LT_EQ(pipe(x.to_parent), 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(t, 0);
    if (c == 0) {
        alarm(20);
        /* child: t now has the child identity */
        int32_t me = t->proc->pid;
        int ok = lkd_famsvc_running() && t->proc->pid != ppid_guest;
        char out[16];
        int pr = ping(getppid(), "up", out, sizeof out, 2000);
        ok = ok && pr == getppid() && strcmp(out, "up") == 0;
        int k1 = lkd_sig_send_pid(t, ppid_guest, LX_SIGUSR1, NULL);           /* kill to the parent */
        int k2 = lkd_sig_send_tid(t, ppid_guest, ptid, LX_SIGUSR2, NULL);     /* tgkill to its thread */
        int k3 = lkd_sig_send_pid(t, 999999, LX_SIGUSR1, NULL);               /* nobody */
        int32_t msg[5] = { me, ok, k1, k2, k3 };
        (void)!write(x.to_parent[1], msg, sizeof msg);
        /* wait for SIGUSR2 from the parent, then report */
        uint64_t set = LX_SIGBIT(LX_SIGUSR2);
        struct lx_siginfo si;
        struct lx_timespec ts = { 5, 0 };
        int64_t got = sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0);
        int32_t r2[3] = { (int32_t)got, si.si_code, si.u.kill.pid };
        /* become another user: the parent may no longer signal us (checked in this process) */
        t->proc->cred.uid = t->proc->cred.euid = t->proc->cred.suid = 4242;
        (void)!write(x.to_parent[1], r2, sizeof r2);
        char done;
        (void)!read(x.to_child[0], &done, 1);
        _exit(0);
    }
    LT_CHECK(c > 0);
    int32_t msg[5] = { 0 };
    LT_EQ(read(x.to_parent[0], msg, sizeof msg), (ssize_t)sizeof msg);
    int32_t child_guest = msg[0];
    LT_EQ(msg[1], 1);
    LT_EQ(msg[2], 0);
    LT_EQ(msg[3], 0);
    LT_ERRNO(msg[4], LX_ESRCH);
    /* the two signals are queued in the parent with the child's identity */
    uint64_t set = LX_SIGBIT(LX_SIGUSR1) | LX_SIGBIT(LX_SIGUSR2);
    struct lx_siginfo si;
    struct lx_timespec ts = { 2, 0 };
    /* dequeue_signal takes the thread-directed one (tgkill) before the process-directed one */
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0), LX_SIGUSR2);
    LT_EQ(si.si_code, LX_SI_TKILL);
    LT_EQ(si.u.kill.pid, child_guest);
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0), LX_SIGUSR1);
    LT_EQ(si.si_code, LX_SI_USER);
    LT_EQ(si.u.kill.pid, child_guest);
    /* the child's own service answers with its host pid */
    char out[16];
    LT_EQ(ping(c, "c", out, sizeof out, 2000), c);
    int64_t med_self = 0, med_child = 0;
    measure("self", getpid(), 2000, &med_self);
    measure("other process", c, 2000, &med_child);
    LT_CHECK(med_child > 0 && med_child < 2000000);
    /* parent -> child signal through the child's service */
    LT_EQ(lkd_sig_send_pid(t, child_guest, LX_SIGUSR2, NULL), 0);
    int32_t r2[3] = { 0 };
    LT_EQ(read(x.to_parent[0], r2, sizeof r2), (ssize_t)sizeof r2);
    LT_EQ(r2[0], LX_SIGUSR2);
    LT_EQ(r2[1], LX_SI_USER);
    LT_EQ(r2[2], ppid_guest);
    /* check_kill_permission runs in the target process with the sender's credentials */
    LT_ERRNO(lkd_sig_send_pid(t, child_guest, LX_SIGUSR1, NULL), LX_EPERM);
    LT_ERRNO(lkd_sig_send_pid(t, child_guest, 0, NULL), LX_EPERM);
    LT_EQ(lkd_sig_send_pid(t, child_guest, LX_SIGCONT, NULL), 0); /* same session */
    LT_EQ(lkd_sig_send_pid(NULL, child_guest, 0, NULL), 0);       /* kernel-originated: no check */
    (void)!write(x.to_child[1], "d", 1);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    LT_ERRNO(ping(c, "x", out, sizeof out, 500), LX_ESRCH); /* gone: stale socket refuses */
    close(x.to_child[0]); close(x.to_child[1]); close(x.to_parent[0]); close(x.to_parent[1]);
}

/* ---- host re-exec: the listening socket survives through LKD_FAMSVC_FD ------------------------ */
static int exec_adopt_main(int report_fd)
{
    alarm(10);
    int rc = fake_engine_init();
    struct lkd_famsvc_stats st;
    lkd_famsvc_get_stats(&st);
    char r = (rc == 0 && lkd_famsvc_running() && st.adopted == 1) ? 'A' : 'N';
    if (r != 'A') fprintf(stderr, "exec-adopt: rc=%d running=%d adopted=%llu env=%s\n", rc, lkd_famsvc_running(),
                          (unsigned long long)st.adopted, getenv("LKD_FAMSVC_FD") ? getenv("LKD_FAMSVC_FD") : "(none)");
    (void)!write(report_fd, &r, 1);
    /* serve until the parent closes the pipe (read returns 0 when the write end is gone) */
    char c;
    (void)!read(report_fd + 1, &c, 1);
    _exit(0);
}

LT_TEST(exec_keeps_the_service)
{
    lkd_task *t = fake_make_task();
    int rep[2], hold[2];
    LT_EQ(pipe(rep), 0);
    LT_EQ(pipe(hold), 0);
    for (int i = 0; i < 2; i++) { fcntl(rep[i], F_SETFD, FD_CLOEXEC); fcntl(hold[i], F_SETFD, FD_CLOEXEC); }
    /* fixed descriptor numbers for the exec'ed image: report at 200, hold at 201 */
    LT_EQ(dup2(rep[1], 200), 200);
    LT_EQ(dup2(hold[0], 201), 201);
    char self[4096];
    uint32_t sz = sizeof self;
    LT_EQ(_NSGetExecutablePath(self, &sz), 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(t, 0);
    if (c == 0) {
        /* ping ourselves before the exec: the connection waits in the backlog */
        execl(self, self, "--exec-adopt", (char *)NULL);
        _exit(127);
    }
    close(200);
    close(201);
    char out[8];
    int r = -1;
    for (int i = 0; i < 100 && r != c; i++) { r = ping(c, "e", out, sizeof out, 2000); if (r != c) usleep(20000); }
    LT_EQ(r, c);
    char rep_c = 0;
    LT_EQ(read(rep[0], &rep_c, 1), 1);
    LT_EQ(rep_c, 'A');
    close(hold[1]);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(rep[0]); close(rep[1]); close(hold[0]);
}

LT_TEST(shutdown_removes_directory)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", lkd_famsvc_dir());
    lkd_famsvc_shutdown();
    LT_EQ(lkd_famsvc_running(), 0);
    struct stat st;
    LT_EQ(stat(dir, &st), -1); /* no live member left: the directory is gone */
    LT_EQ(lkd_famsvc_start(), 0); /* restartable */
    LT_EQ(ping(getpid(), NULL, NULL, 0, 0), getpid());
    lkd_famsvc_shutdown();
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--exec-adopt") == 0) return exec_adopt_main(200);
    LT_EQ(lkd_famsvc_register(LKD_FAMSVC_OP_TEST_FIRST, op_who), 0);
    LT_EQ(lkd_famsvc_register(LKD_FAMSVC_OP_TEST_FIRST + 1, op_slow), 0);
    LT_EQ(lkd_famsvc_register(LKD_FAMSVC_OP_TEST_FIRST + 2, op_big), 0);
    LT_EQ(fake_engine_init(), 0);
    LT_RUN(ping_self_and_payload);
    LT_RUN(large_payload_both_ways);
    LT_RUN(descriptor_passing);
    LT_RUN(identity_unknown_op_and_nesting);
    LT_RUN(no_service_is_esrch);
    LT_RUN(timeout_then_recovers);
    LT_RUN(fork_child_service_and_cross_signals);
    LT_RUN(exec_keeps_the_service);
    LT_RUN(shutdown_removes_directory);
    return lt_finish();
}
