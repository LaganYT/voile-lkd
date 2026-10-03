/*
 * h2_ptrace.c - H: what ptrace(2) offers on this kernel between a process and its own get-task-allow children.
 *
 *   H2.attachexc  PT_ATTACHEXC on a running child, how the stop is reported, PT_CONTINUE, PT_DETACH
 *   H2.requests   every request number tried on the attached child: return value and errno
 *   H2.step       PT_STEP: does it single-step, which thread, how long per step
 *   H2.traceme    PT_TRACE_ME and a signal-delivery stop, continued with and without the signal
 *   H2.exec       PT_TRACE_ME then exec: is there an exec stop
 *   H2.deny       PT_DENY_ATTACH in the child, then PT_ATTACHEXC from the parent
 *   H2.sibling    PT_ATTACHEXC from a sibling process (not the parent), reparenting
 *   H2.attach     the deprecated PT_ATTACH (request 10)
 * No task_for_pid is used except in H2.step to read the program counter of the stepped child
 * (the child is an own get-task-allow child; see h1_task_inspect for the same call).
 */
#include "hi_debuggee.h"

static const char *errname(int e) { return e ? strerror(e) : "ok"; }

static pid_t ppid_of(pid_t pid)
{
    struct proc_bsdshortinfo si;
    int n = proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &si, PROC_PIDT_SHORTBSDINFO_SIZE);
    return n == (int)PROC_PIDT_SHORTBSDINFO_SIZE ? (pid_t)si.pbsi_ppid : -1;
}

static int spawn_dbg(struct hi_child *c, struct hi_dbg_info *d, const char *id)
{
    int rc = hi_spawn_debuggee(c, d);
    if (rc) FAIL(id, "debuggee did not start (%d)", rc);
    return rc;
}

static void bury(struct hi_child *c)
{
    int st;
    kill(c->pid, SIGKILL);
    dp_wait_timeout(c->pid, &st, 2000);
    close(c->wfd); close(c->rfd);
}

/* Is the child (all threads) frozen? Sends a PING and waits for the answer for a short time. */
static int answers_ping(struct hi_child *c, int ms, int *pending)
{
    struct hi_msg ping, r;
    if (!*pending) {
        memset(&ping, 0, sizeof ping); ping.cmd = DC_PING;
        hi_write_full(c->wfd, &ping, sizeof ping);
        *pending = 1;
    }
    if (hi_read_timeout(c->rfd, &r, sizeof r, ms) == 0) { *pending = 0; return 1; }
    return 0;
}

/* ---- H2.attachexc + H2.requests ------------------------------------------------ */
static const char *reqname(int r)
{
    switch (r) {
    case 0: return "PT_TRACE_ME"; case 1: return "PT_READ_I"; case 2: return "PT_READ_D"; case 3: return "PT_READ_U";
    case 4: return "PT_WRITE_I"; case 5: return "PT_WRITE_D"; case 6: return "PT_WRITE_U"; case 7: return "PT_CONTINUE";
    case 8: return "PT_KILL"; case 9: return "PT_STEP"; case 10: return "PT_ATTACH"; case 11: return "PT_DETACH";
    case 12: return "PT_SIGEXC"; case 13: return "PT_THUPDATE"; case 14: return "PT_ATTACHEXC";
    case 30: return "PT_FORCEQUOTA"; case 31: return "PT_DENY_ATTACH"; case 32: return "PT_FIRSTMACH";
    default: return "(unnamed)";
    }
}

static void t_attachexc(void)
{
    struct hi_child c; struct hi_dbg_info d;
    if (spawn_dbg(&c, &d, "H2.attachexc")) return;
    int pending = 0;
    CHECK(answers_ping(&c, 1000, &pending), "H2.attachexc", "child answers before the attach");

    errno = 0;
    uint64_t t0 = dp_ns();
    int rc = ptrace(PT_ATTACHEXC, c.pid, 0, 0);
    int e = errno;
    double us = (dp_ns() - t0) / 1e3;
    INFO("H2.attachexc", "ptrace(PT_ATTACHEXC, child) -> %d errno=%d (%s) in %.1f us", rc, e, errname(e), us);
    if (rc != 0) { FAIL("H2.attachexc", "attach refused; skipping the other ptrace tests on an attached child"); bury(&c); return; }
    int st = 0; char b[128];
    t0 = dp_ns();
    pid_t w = hi_wait_stop(c.pid, &st, 2000);
    INFO("H2.attachexc", "waitpid(WUNTRACED) -> %d %s after %.1f us", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout", (dp_ns() - t0) / 1e3);
    CHECK(w == c.pid && WIFSTOPPED(st), "H2.attachexc", "the attach is reported as a stop (SIGSTOP model), as with Linux PTRACE_ATTACH");
    pending = 0;
    {
        int a = answers_ping(&c, 300, &pending);
        INFO("H2.attachexc", "after the attach stop the other threads of the child %s (server thread %s a PING): %s", a ? "keep running" : "are stopped", a ? "answers" : "does not answer",
             a ? "SIGSTOP in exception mode stops only the thread that took the signal" : "whole-process stop");
    }
    INFO("H2.attachexc", "ppid of the tracee while traced by its parent: %d (parent is %d)", (int)ppid_of(c.pid), (int)getpid());

    /* request matrix, child stopped */
    for (int req = 1; req <= 40; req++) {
        if (req == 7 || req == 8 || req == 9 || req == 11 || req == 12 || req == 14 || req == 31) continue;   /* tested elsewhere or unsafe in the caller */
        errno = 0;
        long r = ptrace(req, c.pid, (caddr_t)(uintptr_t)d.buf_addr, 0x42);
        int e2 = errno;
        if (r == -1 && e2) INFO("H2.requests", "%-14s (%2d) -> -1 errno=%d (%s)", reqname(req), req, e2, strerror(e2));
        else INFO("H2.requests", "%-14s (%2d) -> %ld errno=0 (accepted)", reqname(req), req, r);
    }

    /* continue and detach */
    errno = 0;
    rc = ptrace(PT_CONTINUE, c.pid, (caddr_t)1, 0);
    INFO("H2.attachexc", "ptrace(PT_CONTINUE, addr=1, data=0) -> %d errno=%d (%s)", rc, errno, errname(errno));
    CHECK(answers_ping(&c, 1000, &pending), "H2.attachexc", "child runs again after PT_CONTINUE");

    /* a signal while traced in exception mode */
    kill(c.pid, SIGUSR1);
    st = 0;
    w = hi_wait_stop(c.pid, &st, 1500);
    INFO("H2.attachexc", "SIGUSR1 sent to the attached (PT_ATTACHEXC) child: waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "no stop reported");
    if (w == c.pid && WIFSTOPPED(st)) {
        errno = 0;
        rc = ptrace(PT_CONTINUE, c.pid, (caddr_t)1, 0);
        INFO("H2.attachexc", "PT_CONTINUE(data=0) after that stop -> %d errno=%d (%s) : in exception mode the stop is a Mach exception, resumed by replying to it, not by ptrace", rc, errno, errname(errno));
        int a = answers_ping(&c, 500, &pending);
        INFO("H2.attachexc", "child answers a PING after that: %d", a);
    }
    errno = 0;
    rc = ptrace(PT_DETACH, c.pid, 0, 0);
    INFO("H2.attachexc", "ptrace(PT_DETACH) -> %d errno=%d (%s)", rc, errno, errname(errno));
    INFO("H2.attachexc", "child answers a PING after PT_DETACH: %d", answers_ping(&c, 1000, &pending));
    bury(&c);
}

/* ---- H2.step ---------------------------------------------------------------- */
struct tfp_arg { int pid; mach_port_t task; kern_return_t kr; };
static void *tfp_worker(void *p) { struct tfp_arg *a = p; a->kr = task_for_pid(mach_task_self(), a->pid, &a->task); return NULL; }

static void t_step_core(const char *tag, int classic)
{
    struct hi_child c; struct hi_dbg_info d;
    if (spawn_dbg(&c, &d, tag)) return;
    struct tfp_arg ta = {.pid = c.pid};
    if (!hi_call_with_deadline(tfp_worker, &ta, 5000, NULL) || ta.kr) { FAIL(tag, "task_for_pid failed, cannot read the pc: skipping"); bury(&c); return; }
    int rc, st; char b[128]; pid_t w;
    if (classic) {
        struct hi_msg r;
        hi_call(&c, DC_PTRACE_ME, 0, 0, 0, &r, 2000);
        hi_call(&c, DC_RAISE, SIGUSR1, 0, 0, &r, 2000);
        w = hi_wait_stop(c.pid, &st, 2000);
        if (w != c.pid || !WIFSTOPPED(st)) { FAIL(tag, "no stop after PT_TRACE_ME + SIGUSR1"); bury(&c); return; }
        int pending = 0;
        INFO(tag, "classic mode signal stop: other threads %s", answers_ping(&c, 300, &pending) ? "keep running (thread-level stop)" : "are stopped (whole-process stop)");
    } else {
        errno = 0;
        rc = ptrace(PT_ATTACHEXC, c.pid, 0, 0);
        if (rc) { FAIL(tag, "attach refused (%s)", strerror(errno)); bury(&c); return; }
        w = hi_wait_stop(c.pid, &st, 2000);
        if (w != c.pid) { FAIL(tag, "no stop after attach"); bury(&c); return; }
    }

    thread_act_array_t th; mach_msg_type_number_t nth;
    task_threads(ta.task, &th, &nth);
    thread_act_t spin = MACH_PORT_NULL;
    for (unsigned i = 0; i < nth; i++) {
        thread_identifier_info_data_t id; mach_msg_type_number_t cc = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &cc) == 0 && id.thread_id == d.main_tid) spin = th[i];
    }
    arm_thread_state64_t s0; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    thread_get_state(spin, ARM_THREAD_STATE64, (thread_state_t)&s0, &cnt);
    uintptr_t pc0 = (uintptr_t)arm_thread_state64_get_pc(s0);
    INFO(tag, "stopped at pc=0x%llx (spin loop is [0x%llx,0x%llx), 3 instructions)", (unsigned long long)pc0, (unsigned long long)d.spin_lo, (unsigned long long)d.spin_hi);

    errno = 0;
    rc = ptrace(PT_STEP, c.pid, (caddr_t)1, 0);
    int e = errno;
    INFO(tag, "ptrace(PT_STEP, addr=1, data=0) -> %d errno=%d (%s)", rc, e, errname(e));
    if (rc != 0) { FAIL(tag, "PT_STEP not accepted, no ptrace single-step on this kernel for this attach mode"); bury(&c); return; }
    uint64_t t0 = dp_ns();
    w = hi_wait_stop(c.pid, &st, 2000);
    INFO(tag, "waitpid after PT_STEP -> %d %s (%.1f us)", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout: no stop report", (dp_ns() - t0) / 1e3);
    if (w == c.pid && WIFSTOPPED(st)) {
        cnt = ARM_THREAD_STATE64_COUNT;
        thread_get_state(spin, ARM_THREAD_STATE64, (thread_state_t)&s0, &cnt);
        uintptr_t pc1 = (uintptr_t)arm_thread_state64_get_pc(s0);
        INFO(tag, "pc after one PT_STEP = 0x%llx (moved by %lld bytes)", (unsigned long long)pc1, (long long)(pc1 - pc0));
        int ok = (pc1 != pc0) && (labs((long)(pc1 - pc0)) <= 8);
        CHECK(ok, tag, "PT_STEP advanced the main thread by about one instruction");

        enum { N = 5000 };
        uint64_t *v = malloc(sizeof(uint64_t) * N);
        int n = 0, moved_ok = 0;
        uintptr_t prev = pc1;
        for (; n < N; n++) {
            t0 = dp_ns();
            errno = 0;
            if (ptrace(PT_STEP, c.pid, (caddr_t)1, 0) != 0) { INFO(tag, "PT_STEP number %d failed errno=%d (%s)", n + 2, errno, strerror(errno)); break; }
            pid_t ww;
            do { ww = waitpid(c.pid, &st, WUNTRACED); } while (ww < 0 && errno == EINTR);
            v[n] = dp_ns() - t0;
            if (ww != c.pid || !WIFSTOPPED(st)) break;
            if (n % 50 == 0) {
                cnt = ARM_THREAD_STATE64_COUNT;
                thread_get_state(spin, ARM_THREAD_STATE64, (thread_state_t)&s0, &cnt);
                uintptr_t pcn = (uintptr_t)arm_thread_state64_get_pc(s0);
                if (pcn >= d.spin_lo && pcn < d.spin_hi && pcn != prev) moved_ok++;
                prev = pcn;
            }
        }
        INFO(tag, "%d consecutive PT_STEP + waitpid completed; pc moved inside the spin loop at %d of %d samples", n, moved_ok, n / 50 + 1);
        if (n > 10) hi_report_us(tag, "PT_STEP + waitpid round trip", v, n);
        free(v);
    }
    ptrace(PT_DETACH, c.pid, 0, 0);
    bury(&c);
}

static void t_step(void) { t_step_core("H2.step.exc", 0); t_step_core("H2.step.classic", 1); }

/* ---- H2.traceme ---------------------------------------------------------------- */
static void t_traceme(int pass_signal)
{
    const char *id = pass_signal ? "H2.traceme+pass" : "H2.traceme";
    struct hi_child c; struct hi_dbg_info d;
    if (spawn_dbg(&c, &d, id)) return;
    struct hi_msg r;
    hi_call(&c, DC_PTRACE_ME, 0, 0, 0, &r, 2000);
    INFO(id, "PT_TRACE_ME in the child -> rc=%d errno=%llu", r.rc, (unsigned long long)r.a[0]);
    hi_call(&c, DC_RAISE, SIGUSR1, 0, 0, &r, 2000);
    int st = 0; char b[128];
    pid_t w = hi_wait_stop(c.pid, &st, 2000);
    INFO(id, "after raise(SIGUSR1): waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout");
    CHECK(w == c.pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGUSR1, id, "signal-delivery-stop reported to the tracer with the signal number (Linux model)");
    {
        int pend = 0;
        INFO(id, "at that stop the other threads of the tracee %s", answers_ping(&c, 300, &pend) ? "keep running (the server thread answers a PING)" : "are stopped (whole-process stop)");
        if (pend) { /* drain: the answer comes after the continue */ }
    }
    errno = 0;
    int rc = ptrace(PT_CONTINUE, c.pid, (caddr_t)1, pass_signal ? SIGUSR1 : 0);
    INFO(id, "PT_CONTINUE(data=%d) -> %d errno=%d", pass_signal ? SIGUSR1 : 0, rc, errno);
    w = hi_wait_stop(c.pid, &st, 1500);
    if (pass_signal) {
        INFO(id, "child after the signal was passed on: waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "still running");
        CHECK(w == c.pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1, id, "passing the signal at the stop delivers it (default action: terminate)");
    } else {
        int pending = 0;
        CHECK(w == 0 && answers_ping(&c, 1000, &pending), id, "suppressing the signal at the stop keeps the child alive (waitpid=%d)", (int)w);
    }
    bury(&c);
}

/* ---- H2.exec ------------------------------------------------------------------ */
static void t_exec(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PT_TRACE_ME, 0, 0, 0);
        execl(dp_self_path(), dp_self_path(), "noop", (char *)NULL);
        _exit(127);
    }
    dp_track(pid);
    int st = 0; char b[128];
    pid_t w = hi_wait_stop(pid, &st, 3000);
    INFO("H2.exec", "child did PT_TRACE_ME then execve: waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout");
    CHECK(w == pid && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP, "H2.exec", "exec is reported as a SIGTRAP stop (PTRACE_O_TRACEEXEC-like event exists only as plain SIGTRAP, no event code)");
    if (w == pid && WIFSTOPPED(st)) {
        ptrace(PT_CONTINUE, pid, (caddr_t)1, 0);
        w = hi_wait_stop(pid, &st, 3000);
        INFO("H2.exec", "after PT_CONTINUE: waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout");
    }
    kill(pid, SIGKILL);
    dp_wait_timeout(pid, &st, 1000);
}

/* ---- H2.deny ------------------------------------------------------------------- */
/* The kernel is known to answer an attach to a PT_DENY_ATTACH process by signalling the tracer (SIGSEGV),
 * so the attempt is made by a sibling process B and the result is read from B's wait status. */
static void t_deny(void)
{
    struct hi_child a, b; struct hi_dbg_info da, db;
    if (spawn_dbg(&a, &da, "H2.deny") || spawn_dbg(&b, &db, "H2.deny")) return;
    struct hi_msg r;
    hi_call(&a, DC_DENY_ATTACH, 0, 0, 0, &r, 2000);
    INFO("H2.deny", "PT_DENY_ATTACH in A -> rc=%d errno=%llu", r.rc, (unsigned long long)r.a[0]);
    int rc = hi_call(&b, DC_PTRACE, PT_ATTACHEXC, a.pid, 0, &r, 2000);
    char bb[128]; int st = 0;
    pid_t w = hi_wait_stop(b.pid, &st, 1500);
    if (rc == 0)
        INFO("H2.deny", "B (sibling tracer) calls PT_ATTACHEXC on A, which denied attach: call returned rc=%lld errno=%llu (%s) and B is %s", (long long)r.a[1], (unsigned long long)r.a[0], errname((int)r.a[0]),
             w == b.pid ? dp_status_str(st, bb, sizeof bb) : "still alive");
    else
        INFO("H2.deny", "B (sibling tracer) calls PT_ATTACHEXC on A, which denied attach: B never answered; B wait status: %s", w == b.pid ? dp_status_str(st, bb, sizeof bb) : "still alive");
    CHECK(w == b.pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "H2.deny", "the tracer that attaches to a PT_DENY_ATTACH process is killed by SIGSEGV (kernel signals the tracer): any host-side 'anti-debug' flag on a Wine process would take the tracer down");
    struct tfp_arg ta = {.pid = a.pid};
    if (hi_call_with_deadline(tfp_worker, &ta, 5000, NULL)) INFO("H2.deny", "task_for_pid on A after PT_DENY_ATTACH %s", ta.kr ? "refused" : "still succeeds (the flag only affects ptrace)");
    bury(&a); bury(&b);
}

/* ---- H2.sibling ------------------------------------------------------------------ */
static void t_sibling(void)
{
    struct hi_child a, b; struct hi_dbg_info da, db;
    if (spawn_dbg(&a, &da, "H2.sibling") || spawn_dbg(&b, &db, "H2.sibling")) return;
    struct hi_msg r;
    INFO("H2.sibling", "A=%d B=%d, both children of %d; ppid(A)=%d", (int)a.pid, (int)b.pid, (int)getpid(), (int)ppid_of(a.pid));
    hi_call(&b, DC_PTRACE, PT_ATTACHEXC, a.pid, 0, &r, 3000);
    INFO("H2.sibling", "B calls ptrace(PT_ATTACHEXC, A) -> rc=%lld errno=%llu (%s)", (long long)r.a[1], (unsigned long long)r.a[0], errname((int)r.a[0]));
    if ((int64_t)r.a[1] == 0) {
        hi_call(&b, DC_WAITPID, a.pid, WUNTRACED, 0, &r, 3000);
        char bb[128];
        INFO("H2.sibling", "B waitpid(A, WUNTRACED) -> %lld %s errno=%llu", (long long)r.a[0], (long long)r.a[0] > 0 ? dp_status_str((int)r.a[1], bb, sizeof bb) : "-", (unsigned long long)r.a[2]);
        INFO("H2.sibling", "ppid(A) while traced by B = %d (B is %d, original parent %d)", (int)ppid_of(a.pid), (int)b.pid, (int)getpid());
        int st = 0;
        pid_t w = waitpid(a.pid, &st, WNOHANG | WUNTRACED);
        INFO("H2.sibling", "original parent waitpid(A, WNOHANG|WUNTRACED) while A is traced by B -> %d errno=%d", (int)w, w < 0 ? errno : 0);
        hi_call(&b, DC_PTRACE, PT_DETACH, a.pid, 0, &r, 3000);
        INFO("H2.sibling", "B PT_DETACH -> rc=%lld errno=%llu ; ppid(A) after detach = %d", (long long)r.a[1], (unsigned long long)r.a[0], (int)ppid_of(a.pid));
    }
    bury(&a); bury(&b);
}

/* ---- H2.attach (deprecated request 10) ------------------------------------------ */
static void t_attach_old(void)
{
    struct hi_child c; struct hi_dbg_info d;
    if (spawn_dbg(&c, &d, "H2.attach")) return;
    errno = 0;
    int rc = ptrace(10, c.pid, 0, 0);
    int e = errno;
    INFO("H2.attach", "ptrace(10 = PT_ATTACH, child) -> %d errno=%d (%s)", rc, e, errname(e));
    if (rc == 0) {
        int st = 0; char b[128];
        pid_t w = hi_wait_stop(c.pid, &st, 2000);
        INFO("H2.attach", "waitpid -> %d %s", (int)w, w > 0 ? dp_status_str(st, b, sizeof b) : "timeout");
        ptrace(PT_DETACH, c.pid, 0, 0);
    }
    bury(&c);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "debuggee")) debuggee_main();
    if (argc > 1 && !strcmp(argv[1], "noop")) return 0;
    dp_watchdog(150);
    signal(SIGPIPE, SIG_IGN);
    t_attachexc();
    t_step();
    t_traceme(0);
    t_traceme(1);
    t_exec();
    t_deny();
    t_sibling();
    t_attach_old();
    return dp_summary("h2_ptrace");
}
