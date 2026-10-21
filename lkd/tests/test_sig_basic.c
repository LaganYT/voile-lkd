/* test_sig_basic.c: signal actions, masks, queues, delivery protocol, restart rules, faults, altstack,
 * timed waits, suspend, job control, permissions, signalfd. Each case cites the Linux behaviour it
 * models (kernel/signal.c, arch/x86/kernel/signal.c, fs/signalfd.c at v6.12, and the man pages). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include "sig.h"


static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq r = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &r);
}
#define P(x) ((uint64_t)(uintptr_t)(x))
#define BIT(s) LX_SIGBIT(s)

static lkd_task *mk(void)
{
    lkd_task *t = fake_make_task();
    fake_cpu_of(t)->regs.rsp = 0x7000000;
    return t;
}

static int set_handler(lkd_task *t, int sig, uint64_t h, uint64_t flags, uint64_t mask)
{
    struct lx_sigaction a = { h, flags | LX_SA_RESTORER, 0x2000, mask };
    return (int)sc(t, LKD_NR_rt_sigaction, (uint64_t)sig, P(&a), 0, 8, 0, 0);
}

/* rt_sigaction(2): KILL/STOP cannot be changed (EINVAL), sigsetsize must be 8, unknown flag bits are
 * cleared (kernel/signal.c do_sigaction UAPI_SA_FLAGS: the SA_UNSUPPORTED probe), KILL/STOP leave sa_mask. */
LT_TEST(sigaction_rules)
{
    lkd_task *t = mk();
    struct lx_sigaction a = { 0x1234, LX_SA_SIGINFO | LX_SA_UNSUPPORTED | LX_SA_RESTORER, 0x5678,
                              BIT(LX_SIGKILL) | BIT(LX_SIGUSR2) | BIT(LX_SIGSTOP) }, o;
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&a), 0, 8, 0, 0), 0);
    memset(&o, 0xff, sizeof o);
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, 0x1234);
    LT_EQ(o.flags, LX_SA_SIGINFO | LX_SA_RESTORER);
    LT_EQ(o.restorer, 0x5678);
    LT_EQ(o.mask, BIT(LX_SIGUSR2));
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, LX_SIGKILL, P(&a), 0, 8, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, LX_SIGSTOP, P(&a), 0, 8, 0, 0), LX_EINVAL);
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGKILL, 0, P(&o), 8, 0, 0), 0); /* reading KILL is allowed */
    LT_EQ(o.handler, LX_SIG_DFL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, 0, P(&a), 0, 8, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, 65, 0, P(&o), 8, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&a), 0, 4, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, 8, 0, 8, 0, 0), LX_EFAULT);
}

/* rt_sigprocmask(2): BLOCK/UNBLOCK/SETMASK, SIGKILL and SIGSTOP silently not blockable, old set returned
 * before the change, bad how EINVAL, sigsetsize EINVAL. */
LT_TEST(sigprocmask_rules)
{
    lkd_task *t = mk();
    uint64_t set = BIT(LX_SIGUSR1) | BIT(LX_SIGKILL) | BIT(LX_SIGSTOP), old = 0;
    LT_EQ(sc(t, LKD_NR_rt_sigprocmask, LX_SIG_SETMASK, P(&set), P(&old), 8, 0, 0), 0);
    LT_EQ(old, 0);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGUSR1));
    set = BIT(LX_SIGUSR2);
    LT_EQ(sc(t, LKD_NR_rt_sigprocmask, LX_SIG_BLOCK, P(&set), P(&old), 8, 0, 0), 0);
    LT_EQ(old, BIT(LX_SIGUSR1));
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGUSR1) | BIT(LX_SIGUSR2));
    set = BIT(LX_SIGUSR1);
    LT_EQ(sc(t, LKD_NR_rt_sigprocmask, LX_SIG_UNBLOCK, P(&set), 0, 8, 0, 0), 0);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGUSR2));
    LT_ERRNO(sc(t, LKD_NR_rt_sigprocmask, 7, P(&set), 0, 8, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigprocmask, LX_SIG_BLOCK, P(&set), 0, 16, 0, 0), LX_EINVAL);
    LT_EQ(sc(t, LKD_NR_rt_sigprocmask, LX_SIG_BLOCK, 0, P(&old), 8, 0, 0), 0);
    LT_EQ(old, BIT(LX_SIGUSR2));
}

/* kill(2) to self with a handler: the engine sees a pending signal, lkd_signal_next builds the frame with
 * SI_USER pid/uid of the sender, the handler mask is blocked | sa_mask | the signal (signal_delivered),
 * old_mask is the mask before delivery; then nothing is left. */
LT_TEST(deliver_handler_frame)
{
    lkd_task *t = mk();
    fake_engine_reset();
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x401000, LX_SA_SIGINFO, BIT(LX_SIGUSR2)), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGHUP));
    LT_EQ(lkd_signal_pending(t), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_CHECK(lkd_signal_pending(t));
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_frame_build_count, 1);
    lkd_delivery *d = &fake_last_delivery;
    LT_EQ(d->signo, LX_SIGUSR1);
    LT_EQ(d->info.si_signo, LX_SIGUSR1);
    LT_EQ(d->info.si_code, LX_SI_USER);
    LT_EQ(d->info.u.kill.pid, t->proc->pid);
    LT_EQ(d->info.u.kill.uid, t->proc->cred.uid);
    LT_EQ(d->handler, 0x401000);
    LT_EQ(d->restorer, 0x2000);
    LT_CHECK(d->flags & LX_SA_SIGINFO);
    LT_EQ(d->old_mask, BIT(LX_SIGHUP));
    LT_EQ(d->new_mask, BIT(LX_SIGHUP) | BIT(LX_SIGUSR2) | BIT(LX_SIGUSR1));
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGHUP) | BIT(LX_SIGUSR2) | BIT(LX_SIGUSR1));
    LT_EQ(d->use_altstack, 0);
    LT_EQ(d->altstack.ss_flags, LX_SS_DISABLE);
    LT_EQ(lkd_signal_next(t), 0);
    LT_EQ(fake_frame_build_count, 1);
    /* rt_sigreturn: the engine pops the frame and LKD reinstalls the saved mask */
    fake_cpu_of(t)->saved_mask = BIT(LX_SIGHUP);
    lkd_sysreq r = { LKD_NR_rt_sigreturn, { 0 } };
    int64_t rax;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_CONTEXT_REPLACED);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGHUP));
    LT_EQ(fake_frame_restore_count, 1);
}

/* SA_NODEFER keeps the signal unblocked in the handler; SA_RESETHAND resets the action to SIG_DFL
 * when the handler is entered (get_signal SA_ONESHOT). */
LT_TEST(nodefer_and_resethand)
{
    lkd_task *t = mk();
    LT_EQ(set_handler(t, LX_SIGUSR2, 0x5000, LX_SA_NODEFER | LX_SA_RESETHAND, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR2, 0, 0, 0, 0), 0);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.new_mask & BIT(LX_SIGUSR2), 0);
    LT_EQ(fake_last_delivery.info.si_code, LX_SI_TKILL);
    struct lx_sigaction o;
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR2, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_DFL);
}

/* An ignored signal is discarded at generation (sig_ignored), SIGCHLD with SIG_DFL too; a blocked
 * ignored signal stays pending (the handler may change before it is unblocked); setting SIG_IGN
 * flushes pending instances (POSIX 3.3.1.3). */
LT_TEST(ignored_signals)
{
    lkd_task *t = mk();
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&ign), 0, 8, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGCHLD, 0, 0, 0, 0), 0);
    uint64_t pend = 99;
    LT_EQ(sc(t, LKD_NR_rt_sigpending, P(&pend), 8, 0, 0, 0, 0), 0);
    LT_EQ(pend, 0);
    LT_EQ(lkd_signal_pending(t), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1) | BIT(LX_SIGWINCH));
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGWINCH, 0, 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_rt_sigpending, P(&pend), 8, 0, 0, 0, 0), 0);
    LT_EQ(pend, BIT(LX_SIGUSR1) | BIT(LX_SIGWINCH));
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGWINCH, P(&ign), 0, 8, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_rt_sigpending, P(&pend), 8, 0, 0, 0, 0), 0);
    LT_EQ(pend, BIT(LX_SIGUSR1));
    /* unblocking: still ignored at delivery, nothing runs */
    fake_engine_reset();
    lkd_sig_set_mask(t, 0);
    LT_EQ(lkd_signal_next(t), 0);
    LT_EQ(fake_frame_build_count, 0);
    LT_ERRNO(sc(t, LKD_NR_rt_sigpending, P(&pend), 9, 0, 0, 0, 0), LX_EINVAL);
}

/* Standard signals coalesce (legacy_queue); real-time signals queue every instance in FIFO order with
 * their value (sigqueue(3), signal(7) "Real-time signals"). Delivery order: lowest number first, the
 * synchronous ones before (next_signal). */
LT_TEST(rt_queue_and_order)
{
    lkd_task *t = mk();
    lkd_sig_set_mask(t, ~0ull);
    for (int i = 0; i < 3; i++) LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    for (int i = 0; i < 3; i++) {
        struct lx_siginfo si;
        memset(&si, 0, sizeof si);
        si.si_code = LX_SI_QUEUE;
        si.u.rt.pid = t->proc->pid;
        si.u.rt.sigval = 100 + (uint64_t)i;
        LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 40, P(&si), 0, 0, 0), 0);
    }
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, 35, 0, 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGSEGV, 0, 0, 0, 0), 0);
    struct lkd_sig_status st;
    LT_EQ(lkd_sig_status(t, &st), 0);
    LT_EQ(st.queued, 6); /* USR1 once, 40 three times, 35, SEGV */
    LT_EQ(st.shdpnd, BIT(LX_SIGUSR1) | BIT(40) | BIT(35) | BIT(LX_SIGSEGV));
    uint64_t all = ~0ull & ~(BIT(LX_SIGKILL) | BIT(LX_SIGSTOP));
    struct lx_siginfo si;
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&all), P(&si), P(&zero), 8, 0, 0), LX_SIGSEGV);
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&all), P(&si), P(&zero), 8, 0, 0), LX_SIGUSR1);
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&all), P(&si), P(&zero), 8, 0, 0), 35);
    for (int i = 0; i < 3; i++) {
        LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&all), P(&si), P(&zero), 8, 0, 0), 40);
        LT_EQ(si.si_code, LX_SI_QUEUE);
        LT_EQ(si.u.rt.sigval, 100 + (uint64_t)i);
    }
    LT_ERRNO(sc(t, LKD_NR_rt_sigtimedwait, P(&all), P(&si), P(&zero), 8, 0, 0), LX_EAGAIN);
    LT_EQ(lkd_sig_status(t, &st), 0);
    LT_EQ(st.queued, 0);
}

/* rt_sigqueueinfo(2): a positive si_code (or SI_TKILL) to another process is EPERM ("not even root can
 * pretend to send signals from the kernel"); the signal number argument overrides si_signo. */
LT_TEST(sigqueueinfo_rules)
{
    lkd_task *t = mk(), *u = mk();
    lkd_set_current(t);
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_signo = 3;
    si.si_code = LX_SI_KERNEL;
    LT_ERRNO(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)u->proc->pid, LX_SIGUSR1, P(&si), 0, 0, 0), LX_EPERM);
    si.si_code = LX_SI_TKILL;
    LT_ERRNO(sc(t, LKD_NR_rt_tgsigqueueinfo, (uint64_t)u->proc->pid, (uint64_t)u->tid, LX_SIGUSR1, P(&si), 0, 0), LX_EPERM);
    si.si_code = LX_SI_QUEUE;
    lkd_sig_set_mask(u, BIT(LX_SIGUSR1));
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)u->proc->pid, LX_SIGUSR1, P(&si), 0, 0, 0), 0);
    uint64_t set = BIT(LX_SIGUSR1);
    struct lx_siginfo got;
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(u, LKD_NR_rt_sigtimedwait, P(&set), P(&got), P(&zero), 8, 0, 0), LX_SIGUSR1);
    LT_EQ(got.si_signo, LX_SIGUSR1);
    LT_EQ(got.si_code, LX_SI_QUEUE);
    LT_ERRNO(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)u->proc->pid, LX_SIGUSR1, 16, 0, 0, 0), LX_EFAULT);
    LT_ERRNO(sc(t, LKD_NR_rt_tgsigqueueinfo, 0, 5, LX_SIGUSR1, P(&si), 0, 0), LX_EINVAL);
}

/* kill(2)/tkill(2)/tgkill(2) argument rules: invalid signal EINVAL, tid <= 0 EINVAL, unknown pid ESRCH,
 * tgkill with the wrong tgid ESRCH, signal 0 probes existence (do_send_specific, kill_something_info
 * with INT_MIN). */
LT_TEST(kill_arguments)
{
    lkd_task *t = mk();
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, 65, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, (uint64_t)-1, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_kill, 999999, 0, 0, 0, 0, 0), LX_ESRCH);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, 0, 0, 0, 0, 0), 0);
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)(uint32_t)INT32_MIN, 0, 0, 0, 0, 0), LX_ESRCH);
    LT_ERRNO(sc(t, LKD_NR_tkill, 0, LX_SIGUSR1, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_tkill, (uint64_t)-5, LX_SIGUSR1, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_tgkill, 0, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_tgkill, (uint64_t)t->proc->pid + 77777, (uint64_t)t->tid, 0, 0, 0, 0), LX_ESRCH);
    LT_EQ(sc(t, LKD_NR_tgkill, (uint64_t)t->proc->pid, (uint64_t)t->tid, 0, 0, 0, 0), 0);
    LT_ERRNO(sc(t, LKD_NR_tkill, 999999, 0, 0, 0, 0, 0), LX_ESRCH);
    LT_ERRNO(sc(t, LKD_NR_pidfd_send_signal, 1000, LX_SIGUSR1, 0, 0, 0, 0), LX_EBADF);
    LT_ERRNO(sc(t, LKD_NR_pidfd_send_signal, 0, LX_SIGUSR1, 0, 8, 0, 0), LX_EINVAL);
}

/* check_kill_permission: a sender whose uid and euid match neither uid nor suid of the target gets
 * EPERM, also for signal 0; SIGCONT inside the same session is allowed; CAP_KILL overrides. */
LT_TEST(kill_permission)
{
    lkd_task *t = mk(), *u = mk();
    u->proc->cred.uid = u->proc->cred.euid = u->proc->cred.suid = 4242;
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, LX_SIGUSR1, 0, 0, 0, 0), LX_EPERM);
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, 0, 0, 0, 0, 0), LX_EPERM);
    LT_ERRNO(sc(t, LKD_NR_tkill, (uint64_t)u->tid, LX_SIGUSR1, 0, 0, 0, 0), LX_EPERM);
    u->proc->sid = t->proc->sid;
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, LX_SIGCONT, 0, 0, 0, 0), 0);
    u->proc->sid = t->proc->sid + 1000;
    LT_ERRNO(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, LX_SIGCONT, 0, 0, 0, 0), LX_EPERM);
    u->proc->cred.suid = t->proc->cred.uid; /* the sender's uid matches the target's saved uid */
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, 0, 0, 0, 0, 0), 0);
    u->proc->cred.suid = 4242;
    t->proc->cred.cap_eff |= 1ull << 5;
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)u->proc->pid, 0, 0, 0, 0, 0), 0);
    t->proc->cred.cap_eff &= ~(1ull << 5);
}

/* A process-directed signal goes to a thread that does not block it (complete_signal), which is
 * kicked; a thread-directed one stays with its thread even when blocked there. */
LT_TEST(process_signal_picks_unblocked_thread)
{
    lkd_task *t = mk();
    lkd_task *th = fake_make_thread(t);
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x6000, 0, 0), 0); /* a handler: the default action would kill */
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1));
    atomic_fetch_and(&th->flags, ~LKD_TF_SIGPENDING);
    fake_engine_reset();
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGUSR1, NULL), 0);
    LT_CHECK(atomic_load(&th->flags) & LKD_TF_SIGPENDING);
    LT_EQ(lkd_signal_pending(t), 0);
    LT_CHECK(lkd_signal_pending(th));
    LT_CHECK(fake_kick_count >= 1);
    LT_CHECK(fake_last_kick == th);
    /* block it in th too and take it synchronously */
    lkd_sig_set_mask(th, BIT(LX_SIGUSR1));
    uint64_t set = BIT(LX_SIGUSR1);
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(th, LKD_NR_rt_sigtimedwait, P(&set), 0, P(&zero), 8, 0, 0), LX_SIGUSR1);
    /* thread-directed to the blocking thread: pending there only */
    LT_EQ(sc(t, LKD_NR_tgkill, (uint64_t)t->proc->pid, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0), 0);
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    LT_EQ(st.pnd, BIT(LX_SIGUSR1));
    lkd_sig_status(th, &st);
    LT_EQ(st.pnd, 0);
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&set), 0, P(&zero), 8, 0, 0), LX_SIGUSR1);
}

/* Restart decisions (arch/x86/kernel/signal.c handle_signal and arch_do_signal_or_restart). */
LT_TEST(restart_decisions)
{
    lkd_task *t = mk();
    /* no signal: ERESTARTSYS/NOHAND/NOINTR restart the same call, RESTARTBLOCK uses restart_syscall */
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_SAME);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOHAND), LKD_RESTART_SAME);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOINTR), LKD_RESTART_SAME);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTART_RESTARTBLOCK), LKD_RESTART_BLOCK);
    /* a handler without SA_RESTART: EINTR except ERESTARTNOINTR */
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x9000, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_NONE);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOHAND), LKD_RESTART_NONE);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTART_RESTARTBLOCK), LKD_RESTART_NONE);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOINTR), LKD_RESTART_SAME);
    /* with SA_RESTART: ERESTARTSYS restarts, NOHAND and RESTARTBLOCK still EINTR */
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x9000, LX_SA_RESTART, 0), 0);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_SAME);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOHAND), LKD_RESTART_NONE);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTART_RESTARTBLOCK), LKD_RESTART_NONE);
    /* through lkd_syscall: an interrupted pause returns EINTR to the guest when a handler runs */
    lkd_sysreq r = { LKD_NR_pause, { 0 } };
    int64_t rax = 0;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_EINTR);
    t->restart.fn = (int64_t (*)(lkd_task *))1;
    LT_EQ(lkd_signal_next(t), 1);
    LT_CHECK(t->restart.fn == NULL); /* a handler run clears the restart block */
    /* an ignored pending signal does not stop a restart */
    lkd_sig_set_mask(t, BIT(LX_SIGWINCH));
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGWINCH, 0, 0, 0, 0), 0);
    lkd_sig_set_mask(t, 0);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_SAME);
    LT_EQ(lkd_signal_next(t), 0);
    /* a fatal default action: EINTR (the task dies anyway) */
    atomic_fetch_or(&t->flags, LKD_TF_FATAL);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_NONE);
}

/* lkd_fault: the forced synchronous signal carries si_code and si_addr exactly (force_sig_fault); with
 * SI_KERNEL (int3, #GP) the address is not filled (force_sig). A blocked or ignored action is reset
 * to SIG_DFL and unblocked (force_sig_info_to_task), so the process dies with a core status. */
LT_TEST(fault_delivery)
{
    lkd_task *t = mk();
    fake_engine_reset();
    LT_EQ(set_handler(t, LX_SIGSEGV, 0x7700, LX_SA_SIGINFO, 0), 0);
    lkd_fault(t, LX_SIGSEGV, LX_SEGV_ACCERR, 0xdead0000, 7, 14);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.info.si_signo, LX_SIGSEGV);
    LT_EQ(fake_last_delivery.info.si_code, LX_SEGV_ACCERR);
    LT_EQ(fake_last_delivery.info.u.fault.addr, 0xdead0000);
    lkd_fault(t, LX_SIGTRAP, LX_SI_KERNEL, 0x1234, 0, 3);
    /* SIGTRAP default: blocked SIGSEGV from the handler mask does not matter here */
    LT_EQ(set_handler(t, LX_SIGTRAP, 0x7800, LX_SA_SIGINFO, 0), 0);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.info.si_code, LX_SI_KERNEL);
    LT_EQ(fake_last_delivery.info.u.fault.addr, 0);
    lkd_fault(t, LX_SIGFPE, LX_FPE_INTDIV, 0x401234, 0, 0);
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGFPE, P(&ign), 0, 8, 0, 0), 0); /* flushes the pending SIGFPE */
    lkd_task *u = mk();
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGILL, P(&ign), 0, 8, 0, 0), 0);
    lkd_sig_set_mask(u, BIT(LX_SIGILL));
    lkd_fault(u, LX_SIGILL, LX_ILL_ILLOPN, 0x400000, 0, 6);
    LT_EQ(atomic_load(&u->sigmask) & BIT(LX_SIGILL), 0);
    struct lx_sigaction o;
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGILL, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_DFL);
    int core = u->proc->rlimits[LX_RLIMIT_CORE].rlim_cur != 0 && u->proc->dumpable;
    LT_EQ(lkd_signal_next(u), -1);
    LT_EQ(u->proc->exit_code & 0x7f, LX_SIGILL);
    LT_EQ((u->proc->exit_code & 0x80) != 0, core);
    LT_CHECK(atomic_load(&u->flags) & LKD_TF_FATAL);
}

/* A fatal signal with SIG_DFL ends the whole process at once (complete_signal): every thread is
 * marked, the first one at a safe point records the exit with status = the signal. */
LT_TEST(fatal_signal_kills_group)
{
    lkd_task *t = mk();
    lkd_task *th = fake_make_thread(t);
    fake_engine_reset();
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGTERM, 0, 0, 0, 0), 0);
    LT_CHECK(atomic_load(&t->flags) & LKD_TF_FATAL);
    LT_CHECK(atomic_load(&th->flags) & LKD_TF_FATAL);
    LT_CHECK(lkd_sig_interrupted(th));
    LT_EQ(lkd_signal_next(th), -1);
    LT_EQ(t->proc->exit_code & 0xff, LX_SIGTERM);
    LT_EQ(lkd_signal_next(t), -1);
    /* signals to a dying process are dropped (prepare_signal with SIGNAL_GROUP_EXIT) */
    struct lkd_sig_status st;
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGUSR1, NULL), 0);
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGUSR1), 0);
}

/* stderr into a temporary file while f runs; returns what was written there. */
static void capture_stderr(void (*f)(void *), void *arg, char *out, size_t cap)
{
    fflush(NULL);
    int save = dup(2);
    char path[] = "/tmp/lkd_sigtrace_XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    dup2(fd, 2);
    close(fd);
    f(arg);
    lseek(2, 0, SEEK_SET);
    ssize_t n = read(2, out, cap - 1);
    out[n > 0 ? n : 0] = 0;
    dup2(save, 2);
    close(save);
}

static void kill_self_term(void *arg)
{
    lkd_task *t = arg;
    (void)sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGTERM, 0, 0, 0, 0);
    (void)lkd_signal_next(t);
}

static void segv_at_0x30(void *arg)
{
    lkd_task *t = arg;
    lkd_fault(t, LX_SIGSEGV, LX_SEGV_MAPERR, 0x30, 4, 14);
    (void)lkd_signal_next(t);
}

/* Debug output: with LKD_TRACE on, the signal that ends a process gets one line naming the thread and the sender,
 * or the faulting address and rip. Off by default: the same deaths print nothing. */
LT_TEST(fatal_signal_trace)
{
    char out[2048];
    fake_engine_reset();
    lkd_task *quiet = mk();
    capture_stderr(kill_self_term, quiet, out, sizeof out);
    LT_EQ(quiet->proc->exit_code & 0xff, LX_SIGTERM);
    LT_CHECK(strstr(out, "dies of signal") == NULL);

    lkd_trace_enable(1);
    lkd_task *t = mk();
    capture_stderr(kill_self_term, t, out, sizeof out);
    char want[160];
    snprintf(want, sizeof want, "pid %d dies of signal %d in tid %d: sent by pid %d (si_code 0)", t->proc->pid,
             LX_SIGTERM, t->tid, t->proc->pid);
    LT_CHECK(strstr(out, want) != NULL);

    lkd_task *u = mk();
    fake_cpu_of(u)->regs.rip = 0x401234;
    capture_stderr(segv_at_0x30, u, out, sizeof out);
    lkd_trace_enable(0);
    LT_EQ(u->proc->exit_code & 0x7f, LX_SIGSEGV);
    snprintf(want, sizeof want, "pid %d dies of signal %d in tid %d: fault at 0x30 (si_code %d), rip 0x401234",
             u->proc->pid, LX_SIGSEGV, u->tid, LX_SEGV_MAPERR);
    LT_CHECK(strstr(out, want) != NULL);
}

/* sigaltstack(2): ENOMEM under MINSIGSTKSZ, EINVAL for an unknown mode, old stack with SS_ONSTACK when
 * the current stack pointer is inside, EPERM when changing it from inside, SA_ONSTACK delivery uses it,
 * SS_AUTODISARM disarms it while the handler runs (signal_delivered). */
LT_TEST(sigaltstack_rules)
{
    lkd_task *t = mk();
    struct lx_stack_t ss = { 0x100000, 0, 0, 1024 }, os;
    LT_ERRNO(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), LX_ENOMEM);
    ss.ss_size = 0x10000;
    ss.ss_flags = 5;
    LT_ERRNO(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), LX_EINVAL);
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_flags, LX_SS_DISABLE);
    ss.ss_flags = 0;
    LT_EQ(sc(t, LKD_NR_sigaltstack, P(&ss), P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_flags, LX_SS_DISABLE);
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_sp, 0x100000);
    LT_EQ(os.ss_size, 0x10000);
    LT_EQ(os.ss_flags, 0);
    fake_cpu_of(t)->regs.rsp = 0x108000; /* inside */
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_flags, LX_SS_ONSTACK);
    LT_ERRNO(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), LX_EPERM);
    fake_cpu_of(t)->regs.rsp = 0x7000000;
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x6000, LX_SA_ONSTACK, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.use_altstack, 1);
    LT_EQ(fake_last_delivery.altstack.ss_sp, 0x100000);
    LT_EQ(fake_last_delivery.altstack.ss_size, 0x10000);
    /* without SA_ONSTACK the normal stack is used */
    LT_EQ(set_handler(t, LX_SIGUSR2, 0x6000, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR2, 0, 0, 0, 0), 0);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.use_altstack, 0);
    /* SS_AUTODISARM */
    ss.ss_flags = (int32_t)LX_SS_AUTODISARM;
    LT_EQ(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), 0);
    lkd_sig_set_mask(t, 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.use_altstack, 1);
    LT_EQ((uint32_t)fake_last_delivery.altstack.ss_flags, LX_SS_AUTODISARM);
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_flags, LX_SS_DISABLE);
    LT_EQ(os.ss_size, 0);
    /* rt_sigreturn restores the saved stack_t (restore_altstack) */
    fake_cpu_of(t)->saved_stack = ss;
    fake_cpu_of(t)->saved_mask = 0;
    lkd_sysreq r = { LKD_NR_rt_sigreturn, { 0 } };
    int64_t rax;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_CONTEXT_REPLACED);
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_size, 0x10000);
    LT_EQ((uint32_t)os.ss_flags, LX_SS_AUTODISARM);
}

/* A frame that cannot be written (ops->frame_build -EFAULT) turns into SIGSEGV (signal_setup_done ->
 * force_sigsegv), which kills when its own handler cannot run either. */
LT_TEST(frame_failure_forces_sigsegv)
{
    lkd_task *t = mk();
    fake_engine_reset();
    fake_frame_build_result = -LX_EFAULT;
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x6000, 0, 0), 0);
    LT_EQ(set_handler(t, LX_SIGSEGV, 0x6100, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    LT_EQ(lkd_signal_next(t), -1);
    LT_EQ(fake_frame_build_count, 2); /* USR1 frame, then the SIGSEGV frame, both failed */
    LT_EQ(t->proc->exit_code & 0x7f, LX_SIGSEGV);
    fake_frame_build_result = 0;
}

struct sender_arg { lkd_task *target; int sig; int delay_ms; int tgkill; };
static void *sender(void *p)
{
    struct sender_arg *a = p;
    usleep((useconds_t)a->delay_ms * 1000);
    /* sent by the target's own process (a sibling thread): SI_TKILL or SI_USER with its pid */
    if (a->tgkill) lkd_sig_send_tid(a->target, a->target->proc->pid, a->target->tid, a->sig, NULL);
    else lkd_sig_send_pid(a->target, a->target->proc->pid, a->sig, NULL);
    return NULL;
}

/* rt_sigtimedwait(2): waits with the set unblocked, returns the signal and its siginfo, EAGAIN at the
 * timeout, EINTR when another (handled) signal interrupts, EINVAL for a bad timespec. */
LT_TEST(sigtimedwait_blocking)
{
    lkd_task *t = mk();
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1));
    uint64_t set = BIT(LX_SIGUSR1);
    struct lx_siginfo si;
    struct lx_timespec ts = { 2, 0 };
    pthread_t th;
    struct sender_arg a = { t, LX_SIGUSR1, 30, 1 };
    pthread_create(&th, NULL, sender, &a);
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0), LX_SIGUSR1);
    LT_EQ(si.si_code, LX_SI_TKILL);
    pthread_join(th, NULL);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGUSR1)); /* the mask is back */
    struct lx_timespec shortts = { 0, 30000000 };
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&shortts), 8, 0, 0), LX_EAGAIN);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 29000000);
    LT_EQ(set_handler(t, LX_SIGUSR2, 0x6000, 0, 0), 0);
    struct sender_arg b = { t, LX_SIGUSR2, 30, 1 };
    pthread_create(&th, NULL, sender, &b);
    LT_ERRNO(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0), LX_EINTR);
    pthread_join(th, NULL);
    LT_EQ(lkd_signal_next(t), 1);
    struct lx_timespec bad = { 0, 1000000000 };
    LT_ERRNO(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&bad), 8, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 4, 0, 0), LX_EINVAL);
}

/* rt_sigsuspend(2): sleeps with the temporary mask until a signal, returns ERESTARTNOHAND (EINTR when
 * a handler runs); the frame saves the ORIGINAL mask and the handler runs with the temporary mask plus
 * the signal (sigmask_to_save / restore_sigmask). With nothing delivered the mask is restored. */
LT_TEST(sigsuspend_masks)
{
    lkd_task *t = mk();
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x6000, 0, 0), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1) | BIT(LX_SIGHUP));
    uint64_t tmp = BIT(LX_SIGHUP) | BIT(LX_SIGINT);
    pthread_t th;
    struct sender_arg a = { t, LX_SIGUSR1, 30, 0 };
    pthread_create(&th, NULL, sender, &a);
    lkd_sysreq r = { LKD_NR_rt_sigsuspend, { P(&tmp), 8 } };
    int64_t rax;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_EINTR);
    pthread_join(th, NULL);
    fake_engine_reset();
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.old_mask, BIT(LX_SIGUSR1) | BIT(LX_SIGHUP));
    LT_EQ(fake_last_delivery.new_mask, tmp | BIT(LX_SIGUSR1));
    LT_EQ(t->restore_sigmask, 0);
    /* the temporary mask without delivery: lkd_signal_next restores the saved one */
    lkd_sig_set_mask(t, BIT(LX_SIGUSR2));
    lkd_sig_set_temp_mask(t, BIT(LX_SIGALRM) | BIT(LX_SIGKILL));
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGALRM));
    LT_CHECK(lkd_signal_pending(t));
    LT_EQ(lkd_signal_next(t), 0);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGUSR2));
    LT_EQ(lkd_signal_pending(t), 0);
    LT_ERRNO(sc(t, LKD_NR_rt_sigsuspend, P(&tmp), 16, 0, 0, 0, 0), LX_EINVAL);
}

/* pause(2) returns -ERESTARTNOHAND once a signal is pending; with no handler (ignored SIGCHLD made
 * pending through a blocked-then-unblocked path) the call is restarted. */
LT_TEST(pause_wakes_on_signal)
{
    lkd_task *t = mk();
    LT_EQ(set_handler(t, LX_SIGALRM, 0x6000, 0, 0), 0);
    pthread_t th;
    struct sender_arg a = { t, LX_SIGALRM, 20, 1 };
    pthread_create(&th, NULL, sender, &a);
    LT_ERRNO(sc(t, LKD_NR_pause, 0, 0, 0, 0, 0, 0), LX_ERESTARTNOHAND);
    pthread_join(th, NULL);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTNOHAND), LKD_RESTART_NONE);
    LT_EQ(lkd_signal_next(t), 1);
}

/* Job control (do_signal_stop, prepare_signal for SIGCONT): SIGSTOP parks every thread at its safe
 * point until SIGCONT; SIGCONT discards pending stop signals and a pending stop discards SIGCONT. */
struct stop_arg { lkd_task *t; _Atomic int phase; int ret; };
static void *stopped_thread(void *p)
{
    struct stop_arg *a = p;
    lkd_set_current(a->t);
    atomic_store(&a->phase, 1);
    a->ret = lkd_signal_next(a->t);
    atomic_store(&a->phase, 2);
    return NULL;
}
LT_TEST(job_control_stop_continue)
{
    lkd_task *t = mk();
    lkd_task *w = fake_make_thread(t);
    struct stop_arg a = { w, 0, 99 };
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGSTOP, NULL), 0);
    LT_CHECK(lkd_signal_pending(t) || lkd_signal_pending(w));
    pthread_t th;
    pthread_create(&th, NULL, stopped_thread, &a);
    /* the worker dequeues SIGSTOP, marks the group and parks */
    for (int i = 0; i < 2000 && lkd_sig_task_stopped(w) != 'T'; i++) usleep(1000);
    LT_EQ(lkd_sig_task_stopped(w), 'T');
    LT_CHECK(atomic_load(&t->flags) & LKD_TF_GROUP_STOP); /* the main thread is asked to stop too */
    usleep(30000);
    LT_EQ(atomic_load(&a.phase), 1); /* still parked: the stop holds */
    struct lkd_sig_status st;
    /* SIGCONT wakes it; a later SIGSTOP flushes a pending SIGCONT (and the reverse) */
    lkd_set_current(t);
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGCONT, NULL), 0);
    for (int i = 0; i < 2000 && atomic_load(&a.phase) != 2; i++) usleep(1000);
    LT_EQ(atomic_load(&a.phase), 2);
    LT_EQ(a.ret, 0);
    pthread_join(th, NULL);
    LT_EQ(lkd_signal_next(t), 0); /* main thread: the stop is over before it parked */
    lkd_sig_set_mask(t, BIT(LX_SIGCONT) | BIT(LX_SIGTSTP));
    lkd_sig_set_mask(w, BIT(LX_SIGCONT) | BIT(LX_SIGTSTP));
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER, 0x2000, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGCONT, P(&h), 0, 8, 0, 0), 0);
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGCONT, NULL), 0);
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd, BIT(LX_SIGCONT));
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGTSTP, NULL), 0);
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd, BIT(LX_SIGTSTP));
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGCONT, NULL), 0);
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd, BIT(LX_SIGCONT));
}

/* SIGCHLD to a parent (do_notify_parent / do_notify_parent_cldstop): exact siginfo; SA_NOCLDSTOP
 * suppresses stop/continue notifications; SIG_IGN and SA_NOCLDWAIT make the child auto-reaped (1). */
LT_TEST(child_events)
{
    lkd_task *p = mk();
    lkd_sig_set_mask(p, BIT(LX_SIGCHLD));
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER | LX_SA_SIGINFO, 0x2000, 0 };
    LT_EQ(sc(p, LKD_NR_rt_sigaction, LX_SIGCHLD, P(&h), 0, 8, 0, 0), 0);
    LT_EQ(lkd_sig_child_event(p->proc->pid, 4321, 1000, LX_CLD_EXITED, 3), 0);
    uint64_t set = BIT(LX_SIGCHLD);
    struct lx_siginfo si;
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(p, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_SIGCHLD);
    LT_EQ(si.si_code, LX_CLD_EXITED);
    LT_EQ(si.u.chld.pid, 4321);
    LT_EQ(si.u.chld.uid, 1000);
    LT_EQ(si.u.chld.status, 3);
    h.flags |= LX_SA_NOCLDSTOP;
    LT_EQ(sc(p, LKD_NR_rt_sigaction, LX_SIGCHLD, P(&h), 0, 8, 0, 0), 0);
    LT_EQ(lkd_sig_child_event(p->proc->pid, 4321, 1000, LX_CLD_STOPPED, LX_SIGSTOP), 0);
    LT_ERRNO(sc(p, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_EAGAIN);
    LT_EQ(lkd_sig_child_event(p->proc->pid, 4321, 1000, LX_CLD_KILLED, LX_SIGKILL), 0);
    LT_EQ(sc(p, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_SIGCHLD);
    LT_EQ(si.si_code, LX_CLD_KILLED);
    LT_EQ(si.u.chld.status, LX_SIGKILL);
    h.flags = LX_SA_RESTORER | LX_SA_NOCLDWAIT;
    LT_EQ(sc(p, LKD_NR_rt_sigaction, LX_SIGCHLD, P(&h), 0, 8, 0, 0), 0);
    LT_EQ(lkd_sig_child_event(p->proc->pid, 4322, 1000, LX_CLD_EXITED, 0), 1);
    LT_EQ(sc(p, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_SIGCHLD);
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(p, LKD_NR_rt_sigaction, LX_SIGCHLD, P(&ign), 0, 8, 0, 0), 0);
    LT_EQ(lkd_sig_child_event(p->proc->pid, 4323, 1000, LX_CLD_DUMPED, LX_SIGSEGV), 1);
    LT_ERRNO(sc(p, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_EAGAIN);
    LT_EQ(lkd_sig_child_event(999999, 1, 0, LX_CLD_EXITED, 0), 0);
}

/* kill(0, sig) signals the caller's process group, kill(-pgrp, sig) a given group, kill(-1, sig) every
 * process except the caller and init (kill_something_info, __kill_pgrp_info). */
LT_TEST(kill_groups)
{
    lkd_task *a = mk();
    (void)sc(a, LKD_NR_setsid, 0, 0, 0, 0, 0, 0);  /* a leads its own group */
    lkd_task *b = lkd_task_alloc(a, 0);            /* a child inherits the process group */
    lkd_task_publish(b);
    fake_cpu_of(b);
    lkd_task *c = mk();
    (void)sc(c, LKD_NR_setsid, 0, 0, 0, 0, 0, 0);
    lkd_set_current(a);
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(b->proc->pid, &pi), 0);
    LT_EQ(pi.pgid, a->proc->pid);
    LT_EQ(lkd_proc_lookup(c->proc->pid, &pi), 0);
    LT_EQ(pi.pgid, c->proc->pid);
    lkd_sig_set_mask(a, ~0ull);
    lkd_sig_set_mask(b, ~0ull);
    lkd_sig_set_mask(c, ~0ull);
    struct lkd_sig_status st;
    LT_EQ(sc(a, LKD_NR_kill, 0, LX_SIGUSR2, 0, 0, 0, 0), 0);
    lkd_sig_status(b, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGUSR2), BIT(LX_SIGUSR2));
    lkd_sig_status(c, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGUSR2), 0);
    LT_EQ(sc(a, LKD_NR_kill, (uint64_t)-c->proc->pid, LX_SIGHUP, 0, 0, 0, 0), 0);
    lkd_sig_status(c, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGHUP), BIT(LX_SIGHUP));
    LT_ERRNO(sc(a, LKD_NR_kill, (uint64_t)-999999, LX_SIGHUP, 0, 0, 0, 0), LX_ESRCH);
    LT_EQ(sc(a, LKD_NR_kill, (uint64_t)-1, LX_SIGWINCH, 0, 0, 0, 0), 0);
    lkd_sig_status(a, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGWINCH), 0); /* not the caller */
    lkd_sig_status(c, &st);
    LT_EQ(st.shdpnd & BIT(LX_SIGWINCH), BIT(LX_SIGWINCH));
    LT_EQ(lkd_sig_send_pgrp(a, a->proc->pid, LX_SIGURG, NULL), 2);
}

/* signalfd(2): reads signalfd_siginfo records of pending signals in its mask (blocked ones), EINVAL
 * for a short buffer, EAGAIN when nonblocking and empty, mask update through the same fd, EINVAL on a
 * non-signalfd descriptor, fdinfo shows the mask (fs/signalfd.c). */
LT_TEST(signalfd_reads)
{
    lkd_task *t = mk();
    t->fdt = lkd_fdtable_new();
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1) | BIT(LX_SIGUSR2));
    uint64_t m = BIT(LX_SIGUSR1);
    int fd = (int)sc(t, LKD_NR_signalfd4, (uint64_t)-1, P(&m), 8, LX_SFD_NONBLOCK | LX_SFD_CLOEXEC, 0, 0);
    LT_CHECK(fd >= 0);
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    LT_CHECK(o && o->kind == LKD_FD_SIGNALFD && o->notify_fd >= 0);
    struct lx_signalfd_siginfo rec[4];
    LT_ERRNO(o->ops->read(t, o, P(rec), sizeof rec, NULL), LX_EAGAIN);
    LT_EQ(o->ops->poll(o, LX_POLLIN), 0);
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_code = LX_SI_QUEUE;
    si.u.rt.pid = t->proc->pid;
    si.u.rt.uid = 1000;
    si.u.rt.sigval = 0x1122334455ull;
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, LX_SIGUSR1, P(&si), 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGUSR2, 0, 0, 0, 0), 0);
    LT_EQ(o->ops->poll(o, LX_POLLIN), LX_POLLIN);
    char nb;
    LT_EQ(read(o->notify_fd, &nb, 1), 1); /* the readiness pipe carries a byte while ready */
    LT_ERRNO(o->ops->read(t, o, P(rec), 100, NULL), LX_EINVAL);
    LT_EQ(o->ops->read(t, o, P(rec), sizeof rec, NULL), (int64_t)sizeof rec[0]);
    LT_EQ(rec[0].ssi_signo, LX_SIGUSR1);
    LT_EQ(rec[0].ssi_code, LX_SI_QUEUE);
    LT_EQ(rec[0].ssi_pid, (uint32_t)t->proc->pid);
    LT_EQ(rec[0].ssi_uid, 1000);
    LT_EQ(rec[0].ssi_ptr, 0x1122334455ull);
    LT_EQ(rec[0].ssi_int, 0x22334455);
    /* SIGUSR2 is not in the mask: still pending for the process */
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd, BIT(LX_SIGUSR2));
    m = BIT(LX_SIGUSR2);
    LT_EQ(sc(t, LKD_NR_signalfd, (uint64_t)fd, P(&m), 8, 0, 0, 0), fd);
    LT_EQ(o->ops->read(t, o, P(rec), sizeof rec, NULL), (int64_t)sizeof rec[0]);
    LT_EQ(rec[0].ssi_signo, LX_SIGUSR2);
    LT_EQ(rec[0].ssi_code, LX_SI_USER);
    char info[128];
    LT_CHECK(o->ops->fdinfo(o, info, sizeof info) > 0);
    LT_STREQ(info, "sigmask:\t0000000000000800\n");
    char d[64];
    LT_CHECK(o->ops->describe(o, d, sizeof d) > 0);
    LT_STREQ(d, "anon_inode:[signalfd]");
    LT_ERRNO(sc(t, LKD_NR_signalfd4, (uint64_t)-1, P(&m), 8, 0x10, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_signalfd4, (uint64_t)-1, P(&m), 4, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(t, LKD_NR_signalfd4, 777, P(&m), 8, 0, 0, 0), LX_EBADF);
    lkd_fdobj_put(o);
    LT_EQ(lkd_fd_close(t, fd), 0);
}

/* A blocking signalfd read sleeps until a signal of its mask arrives (signalfd_dequeue). */
LT_TEST(signalfd_blocking_read)
{
    lkd_task *t = mk();
    t->fdt = lkd_fdtable_new();
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1));
    uint64_t m = BIT(LX_SIGUSR1);
    int fd = (int)sc(t, LKD_NR_signalfd4, (uint64_t)-1, P(&m), 8, 0, 0, 0);
    LT_CHECK(fd >= 0);
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    pthread_t th;
    struct sender_arg a = { t, LX_SIGUSR1, 30, 0 };
    pthread_create(&th, NULL, sender, &a);
    struct lx_signalfd_siginfo rec;
    LT_EQ(o->ops->read(t, o, P(&rec), sizeof rec, NULL), (int64_t)sizeof rec);
    LT_EQ(rec.ssi_signo, LX_SIGUSR1);
    LT_EQ(rec.ssi_code, LX_SI_USER);
    LT_EQ(rec.ssi_pid, (uint32_t)t->proc->pid);
    pthread_join(th, NULL);
    lkd_fdobj_put(o);
    lkd_fd_close(t, fd);
}

/* execve: handlers become SIG_DFL, ignored stay ignored, the mask and pending signals survive, the
 * alternate stack is dropped (flush_signal_handlers, begin_new_exec). */
LT_TEST(exec_resets_handlers)
{
    lkd_task *t = mk();
    LT_EQ(set_handler(t, LX_SIGUSR1, 0x6000, LX_SA_RESTART, BIT(LX_SIGHUP)), 0);
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR2, P(&ign), 0, 8, 0, 0), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGHUP));
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGHUP, 0, 0, 0, 0), 0);
    struct lx_stack_t ss = { 0x100000, 0, 0, 0x10000 };
    LT_EQ(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), 0);
    lkd_sig_exec_reset(t);
    struct lx_sigaction o;
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_DFL);
    LT_EQ(o.flags, 0);
    LT_EQ(o.mask, 0);
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR2, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_IGN);
    LT_EQ(atomic_load(&t->sigmask), BIT(LX_SIGHUP));
    uint64_t pend;
    LT_EQ(sc(t, LKD_NR_rt_sigpending, P(&pend), 8, 0, 0, 0, 0), 0);
    LT_EQ(pend, BIT(LX_SIGHUP));
    struct lx_stack_t os;
    LT_EQ(sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0), 0);
    LT_EQ(os.ss_flags, LX_SS_DISABLE);
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    LT_EQ(st.ign, BIT(LX_SIGUSR2));
    LT_EQ(st.cgt, 0);
}

/* Host re-exec: what execve keeps (mask, ignored dispositions, pending signals with their siginfo,
 * signal(7) "Signals and execve") travels through an exported blob into the new image. */
LT_TEST(exec_state_survives_host_reexec)
{
    lkd_task *t = mk();
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGPIPE, P(&ign), 0, 8, 0, 0), 0);
    LT_EQ(set_handler(t, LX_SIGUSR2, 0x6000, 0, 0), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1) | BIT(40));
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_code = LX_SI_QUEUE;
    si.u.rt.sigval = 7;
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 40, P(&si), 0, 0, 0), 0);
    si.u.rt.sigval = 8;
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 40, P(&si), 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGUSR1, 0, 0, 0, 0), 0);
    size_t need = lkd_sig_exec_export(t, NULL, 0);
    LT_EQ(need, 48 + 3 * 136); /* header, then one record per queued siginfo */
    char *buf = malloc(need);
    LT_EQ(lkd_sig_exec_export(t, buf, need), need);
    lkd_task *u = mk();
    LT_ERRNO(lkd_sig_exec_import(u, buf, 10), LX_EINVAL);
    LT_EQ(lkd_sig_exec_import(u, buf, need), 0);
    free(buf);
    LT_EQ(atomic_load(&u->sigmask), BIT(LX_SIGUSR1) | BIT(40));
    struct lx_sigaction o;
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGPIPE, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_IGN);
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGUSR2, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_DFL);
    uint64_t set = BIT(LX_SIGUSR1) | BIT(40);
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(u, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_SIGUSR1);
    LT_EQ(si.si_code, LX_SI_TKILL);
    LT_EQ(sc(u, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), 40);
    LT_EQ(si.u.rt.sigval, 7);
    LT_EQ(sc(u, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), 40);
    LT_EQ(si.u.rt.sigval, 8);
    LT_ERRNO(sc(u, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), LX_EAGAIN);
}

/* Syscall user dispatch as Wine sets it for Windows code (kernel/entry/syscall_user_dispatch.c): a syscall
 * from outside the allowed region is not run, RAX keeps its number and a forced SIGSYS reports
 * SYS_USER_DISPATCH, the address after the syscall instruction, the number and AUDIT_ARCH_X86_64. Inside the
 * region, with the selector on ALLOW, or with dispatch off, the syscall runs. */
LT_TEST(syscall_user_dispatch)
{
    lkd_task *t = mk();
    fake_engine_reset();
    LT_EQ(set_handler(t, LX_SIGSYS, 0x402000, LX_SA_SIGINFO, 0), 0);
    LT_ERRNO(sc(t, LKD_NR_prctl, 59, 0, 1, 0, 0, 0), LX_EINVAL);             /* OFF takes no region */
    LT_ERRNO(sc(t, LKD_NR_prctl, 59, 3, 0, 0, 0, 0), LX_EINVAL);             /* unknown mode */
    LT_ERRNO(sc(t, LKD_NR_prctl, 59, 1, ~0ull, 2, 0, 0), LX_EINVAL);         /* region overflows */
    LT_EQ(sc(t, LKD_NR_prctl, 59, 1, 0x700000000000ull, 0x100000000000ull, 0, 0), 0);
    lkd_sysreq r = { LKD_NR_getpid, { 0 } };
    int64_t rax;
    fake_cpu_of(t)->regs.rip = 0x14000147b;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, LKD_NR_getpid);
    LT_CHECK(lkd_signal_pending(t));
    LT_EQ(lkd_signal_next(t), 1);
    lkd_delivery *d = &fake_last_delivery;
    LT_EQ(d->signo, LX_SIGSYS);
    LT_EQ(d->info.si_code, LX_SYS_USER_DISPATCH);
    LT_EQ(d->info.u.sys.call_addr, 0x14000147b);
    LT_EQ(d->info.u.sys.syscall, LKD_NR_getpid);
    LT_EQ(d->info.u.sys.arch, LX_AUDIT_ARCH_X86_64);
    LT_EQ(d->handler, 0x402000);
    lkd_sig_set_mask(t, 0);
    fake_cpu_of(t)->regs.rip = 0x7efff5dc0000;                               /* native code */
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, t->proc->pid);
    LT_EQ(lkd_signal_pending(t), 0);
    uint8_t selector = 0;                                                    /* ALLOW */
    LT_EQ(sc(t, LKD_NR_prctl, 59, 1, 0x700000000000ull, 0x100000000000ull, P(&selector), 0), 0);
    fake_cpu_of(t)->regs.rip = 0x14000147b;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, t->proc->pid);
    selector = 1;                                                            /* BLOCK */
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, LKD_NR_getpid);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.signo, LX_SIGSYS);
    lkd_sig_set_mask(t, 0);
    LT_EQ(sc(t, LKD_NR_prctl, 59, 0, 0, 0, 0, 0), 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, t->proc->pid);
    LT_EQ(lkd_signal_pending(t), 0);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    LT_RUN(sigaction_rules);
    LT_RUN(sigprocmask_rules);
    LT_RUN(deliver_handler_frame);
    LT_RUN(nodefer_and_resethand);
    LT_RUN(ignored_signals);
    LT_RUN(rt_queue_and_order);
    LT_RUN(sigqueueinfo_rules);
    LT_RUN(kill_arguments);
    LT_RUN(kill_permission);
    LT_RUN(process_signal_picks_unblocked_thread);
    LT_RUN(restart_decisions);
    LT_RUN(fault_delivery);
    LT_RUN(fatal_signal_kills_group);
    LT_RUN(fatal_signal_trace);
    LT_RUN(sigaltstack_rules);
    LT_RUN(frame_failure_forces_sigsegv);
    LT_RUN(sigtimedwait_blocking);
    LT_RUN(sigsuspend_masks);
    LT_RUN(pause_wakes_on_signal);
    LT_RUN(job_control_stop_continue);
    LT_RUN(child_events);
    LT_RUN(kill_groups);
    LT_RUN(signalfd_reads);
    LT_RUN(signalfd_blocking_read);
    LT_RUN(exec_resets_handlers);
    LT_RUN(exec_state_survives_host_reexec);
    LT_RUN(syscall_user_dispatch);
    return lt_finish();
}
