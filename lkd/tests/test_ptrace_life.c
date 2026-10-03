/* test_ptrace_life.c: tracer and tracee lifetimes, events and wait statuses.
 * Linux behaviour modelled (v6.12): kernel/ptrace.c exit_ptrace()/__ptrace_detach(),
 * ptrace_init_task() and kernel_clone() (auto-attach, initial SIGSTOP), ptrace_event(); kernel/exit.c
 * wait_task_stopped(ptrace), wait_task_zombie(), eligible_child(); kernel/signal.c do_jobctl_trap()
 * (group stop of a traced task); fs/exec.c exec_binprm() (SIGTRAP after exec without TRACEEXEC). */
#include "test_ptrace_util.h"
#include "../src/ptrace/pt_priv.h"

static struct tracee *attach_loop(void)
{
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    return e;
}

static void finish(struct tracee *e)
{
    (void)PT(LX_PTRACE_DETACH, e->tid, 0, 0);
    tracee_quit(e);
    get64(e->wr[0]);
    pthread_join(e->th, NULL);
    int st;
    (void)wait_pid(e->tid, &st, LX_WNOHANG);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

static int wait_byte(int fd, int ms)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    char c;
    return poll(&p, 1, ms) > 0 && read(fd, &c, 1) == 1;
}

/* ---- a tracer that is another guest process (a thread of this host process) -------------------------- */

struct tr_arg { struct tracee *c; uint32_t opts; };

static void body_tracer(struct tracee *e)
{
    struct tr_arg *a = e->arg;
    lkd_task *B = e->t;
    int st = 0;
    put64(e->wr[1], sct(B, LKD_NR_ptrace, LX_PTRACE_ATTACH, (uint64_t)a->c->tid, 0, 0, 0, 0));
    put64(e->wr[1], sct(B, LKD_NR_wait4, (uint64_t)a->c->tid, P(&st), 0, 0, 0, 0) == a->c->tid && st == 0x137f);
    if (a->opts) put64(e->wr[1], sct(B, LKD_NR_ptrace, LX_PTRACE_SETOPTIONS, (uint64_t)a->c->tid, 0, a->opts, 0, 0));
    wait_byte(e->rd[0], 20000);          /* told to die */
    tracee_exit(e, 0);
}

/* exit_ptrace(): when the tracer exits its tracees are detached and keep running; the signal they were
 * stopped with (here the SIGSTOP of ATTACH) is dropped, not delivered. */
LT_TEST(tracer_exit_detaches_and_tracee_continues)
{
    struct tracee *c = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && c->calls < 3; i++) usleep(500);
    struct tr_arg a = { c, 0 };
    struct tracee *b = tracee_start(body_tracer, &a);
    LT_EQ(get64(b->wr[0]), 0);
    LT_EQ(get64(b->wr[0]), 1);
    LT_EQ(lkd_ptrace_tracer_of(c->tid), b->tid);
    LT_CHECK(atomic_load(&c->t->ptrace.flags) & LKD_PT_STOPPED);
    uint64_t n0 = c->calls;
    usleep(20000);
    LT_EQ(c->calls, n0);                                          /* parked */
    fake_engine_reset();
    char go = 1;
    (void)!write(b->rd[1], &go, 1);
    pthread_join(b->th, NULL);
    for (int i = 0; i < 4000 && c->calls == n0; i++) usleep(500);
    LT_CHECK(c->calls > n0);                                      /* continued */
    LT_EQ(lkd_ptrace_tracer_of(c->tid), 0);
    LT_EQ(atomic_load(&c->t->ptrace.flags) & (LKD_PT_TRACED | LKD_PT_STOPPED), 0);
    LT_EQ(fake_frame_build_count, 0);                             /* nothing was delivered */
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(c->pid, &pi), 0);
    LT_CHECK(pi.state != 'Z' && pi.state != 'T' && pi.state != 't');
    tracee_quit(c);
    get64(c->wr[0]);
    pthread_join(c->th, NULL);
}

/* PTRACE_O_EXITKILL: the tracee is killed with SIGKILL when its tracer exits. */
LT_TEST(exitkill)
{
    struct tracee *c = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && c->calls < 3; i++) usleep(500);
    struct tr_arg a = { c, LX_PTRACE_O_EXITKILL };
    struct tracee *b = tracee_start(body_tracer, &a);
    LT_EQ(get64(b->wr[0]), 0);
    LT_EQ(get64(b->wr[0]), 1);
    LT_EQ(get64(b->wr[0]), 0);                                    /* SETOPTIONS */
    char go = 1;
    (void)!write(b->rd[1], &go, 1);
    pthread_join(b->th, NULL);
    pthread_join(c->th, NULL);                                    /* it died: its thread ended */
    LT_EQ(c->exit_status, 128 + LX_SIGKILL);                      /* killed, not exited */
    LT_EQ(lkd_ptrace_tracer_of(c->tid), 0);
}

/* wait_task_zombie(): a tracee that dies is reported to its tracer (who is not its parent) with the
 * exit status; reaped once. PTRACE_O_TRACEEXIT adds the exit event stop (status 0x6057f) whose message is
 * the exit status. */
LT_TEST(tracee_death_reaches_the_tracer)
{
    struct tracee *e = attach_loop();
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    tracee_quit(e);
    int st = 0;
    LT_EQ(wait_pid(e->tid, &st, 0), e->tid);
    LT_EQ(st, 3 << 8);                                            /* exit_group(3) */
    LT_ERRNO(wait_pid(e->tid, &st, 0), LX_ECHILD);
    LT_ERRNO(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&(struct lx_user_regs){0})), LX_ESRCH);
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);

    e = attach_loop();
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, LX_PTRACE_O_TRACEEXIT), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    tracee_quit(e);
    LT_CHECK(expect_stop(e->tid, 0x6057f));
    uint64_t msg = 0;
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 3 << 8);
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);            /* still inspectable */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    LT_EQ(wait_pid(e->tid, &st, 0), e->tid);
    LT_EQ(st, 3 << 8);
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

/* PTRACE_KILL sends SIGKILL; the tracee wakes from its stop and dies; the tracer sees WIFSIGNALED(9). */
LT_TEST(kill_request)
{
    struct tracee *e = attach_loop();
    LT_EQ(PT(LX_PTRACE_KILL, e->tid, 0, 0), 0);
    int st = 0;
    LT_EQ(wait_pid(e->tid, &st, 0), e->tid);
    LT_EQ(st, 9);
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

/* ptrace_detach(): the signal in data is delivered, further requests are ESRCH, wait4 has nothing to wait for. */
LT_TEST(detach_with_signal)
{
    struct tracee *e = attach_loop();
    fake_engine_reset();
    LT_ERRNO(PT(LX_PTRACE_DETACH, e->tid, 0, 65), LX_EIO);
    LT_EQ(PT(LX_PTRACE_DETACH, e->tid, 0, 10), 0);
    for (int i = 0; i < 4000 && !fake_frame_build_count; i++) usleep(500);
    LT_EQ(fake_frame_build_count, 1);
    LT_EQ(fake_last_delivery.signo, 10);
    LT_ERRNO(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&(struct lx_user_regs){0})), LX_ESRCH);
    LT_ERRNO(PT(LX_PTRACE_CONT, e->tid, 0, 0), LX_ESRCH);
    LT_EQ(lkd_ptrace_tracer_of(e->tid), 0);
    int st;
    LT_ERRNO(wait_pid(e->tid, &st, LX_WNOHANG), LX_ECHILD);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);                 /* it can be traced again */
    LT_CHECK(expect_stop(e->tid, 0x137f));
    finish(e);
}

/* wait4 statuses of ptrace stops: reported once, whatever WUNTRACED says; waitid with WNOWAIT keeps the
 * event; wait4(-1) and wait4(pid) select; WNOHANG returns 0 while tracees exist but nothing is pending. */
LT_TEST(wait_semantics)
{
    struct tracee *a = attach_loop();
    struct tracee *b = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && b->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, b->tid, 0, 0), 0);
    int st = 0;
    LT_EQ(wait_pid(-1, &st, 0), b->tid);                          /* a's stop was consumed by attach_loop */
    LT_EQ(st, 0x137f);
    LT_EQ(wait_pid(-1, &st, LX_WNOHANG), 0);
    LT_EQ(wait_pid(a->tid, &st, LX_WNOHANG), 0);
    /* waitid: CLD_TRAPPED, repeatable with WNOWAIT, then consumed */
    LT_EQ(PT(LX_PTRACE_CONT, a->tid, 0, 12), 0);                  /* SIGUSR2 delivered later */
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)a->pid, 14, 0, 0, 0, 0), 0);
    struct lx_siginfo wi;
    for (int k = 0; k < 2; k++) {
        memset(&wi, 0, sizeof wi);
        LT_EQ(sct(TR, LKD_NR_waitid, LX_P_PID, (uint64_t)a->tid, P(&wi), LX_WSTOPPED | LX_WNOWAIT, 0, 0), 0);
        LT_EQ(wi.si_code, LX_CLD_TRAPPED);
        LT_EQ(wi.u.chld.pid, a->tid);
    }
    LT_EQ(wait_pid(a->tid, &st, 0), a->tid);
    LT_EQ(st, 0xe7f);                                             /* SIGALRM stop */
    LT_EQ(wait_pid(a->tid, &st, LX_WNOHANG), 0);
    LT_ERRNO(sct(TR, LKD_NR_wait4, (uint64_t)b->tid, P(&st), 0x10, 0, 0, 0), LX_EINVAL);
    /* a thread of the tracer's process may wait for the tracee of another thread, unless __WNOTHREAD */
    lkd_task *th2 = fake_make_thread(TR);
    LT_EQ(PT(LX_PTRACE_CONT, a->tid, 0, 14), 0);
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)a->pid, 15, 0, 0, 0, 0), 0);
    usleep(30000);
    /* __WNOTHREAD: only the tracees of th2 itself, it has none: ECHILD (do_wait looks at current only) */
    LT_ERRNO(sct(th2, LKD_NR_wait4, (uint64_t)a->tid, P(&st), LX_WNOHANG | LX_WNOTHREAD, 0, 0, 0), LX_ECHILD);
    LT_EQ(sct(th2, LKD_NR_wait4, (uint64_t)a->tid, P(&st), LX_WNOHANG, 0, 0, 0), a->tid);
    LT_EQ(st, 0xf7f);
    struct lx_user_regs r;
    LT_ERRNO(sct(th2, LKD_NR_ptrace, LX_PTRACE_GETREGS, (uint64_t)a->tid, 0, P(&r), 0, 0), LX_ESRCH);   /* not the tracer */
    LT_EQ(PT(LX_PTRACE_GETREGS, a->tid, 0, P(&r)), 0);
    finish(a);
    finish(b);
}

/* do_jobctl_trap(): a traced task that takes a stopping signal stops twice, first the signal-delivery-stop,
 * then (when the tracer passes the signal on) the group stop, reported as CLD_STOPPED with no siginfo
 * (non-seized: GETSIGINFO is EINVAL); a seized tracee gets a PTRACE_EVENT_STOP, (128 << 16) | (19 << 8) | 0x7f. */
LT_TEST(group_stop_of_a_traced_task)
{
    struct tracee *e = attach_loop();
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    usleep(10000);
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)e->pid, 19, 0, 0, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));                        /* signal-delivery-stop */
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 19), 0);                  /* pass the stop on */
    struct lx_siginfo wi;
    memset(&wi, 0, sizeof wi);
    LT_EQ(sct(TR, LKD_NR_waitid, LX_P_PID, (uint64_t)e->tid, P(&wi), LX_WSTOPPED | LX_WNOWAIT, 0, 0), 0);
    LT_EQ(wi.si_code, LX_CLD_TRAPPED);                            /* wait_task_stopped(): ptrace wait => CLD_TRAPPED */
    LT_CHECK(expect_stop(e->tid, 0x137f));
    struct lx_siginfo si;
    LT_ERRNO(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), LX_EINVAL);
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);                   /* the tracee runs again */
    uint64_t n0 = e->calls;
    usleep(20000);
    LT_CHECK(e->calls > n0);
    finish(e);

    e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, 0), 0);
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)e->pid, 19, 0, 0, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 19), 0);
    /* waitid: si_status is the whole stop code, SIGSTOP | PTRACE_EVENT_STOP << 8 (wait_task_stopped,
     * kernel/exit.c v6.12 l.1358: infop->status = exit_code), not its low byte */
    memset(&wi, 0, sizeof wi);
    LT_EQ(sct(TR, LKD_NR_waitid, LX_P_PID, (uint64_t)e->tid, P(&wi), LX_WSTOPPED | LX_WNOWAIT, 0, 0), 0);
    LT_EQ(wi.si_code, LX_CLD_TRAPPED);
    LT_EQ(wi.u.chld.status, 0x8013);
    LT_CHECK(expect_stop(e->tid, 0x80137f));
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 19);
    LT_EQ(si.si_code, (128 << 8) | 19);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)e->pid, 18, 0, 0, 0, 0), 0);   // SIGCONT is a traced signal too
    LT_CHECK(expect_stop(e->tid, 0x127f));
    finish(e);
}

/* ---- exec ------------------------------------------------------------------------------------------- */

static void body_exec(struct tracee *e)
{
    char c;
    int died = 0;
    for (int i = 0; i < 40000 && !died; i++) {
        (void)gsys(e->t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died);
        if (read(e->rd[0], &c, 1) == 1) {
            if (c != 'E') break;
            /* the tail of execve: the image is in place; lkd_exec_finish() reports the exec to the tracer */
            atomic_store(&e->t->cur_nr, 59);
            lkd_exec_finish(e->t);
            /* then the tail of lkd_syscall for LKD_RET_CONTEXT_REPLACED: the exit stop (dispatch.c) */
            if (atomic_load(&e->t->ptrace.flags) & LKD_PT_SYSCALL_TRACE) lkd_ptrace_syscall_exit_replaced(e->t);
            atomic_store(&e->t->cur_nr, -1);
            if (safe_point(e->t) < 0) died = 1;
            put64(e->wr[1], 2);
        }
        usleep(300);
    }
    tracee_exit(e, 0);
}

/* exec_binprm(): with PTRACE_O_TRACEEXEC the exec is a PTRACE_EVENT_EXEC stop (0x4057f, message = former
 * tid); without it a plain traced task receives SIGTRAP (SI_USER) instead and stops in signal-delivery-stop
 * (0x57f). A seized task without the option gets nothing. */
LT_TEST(exec_reports)
{
    struct tracee *e = tracee_start(body_exec, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    char x = 'E';
    (void)!write(e->rd[1], &x, 1);
    LT_CHECK(expect_stop(e->tid, 0x57f));                         /* SIGTRAP after exec */
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 5);
    LT_EQ(si.si_code, LX_SI_USER);
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, LX_PTRACE_O_TRACEEXEC), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    LT_EQ(get64(e->wr[0]), 2);
    (void)!write(e->rd[1], &x, 1);
    LT_CHECK(expect_stop(e->tid, 0x4057f));                       /* PTRACE_EVENT_EXEC */
    uint64_t msg = 0;
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, e->tid);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 12), 0);                  /* a signal given at an event stop is ignored */
    LT_EQ(get64(e->wr[0]), 2);
    fake_engine_reset();
    usleep(10000);
    LT_EQ(fake_frame_build_count, 0);
    /* PTRACE_SYSCALL at the event stop: the next stop is the exit of execve (0x857f here with SYSGOOD) */
    LT_ERRNO(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, 0), LX_ESRCH);   /* running again */
    char q = 'Q';
    (void)!write(e->rd[1], &q, 1);
    int st;
    LT_EQ(wait_pid(e->tid, &st, 0), e->tid);
    LT_EQ(st, 0);
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);

    e = tracee_start(body_exec, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, LX_PTRACE_O_TRACEEXEC | LX_PTRACE_O_TRACESYSGOOD), 0);
    (void)!write(e->rd[1], &x, 1);
    LT_CHECK(expect_stop(e->tid, 0x4057f));
    LT_EQ(PT(LX_PTRACE_SYSCALL, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x857f));                        /* exit of execve */
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
    LT_EQ(msg, 2);
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_GETREGS, e->tid, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 59);
    LT_EQ(r.rax, 0);
    finish(e);
}

/* ---- TRACEME, creator thread --------------------------------------------------------------------------- */

/* ptrace_traceme(): the tracer is the real parent's thread that created the process; EPERM if it is
 * already traced or if that thread is gone (PF_EXITING). */
LT_TEST(traceme_rules)
{
    lkd_task *x = lkd_task_alloc(NULL, 0);
    lkd_task_publish(x);
    cpu_init(x);
    struct pt_local *L = pt_local_of(x, 1);
    L->creator_tid = 0x3ffffff0; L->creator_pid = 0x3ffffff0;      /* a thread that does not exist */
    LT_ERRNO(sct(x, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0), LX_EPERM);
    L->creator_tid = TR->tid; L->creator_pid = TR->proc->pid;
    LT_EQ(sct(x, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0), 0);
    LT_EQ(lkd_ptrace_tracer_of(x->tid), TR->tid);
    LT_ERRNO(sct(x, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0), LX_EPERM);
    LT_CHECK(atomic_load(&x->ptrace.flags) & LKD_PT_TRACED);
    LT_EQ(x->ptrace.tracer_tid, TR->tid);
    /* the registry shows TracerPid */
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup(x->tid, &ti), 0);
    LT_EQ(ti.tracer_tid, TR->tid);
    LT_ERRNO(PT(LX_PTRACE_ATTACH, x->tid, 0, 0), LX_EPERM);       /* already traced */
    /* its exit while nobody is stopped: reported to the tracer, who is not its parent */
    int32_t xt = x->tid;
    int died;
    (void)gsys(x, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0, &died);
    int st;
    (void)lkd_task_release(x, &st);
    LT_EQ(lkd_ptrace_tracer_of(xt), 0);
    if (getenv("PTDBG")) {
        for (int i = 0; i < PT_SLOTS; i++) { struct pt_slot *s = pt_slot_at(i); if (atomic_load(&s->link)) fprintf(stderr, "slot %d link %u tid %d tracer %d/%d TR %d\n", i, atomic_load(&s->link), atomic_load(&s->tid), atomic_load(&s->tracer_pid), atomic_load(&s->tracer_tid), TR->tid); }
    }
    LT_EQ(wait_pid(xt, &st, 0), xt);
    LT_EQ(st, 0);
}

/* ---- tracee that forks ------------------------------------------------------------------------------- */

static int g_wfd2;

static void grandchild_body(lkd_task *t)
{
    int died;
    put64(g_wfd2, gsys(t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died));
    (void)gsys(t, LKD_NR_exit_group, 7, 0, 0, 0, 0, 0, &died);
    int st;
    if (lkd_task_release(t, &st)) _exit(st);
    _exit(96);
}

static void forker_body(lkd_task *t)
{
    int died = 0;
    cpu_init(t);
    (void)gsys(t, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0, &died);
    died = self_signal(t, LX_SIGSTOP);
    int64_t r = gsys(t, LKD_NR_fork, 0, 0, 0, 0, 0, 0, &died);
    if (r == 0) { alarm(30); grandchild_body(t); }
    put64(g_wfd2, r);
    int st;
    (void)gsys(t, LKD_NR_exit_group, 5, 0, 0, 0, 0, 0, &died);
    if (lkd_task_release(t, &st)) _exit(st);
    _exit(95);
}

/* kernel_clone() / ptrace_init_task(): with PTRACE_O_TRACEFORK a fork is a PTRACE_EVENT_FORK stop
 * (0x1057f, message = the new pid) and the new process is traced by the same tracer, starting with a
 * pending SIGSTOP (status 0x137f); its exit is reported to the tracer (it is not the tracer's child). */
LT_TEST(fork_event_and_auto_attach)
{
    int rp[2];
    LT_EQ(pipe(rp), 0);
    g_wfd2 = rp[1];
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) { alarm(40); forker_body(lkd_current()); _exit(94); }
    LT_CHECK(pid > 0);
    int32_t p = (int32_t)pid;
    LT_CHECK(expect_stop(p, 0x137f));
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, p, 0, LX_PTRACE_O_TRACEFORK), 0);
    LT_EQ(PT(LX_PTRACE_CONT, p, 0, 0), 0);
    LT_CHECK(expect_stop(p, 0x1057f));
    uint64_t gpid = 0;
    LT_EQ(PT(LX_PTRACE_GETEVENTMSG, p, 0, P(&gpid)), 0);
    LT_CHECK(gpid > (uint64_t)p);
    struct lx_user_regs r;
    LT_EQ(PT(LX_PTRACE_GETREGS, p, 0, P(&r)), 0);
    LT_EQ(r.orig_rax, 57);                                       /* inside fork(): -ENOSYS in rax */
    LT_EQ(r.rax, (uint64_t)-38);
    int32_t g = (int32_t)gpid;
    LT_CHECK(expect_stop(g, 0x137f));                            /* the child starts with SIGSTOP pending */
    LT_EQ(lkd_ptrace_tracer_of(g), TR->tid);
    struct lx_siginfo si;
    LT_EQ(PT(LX_PTRACE_GETSIGINFO, g, 0, P(&si)), 0);
    LT_EQ(si.si_signo, 19);
    LT_EQ(si.si_code, LX_SI_USER);
    LT_EQ(PT(LX_PTRACE_CONT, g, 0, 0), 0);                       /* SIGSTOP dropped: it runs and exits with 7 */
    LT_EQ(PT(LX_PTRACE_CONT, p, 0, 0), 0);
    int st = 0;
    LT_EQ(wait_pid(g, &st, 0), g);
    LT_EQ(st, 7 << 8);                                           /* reported to the tracer */
    LT_EQ(wait_pid(p, &st, 0), p);
    LT_EQ(st, 5 << 8);
    LT_EQ(get64(rp[0]), g);                                       /* the grandchild's getpid, and the pid that */
    LT_EQ(get64(rp[0]), g);                                       /* fork() returned to the parent (either order) */
    close(rp[0]); close(rp[1]);
    /* the tracee's process is the grandchild's real parent: it is reaped by its wait4, not ours; we may
     * still see it as a child of nobody */
}


/* ---- events and the options that enable them ------------------------------------------------------- */

static void body_event(struct tracee *e)
{
    char c;
    int died = 0;
    for (int i = 0; i < 40000 && !died; i++) {
        (void)gsys(e->t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died);
        if (read(e->rd[0], &c, 1) == 1) {
            if (c > 7) break;
            put64(e->wr[1], lkd_ptrace_event(e->t, c, 0x1000 + (uint64_t)c));
        }
        usleep(300);
    }
    tracee_exit(e, 0);
}

/* ptrace_event(): a PTRACE_EVENT_x stop happens only when its PTRACE_O_TRACEx option is on; the status is
 * (event << 16) | (SIGTRAP << 8) | 0x7f, GETEVENTMSG the message, and a signal given at the stop is ignored. */
LT_TEST(events_follow_their_options)
{
    struct tracee *e = tracee_start(body_event, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    const struct { int ev; uint32_t opt; uint32_t status; } t[] = {
        { LX_PTRACE_EVENT_FORK, LX_PTRACE_O_TRACEFORK, 0x1057f },
        { LX_PTRACE_EVENT_VFORK, LX_PTRACE_O_TRACEVFORK, 0x2057f },
        { LX_PTRACE_EVENT_CLONE, LX_PTRACE_O_TRACECLONE, 0x3057f },
        { LX_PTRACE_EVENT_VFORK_DONE, LX_PTRACE_O_TRACEVFORKDONE, 0x5057f },
    };
    for (int k = 0; k < 4; k++) {
        char c = (char)t[k].ev;
        (void)!write(e->rd[1], &c, 1);                              /* option off: no stop */
        LT_EQ(get64(e->wr[0]), 0);
    }
    /* stop the tracee to set the options (a SIGSTOP via kill is a signal-delivery-stop) */
    LT_EQ(sct(TR, LKD_NR_kill, (uint64_t)e->pid, 10, 0, 0, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0xa7f));
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0,
             LX_PTRACE_O_TRACEFORK | LX_PTRACE_O_TRACEVFORK | LX_PTRACE_O_TRACECLONE | LX_PTRACE_O_TRACEVFORKDONE), 0);
    LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 0), 0);
    for (int k = 0; k < 4; k++) {
        char c = (char)t[k].ev;
        (void)!write(e->rd[1], &c, 1);
        LT_CHECK(expect_stop(e->tid, (int)t[k].status));
        uint64_t msg = 0;
        LT_EQ(PT(LX_PTRACE_GETEVENTMSG, e->tid, 0, P(&msg)), 0);
        LT_EQ(msg, 0x1000 + (uint64_t)t[k].ev);
        struct lx_siginfo si;
        LT_EQ(PT(LX_PTRACE_GETSIGINFO, e->tid, 0, P(&si)), 0);
        LT_EQ(si.si_code, (t[k].ev << 8) | 5);
        LT_EQ(PT(LX_PTRACE_CONT, e->tid, 0, 12), 0);                /* the signal is ignored */
        LT_EQ(get64(e->wr[0]), 1);
    }
    LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, 0), -LX_ESRCH);
    char q = 9;
    (void)!write(e->rd[1], &q, 1);
    int st;
    LT_EQ(wait_pid(e->tid, &st, 0), e->tid);
    LT_EQ(st, 0);
    pthread_join(e->th, NULL);
    close(e->rd[0]); close(e->rd[1]); close(e->wr[0]); close(e->wr[1]);
    free(e);
}

/* ptrace_init_task() / kernel_clone(): the child of a traced task is traced by the same tracer when
 * CLONE_PTRACE is given, or when the matching PTRACE_O_TRACE* option is on and CLONE_UNTRACED is not; it
 * inherits the options and starts with SIGSTOP pending. An untraced parent never attaches its child. */
LT_TEST(auto_attach_rules)
{
    struct tracee *e = attach_loop();
    const uint64_t thr = LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD;
    const struct { uint64_t flags; uint32_t opts; int attached; } cases[] = {
        { thr, 0, 0 },                                                       /* no option, no CLONE_PTRACE */
        { thr, LX_PTRACE_O_TRACEFORK, 0 },                                   /* wrong option: this is a clone */
        { thr, LX_PTRACE_O_TRACECLONE, 1 },
        { thr | LX_CLONE_UNTRACED, LX_PTRACE_O_TRACECLONE, 0 },
        { thr | LX_CLONE_PTRACE, 0, 1 },
        { thr | LX_CLONE_PTRACE | LX_CLONE_UNTRACED, 0, 1 },
        { LX_SIGCHLD, LX_PTRACE_O_TRACEFORK, 1 },                            /* fork() */
        { LX_SIGCHLD | LX_CLONE_VFORK | LX_CLONE_VM, LX_PTRACE_O_TRACEVFORK, 1 },
        { LX_SIGCHLD | LX_CLONE_VFORK | LX_CLONE_VM, LX_PTRACE_O_TRACEFORK, 0 },
        { 17 | LX_CLONE_VM, LX_PTRACE_O_TRACECLONE, 0 },                      /* SIGCHLD exit signal: a fork */
        { 12 | LX_CLONE_VM, LX_PTRACE_O_TRACECLONE, 1 },                      /* other exit signal: a clone */
    };
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        LT_EQ(PT(LX_PTRACE_SETOPTIONS, e->tid, 0, cases[k].opts | LX_PTRACE_O_TRACESYSGOOD), 0);
        lkd_task *ch = lkd_task_alloc(e->t, cases[k].flags);
        lkd_ptrace_task_init(ch, e->t, cases[k].flags);
        LT_EQ(lkd_ptrace_tracer_of(ch->tid), cases[k].attached ? TR->tid : 0);
        struct lkd_sig_status ss;
        lkd_sig_status(ch, &ss);
        LT_EQ((ss.pnd >> 18) & 1, cases[k].attached);                         /* SIGSTOP pending */
        if (cases[k].attached) {
            LT_CHECK(atomic_load(&ch->ptrace.flags) & LKD_PT_TRACED);
            int i = pt_find_tracee(ch->tid);
            LT_CHECK(i >= 0);
            LT_EQ(pt_slot_at(i)->options, cases[k].opts | LX_PTRACE_O_TRACESYSGOOD);    /* inherited */
            pt_lock();
            pt_free_locked(i);
            pt_unlock();
        }
        lkd_task_put(ch);
    }
    /* a seized parent seizes its children: they start with a trap, not a SIGSTOP */
    finish(e);
    e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_SEIZE, e->tid, 0, LX_PTRACE_O_TRACECLONE), 0);
    lkd_task *ch = lkd_task_alloc(e->t, thr);
    lkd_ptrace_task_init(ch, e->t, thr);
    struct lkd_sig_status ss;
    lkd_sig_status(ch, &ss);
    LT_EQ(lkd_ptrace_tracer_of(ch->tid), TR->tid);
    LT_EQ((ss.pnd >> 18) & 1, 0);
    LT_CHECK(pt_slot_at(pt_find_tracee(ch->tid))->seized);
    pt_lock();
    pt_free_locked(pt_find_tracee(ch->tid));
    pt_unlock();
    lkd_task_put(ch);
    /* a parent that is not traced never attaches */
    finish(e);
    e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    ch = lkd_task_alloc(e->t, thr | LX_CLONE_PTRACE);
    lkd_ptrace_task_init(ch, e->t, thr | LX_CLONE_PTRACE);
    LT_EQ(lkd_ptrace_tracer_of(ch->tid), 0);
    lkd_task_put(ch);
    finish(e);
}

/* do_wait() says ECHILD unless some child (real or traced) satisfies the selector: pid, any, process group,
 * __WCLONE for a thread-group leader. */
LT_TEST(wait_selectors_see_tracees)
{
    struct tracee *e = attach_loop();
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(e->pid, &pi), 0);
    LT_EQ(lkd_ptrace_wait_has(TR, -1, LX_WEXITED), 1);
    LT_EQ(lkd_ptrace_wait_has(TR, e->tid, LX_WEXITED), 1);
    LT_EQ(lkd_ptrace_wait_has(TR, e->tid + 1000, LX_WEXITED), 0);
    LT_EQ(lkd_ptrace_wait_has(TR, -pi.pgid, LX_WEXITED), 1);
    LT_EQ(lkd_ptrace_wait_has(TR, -pi.pgid - 1000, LX_WEXITED), 0);
    LT_EQ(lkd_ptrace_wait_has(TR, -1, LX_WEXITED | LX_WCLONE), 0);          /* a leader is not a "clone" child */
    LT_EQ(lkd_ptrace_wait_has(TR, -1, LX_WEXITED | LX_WALL), 1);
    LT_EQ(lkd_ptrace_has_tracees(TR), 1);
    /* wait4 itself: a pid that is neither a child nor a tracee is ECHILD even while tracees exist
     * (do_wait: no child satisfies the selector), a tracee is waited for (0 with WNOHANG) */
    int st = 0;
    LT_ERRNO(wait_pid(e->tid + 1000, &st, LX_WNOHANG), LX_ECHILD);
    LT_ERRNO(wait_pid(e->tid + 1000, &st, 0), LX_ECHILD);
    LT_ERRNO(wait_pid(-pi.pgid - 1000, &st, LX_WNOHANG), LX_ECHILD);
    LT_EQ(wait_pid(e->tid, &st, LX_WNOHANG), 0);
    finish(e);
    LT_EQ(lkd_ptrace_has_tracees(TR), 0);
}

/* A blocking wait4 returns as soon as the tracee stops (the tracee wakes the waiter's wake pipe). Before
 * request ptrace-2 the waiter slept the rest of a 10 ms slice after the wake: about 11 ms per stop
 * (MESURE in test_ptrace_transport). 20 syscall-entry stops of a tracee looping every 0.3 ms must take
 * well under 20 x 10 ms. */
LT_TEST(blocking_wait4_wakes_at_once)
{
    struct tracee *e = attach_loop();
    int ok = 1;
    int64_t t0 = lkd_host_mono_ns();
    for (int i = 0; i < 20 && ok; i++) {
        ok = PT(LX_PTRACE_SYSCALL, e->tid, 0, 0) == 0;
        if (ok) ok = expect_stop(e->tid, 0x57f);
    }
    int64_t ms = (lkd_host_mono_ns() - t0) / 1000000;
    LT_CHECK(ok);
    LT_CHECK(ms < 100);
    if (ms >= 100) fprintf(stderr, "20 stops took %lld ms\n", (long long)ms);
    finish(e);
}

/* A tracee whose host process disappears without running its exit path (killed from outside) is reported
 * to its tracer as killed by SIGKILL, not waited for ever. The tracer is not its parent here: the tracee is
 * the grandchild of the tracer's process. */
static void host_victim_body(lkd_task *t)
{
    int died = 0;
    cpu_init(t);
    int64_t r = gsys(t, LKD_NR_fork, 0, 0, 0, 0, 0, 0, &died);
    if (r == 0) {
        alarm(30);
        put64(g_wfd2, (int64_t)getpid());                              /* host pid of the grandchild */
        put64(g_wfd2, t->tid);
        for (int i = 0; i < 20000 && !died; i++) { (void)gsys(t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died); usleep(1000); }
        _exit(0);
    }
    usleep(2000000);
    _exit(0);
}

LT_TEST(host_death_of_a_tracee)
{
    int rp[2];
    LT_EQ(pipe(rp), 0);
    g_wfd2 = rp[1];
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) { alarm(40); host_victim_body(lkd_current()); _exit(93); }
    LT_CHECK(pid > 0);
    int64_t host = get64(rp[0]);
    int64_t gtid = get64(rp[0]);
    LT_CHECK(host > 0 && gtid > 0);
    LT_EQ(PT(LX_PTRACE_ATTACH, (int32_t)gtid, 0, 0), 0);
    LT_CHECK(expect_stop((int32_t)gtid, 0x137f));
    LT_EQ(kill((pid_t)host, SIGKILL), 0);                              /* no exit path runs in the victim */
    int st = 0;
    LT_EQ(wait_pid((int32_t)gtid, &st, 0), gtid);
    LT_EQ(st, 9);
    LT_ERRNO(wait_pid((int32_t)gtid, &st, LX_WNOHANG), LX_ECHILD);
    int st2;
    wait_pid((int32_t)pid, &st2, 0);
    close(rp[0]); close(rp[1]);
}

/* de_thread(): a non-leader thread that execs takes the thread-group leader's tid. The links follow: the
 * tid of a traced task and the tracer tid of its tracees are rewritten (exit_ptrace/ptrace_link keep
 * pointers to the task, not ids, in Linux). */
LT_TEST(exec_by_a_non_leader_keeps_the_links)
{
    lkd_task *x = lkd_task_alloc(NULL, 0);
    lkd_task_publish(x);
    cpu_init(x);
    struct pt_local *L = pt_local_of(x, 1);
    L->creator_tid = TR->tid; L->creator_pid = TR->proc->pid;
    int32_t old = x->tid;
    LT_EQ(sct(x, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0), 0);
    int i = pt_find_tracee(old);
    LT_CHECK(i >= 0);
    /* x is also the tracer of a second task */
    lkd_task *y = lkd_task_alloc(NULL, 0);
    lkd_task_publish(y);
    LT_CHECK(pt_link(y->tid, y->proc->pid, getpid(), 1000, x->proc->pid, old, getpid(), 0, 0) >= 0);
    x->tid = 0x55555;                                              /* the tid after de_thread() */
    lkd_ptrace_exec_commit(x);
    LT_EQ(atomic_load(&pt_slot_at(i)->tid), 0x55555);
    LT_EQ(lkd_ptrace_tracer_of(0x55555), TR->tid);
    LT_EQ(lkd_ptrace_tracer_of(y->tid), 0x55555);
    LT_EQ(pt_find_tracee(old), -1);
    pt_lock();
    pt_free_locked(i);
    pt_free_locked(pt_find_tracee(y->tid));
    pt_unlock();
    x->tid = old;
}

/* A forked tracee execs (EAC sequence: fork, TRACEME, SIGSTOP, execve). Its ptrace state was copied from
 * the parent, whose tid is still in tid_seen when the ptrace fork hook runs; exec must not take the
 * tracer's tid for an old tid of the tracee's process. Before the fix the link's tracer_tid became the
 * tracee's own tid and every request after the exec stop answered ESRCH. */
LT_TEST(exec_of_a_forked_tracee_keeps_its_tracer)
{
    lkd_task *x = lkd_task_alloc(NULL, 0);
    lkd_task_publish(x);
    cpu_init(x);
    LT_CHECK(x->proc->pid != TR->proc->pid);
    struct pt_local *L = pt_local_of(x, 1);
    L->tid_seen = TR->tid;                                         /* the parent's tid, copied by fork */
    int i = pt_link(x->tid, x->proc->pid, getpid(), 1000, TR->proc->pid, TR->tid, getpid(), 0, 0);
    LT_CHECK(i >= 0);
    lkd_ptrace_exec_commit(x);
    LT_EQ(atomic_load(&pt_slot_at(i)->tracer_tid), TR->tid);
    LT_EQ(atomic_load(&pt_slot_at(i)->tid), x->tid);
    LT_EQ(lkd_ptrace_tracer_of(x->tid), TR->tid);
    pt_lock();
    pt_free_locked(i);
    pt_unlock();
}

/* The tracer's host process is killed from outside, so no exit hook runs and the tracee is parked with
 * nobody to resume it: it notices that the tracer's process is gone, detaches itself (the signal of the
 * stop is dropped) and goes on. */
static void crash_tracer_body(lkd_task *t)
{
    int died = 0;
    cpu_init(t);
    int64_t r = gsys(t, LKD_NR_fork, 0, 0, 0, 0, 0, 0, &died);
    if (r == 0) {                                           /* the tracee */
        alarm(30);
        (void)gsys(t, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0, &died);
        died = self_signal(t, LX_SIGSTOP);                  /* parked: its tracer is the process that forked it */
        put64(g_wfd2, 42);
        (void)gsys(t, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0, &died);
        int st;
        if (lkd_task_release(t, &st)) _exit(st);
        _exit(89);
    }
    int st = 0;
    (void)sct(t, LKD_NR_wait4, (uint64_t)r, P(&st), 0, 0, 0, 0);       /* its first stop */
    put64(g_wfd2, st == 0x137f ? (int64_t)getpid() : -1);
    sleep(25);
    _exit(0);
}

LT_TEST(tracer_host_process_killed)
{
    int rp[2];
    LT_EQ(pipe(rp), 0);
    g_wfd2 = rp[1];
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) { alarm(40); crash_tracer_body(lkd_current()); _exit(88); }
    LT_CHECK(pid > 0);
    int64_t host = get64(rp[0]);
    LT_CHECK(host > 0);
    LT_EQ(kill((pid_t)host, SIGKILL), 0);
    int64_t t0 = mono_ns();
    LT_EQ(get64(rp[0]), 42);                                 /* the tracee ran again */
    int64_t ms = (mono_ns() - t0) / 1000000;
    printf("MESURE tracee resumed %lld ms after its tracer's host process was killed\n", (long long)ms);
    LT_CHECK(ms < 5000);
    close(rp[0]); close(rp[1]);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_filter = argv[1];
    test_setup(1);
    RUN(tracer_exit_detaches_and_tracee_continues);
    RUN(exitkill);
    RUN(tracee_death_reaches_the_tracer);
    RUN(kill_request);
    RUN(detach_with_signal);
    RUN(wait_semantics);
    RUN(group_stop_of_a_traced_task);
    RUN(exec_reports);
    RUN(traceme_rules);
    RUN(fork_event_and_auto_attach);
    RUN(events_follow_their_options);
    RUN(auto_attach_rules);
    RUN(wait_selectors_see_tracees);
    RUN(blocking_wait4_wakes_at_once);
    RUN(host_death_of_a_tracee);
    RUN(exec_by_a_non_leader_keeps_the_links);
    RUN(exec_of_a_forked_tracee_keeps_its_tracer);
    RUN(tracer_host_process_killed);
    return lt_finish();
}
