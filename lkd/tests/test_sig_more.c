/* test_sig_more.c: signal edge cases across threads and a real fork. Linux references:
 * kernel/signal.c v6.12 (complete_signal, prepare_signal, __send_signal_locked, retarget_shared_pending,
 * exit_signals), kernel/fork.c copy_sighand and copy_process (pending sets cleared in the child). */
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "sig.h"


static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq r = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &r);
}
#define P(x) ((uint64_t)(uintptr_t)(x))
#define BIT(s) LX_SIGBIT(s)

static int set_handler(lkd_task *t, int sig, uint64_t flags)
{
    struct lx_sigaction a = { 0x6000, flags | LX_SA_RESTORER, 0x2000, 0 };
    return (int)sc(t, LKD_NR_rt_sigaction, (uint64_t)sig, P(&a), 0, 8, 0, 0);
}

struct parker { lkd_task *t; _Atomic int done; int ret; };
static void *park_main(void *p)
{
    struct parker *a = p;
    lkd_set_current(a->t);
    a->ret = lkd_signal_next(a->t);
    atomic_store(&a->done, 1);
    return NULL;
}

/* SIGKILL ends a job-control stop: the parked thread leaves with -1 and the process exit status is
 * SIGKILL (prepare_signal does not filter SIGKILL; complete_signal starts the group exit). */
LT_TEST(sigkill_while_stopped)
{
    lkd_task *t = fake_make_task();
    lkd_task *w = fake_make_thread(t);
    struct parker a = { w, 0, 99 };
    LT_EQ(lkd_sig_send_tid(t, t->proc->pid, w->tid, LX_SIGSTOP, NULL), 0);
    pthread_t th;
    pthread_create(&th, NULL, park_main, &a);
    for (int i = 0; i < 2000 && lkd_sig_task_stopped(w) != 'T'; i++) usleep(1000);
    LT_EQ(lkd_sig_task_stopped(w), 'T');
    /* a signal sent to the stopped process stays pending, it does not resume it */
    LT_EQ(set_handler(t, LX_SIGUSR1, 0), 0);
    LT_EQ(lkd_sig_send_pid(t, t->proc->pid, LX_SIGUSR1, NULL), 0);
    usleep(20000);
    LT_EQ(atomic_load(&a.done), 0);
    LT_EQ(lkd_sig_send_pid(NULL, t->proc->pid, LX_SIGKILL, NULL), 0);
    for (int i = 0; i < 2000 && !atomic_load(&a.done); i++) usleep(1000);
    pthread_join(th, NULL);
    LT_EQ(a.ret, -1);
    LT_EQ(t->proc->exit_code & 0x7f, LX_SIGKILL);
    LT_EQ(lkd_signal_next(t), -1);
    LT_EQ(lkd_sig_task_stopped(w), 0);
}

/* tgkill(SIGKILL) to one thread kills the whole thread group (complete_signal fatal path). */
LT_TEST(thread_sigkill_kills_group)
{
    lkd_task *t = fake_make_task();
    lkd_task *w = fake_make_thread(t);
    lkd_task *v = fake_make_thread(t);
    LT_EQ(sc(t, LKD_NR_tgkill, (uint64_t)t->proc->pid, (uint64_t)w->tid, LX_SIGKILL, 0, 0, 0), 0);
    LT_CHECK(atomic_load(&t->flags) & LKD_TF_FATAL);
    LT_CHECK(atomic_load(&v->flags) & LKD_TF_FATAL);
    LT_EQ(lkd_signal_next(v), -1);
    LT_EQ(t->proc->exit_code & 0x7f, LX_SIGKILL);
    /* SIGPIPE from the fs module (thread-directed, default action terminate) kills a process too */
    lkd_task *u = fake_make_task();
    lkd_sig_send_task(u, LX_SIGPIPE, NULL, 1);
    LT_EQ(lkd_signal_next(u), -1);
    LT_EQ(u->proc->exit_code & 0x7f, LX_SIGPIPE);
}

/* RLIMIT_SIGPENDING: sigqueue-style RT signals beyond the limit fail with EAGAIN; kill() of an RT
 * signal is a silent loss of the siginfo but the signal stays pending; standard signals are always
 * queued once (__send_signal_locked override_rlimit). */
LT_TEST(rt_queue_limit)
{
    lkd_task *t = fake_make_task();
    t->proc->rlimits[LX_RLIMIT_SIGPENDING].rlim_cur = 2;
    lkd_sig_set_mask(t, ~0ull);
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_code = LX_SI_QUEUE;
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 50, P(&si), 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 50, P(&si), 0, 0, 0), 0);
    LT_ERRNO(sc(t, LKD_NR_rt_sigqueueinfo, (uint64_t)t->proc->pid, 50, P(&si), 0, 0, 0), LX_EAGAIN);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, 51, 0, 0, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_kill, (uint64_t)t->proc->pid, LX_SIGHUP, 0, 0, 0, 0), 0);
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    LT_EQ(st.shdpnd, BIT(50) | BIT(51) | BIT(LX_SIGHUP));
    LT_EQ(st.queued, 3);
    LT_EQ(st.queue_max, 2);
    uint64_t set = BIT(51);
    struct lx_timespec zero = { 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&zero), 8, 0, 0), 51);
    LT_EQ(si.si_code, LX_SI_USER); /* the record was lost: collect_signal's placeholder */
    LT_EQ(si.u.kill.pid, 0);
}

/* copy_sighand: CLONE_SIGHAND shares the actions, otherwise they are copied; CLONE_CLEAR_SIGHAND
 * resets handlers to SIG_DFL but keeps ignored signals. */
LT_TEST(sighand_clone_flags)
{
    lkd_task *t = fake_make_task();
    t->sighand = lkd_sighand_clone(NULL, 0);
    LT_EQ(set_handler(t, LX_SIGUSR1, 0), 0);
    struct lx_sigaction ign = { LX_SIG_IGN, 0, 0, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR2, P(&ign), 0, 8, 0, 0), 0);
    struct lkd_sighand *shared = lkd_sighand_clone(t->sighand, LX_CLONE_SIGHAND | LX_CLONE_VM | LX_CLONE_THREAD);
    LT_CHECK(shared == t->sighand);
    struct lkd_sighand *copy = lkd_sighand_clone(t->sighand, 0);
    LT_CHECK(copy != t->sighand);
    struct lkd_sighand *clear = lkd_sighand_clone(t->sighand, LX_CLONE_CLEAR_SIGHAND);
    lkd_task *u = fake_make_task();
    u->sighand = copy;
    struct lx_sigaction o;
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, 0x6000);
    lkd_task *v = fake_make_task();
    v->sighand = clear;
    LT_EQ(sc(v, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_DFL);
    LT_EQ(sc(v, LKD_NR_rt_sigaction, LX_SIGUSR2, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, LX_SIG_IGN);
    /* the copy is independent */
    LT_EQ(sc(u, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&ign), 0, 8, 0, 0), 0);
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0), 0);
    LT_EQ(o.handler, 0x6000);
    lkd_sighand_put(shared);
}

/* A thread that blocks a pending process signal hands it to another thread (retarget_shared_pending
 * from __set_task_blocked), and so does a thread that exits (exit_signals). */
LT_TEST(retarget_on_block_and_exit)
{
    lkd_task *t = fake_make_task();
    lkd_task *w = fake_make_thread(t);
    LT_EQ(set_handler(t, LX_SIGUSR1, 0), 0);
    lkd_sig_set_mask(w, BIT(LX_SIGUSR1));
    LT_EQ(lkd_sig_send_pid(t, t->proc->pid, LX_SIGUSR1, NULL), 0);
    LT_CHECK(atomic_load(&t->flags) & LKD_TF_SIGPENDING);
    atomic_fetch_and(&w->flags, ~LKD_TF_SIGPENDING);
    lkd_sig_set_mask(w, 0);
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1));
    LT_CHECK(atomic_load(&w->flags) & LKD_TF_SIGPENDING);
    LT_EQ(lkd_signal_pending(t), 0);
    /* t unblocks again, w exits: t is told */
    lkd_sig_set_mask(t, 0);
    atomic_fetch_and(&t->flags, ~LKD_TF_SIGPENDING);
    lkd_sig_task_exit(w);
    LT_CHECK(atomic_load(&t->flags) & LKD_TF_SIGPENDING);
    LT_EQ(lkd_signal_next(t), 1);
    LT_EQ(fake_last_delivery.signo, LX_SIGUSR1);
}

/* fork: the child starts with empty pending sets, keeps the mask, the actions and the alternate stack
 * (copy_process: init_sigpending, copy_sighand). Real fork through lkd_fork_host. */
LT_TEST(fork_child_signal_state)
{
    lkd_task *t = fake_make_task();
    LT_EQ(set_handler(t, LX_SIGUSR1, LX_SA_RESTART), 0);
    lkd_sig_set_mask(t, BIT(LX_SIGUSR1) | BIT(LX_SIGHUP));
    LT_EQ(lkd_sig_send_pid(t, t->proc->pid, LX_SIGUSR1, NULL), 0);
    LT_EQ(sc(t, LKD_NR_tkill, (uint64_t)t->tid, LX_SIGHUP, 0, 0, 0, 0), 0);
    struct lx_stack_t ss = { 0x100000, 0, 0, 0x10000 };
    LT_EQ(sc(t, LKD_NR_sigaltstack, P(&ss), 0, 0, 0, 0, 0), 0);
    int p[2];
    LT_EQ(pipe(p), 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(t, 0);
    if (c == 0) {
        alarm(10);
        struct lkd_sig_status st;
        lkd_sig_status(t, &st);
        struct lx_sigaction o;
        sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, 0, P(&o), 8, 0, 0);
        struct lx_stack_t os;
        sc(t, LKD_NR_sigaltstack, 0, P(&os), 0, 0, 0, 0);
        uint64_t r[6] = { st.pnd, st.shdpnd, st.blk, o.handler, o.flags, os.ss_size };
        (void)!write(p[1], r, sizeof r);
        _exit(0);
    }
    uint64_t r[6] = { 1, 1, 0, 0, 0, 0 };
    LT_EQ(read(p[0], r, sizeof r), (ssize_t)sizeof r);
    LT_EQ(r[0], 0);
    LT_EQ(r[1], 0);
    LT_EQ(r[2], BIT(LX_SIGUSR1) | BIT(LX_SIGHUP));
    LT_EQ(r[3], 0x6000);
    LT_EQ(r[4], LX_SA_RESTART | LX_SA_RESTORER);
    LT_EQ(r[5], 0x10000);
    int st;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* the parent still has both pending */
    struct lkd_sig_status s;
    lkd_sig_status(t, &s);
    LT_EQ(s.pnd, BIT(LX_SIGHUP));
    LT_EQ(s.shdpnd, BIT(LX_SIGUSR1));
    close(p[0]);
    close(p[1]);
}

/* Several threads race signals at one process: every RT instance is delivered exactly once and in
 * per-sender FIFO order (signal(7)); 4 senders x 500 sigqueue values. */
struct qsender { lkd_task *t; int32_t pid; int id; };
static void *qsend_main(void *p)
{
    struct qsender *q = p;
    lkd_set_current(q->t);
    for (int i = 0; i < 500; i++) {
        struct lx_siginfo si;
        memset(&si, 0, sizeof si);
        si.si_code = LX_SI_QUEUE;
        si.u.rt.sigval = ((uint64_t)q->id << 32) | (uint64_t)i;
        while (lkd_sig_send_pid(q->t, q->pid, 45, &si) == -LX_EAGAIN) usleep(100);
    }
    return NULL;
}
LT_TEST(concurrent_rt_senders)
{
    lkd_task *t = fake_make_task();
    t->proc->rlimits[LX_RLIMIT_SIGPENDING].rlim_cur = 4096;
    lkd_sig_set_mask(t, BIT(45));
    struct qsender qs[4];
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        qs[i].t = fake_make_thread(t);
        lkd_sig_set_mask(qs[i].t, BIT(45));
        qs[i].pid = t->proc->pid;
        qs[i].id = i;
        pthread_create(&th[i], NULL, qsend_main, &qs[i]);
    }
    int next[4] = { 0, 0, 0, 0 }, got = 0, order_ok = 1;
    uint64_t set = BIT(45);
    struct lx_timespec ts = { 2, 0 };
    lkd_set_current(t);
    while (got < 2000) {
        struct lx_siginfo si;
        int64_t r = sc(t, LKD_NR_rt_sigtimedwait, P(&set), P(&si), P(&ts), 8, 0, 0);
        if (r != 45) break;
        int id = (int)(si.u.rt.sigval >> 32), n = (int)(uint32_t)si.u.rt.sigval;
        if (id < 0 || id > 3 || n != next[id]) order_ok = 0;
        else next[id]++;
        got++;
    }
    for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
    LT_EQ(got, 2000);
    LT_CHECK(order_ok);
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    LT_EQ(st.queued, 0);
}

/* The w15_async path: rt_sigaction in the main thread, clone with CLONE_VM|FS|FILES|SIGHAND|THREAD|
 * SYSVSEM|PARENT_SETTID|CHILD_CLEARTID, tgkill to the new thread: the shared action runs there
 * (copy_sighand shares sighand_struct with CLONE_SIGHAND). */
LT_TEST(clone_thread_shares_handler)
{
    const char *argv[] = { "w", NULL };
    lkd_main_spec spec = { sizeof spec, "/bin/w", argv, NULL, "/", 0, 0, NULL };
    lkd_task *t = lkd_task_create_main(&spec);
    LT_CHECK(t != NULL);
    if (!t) return;
    lkd_set_current(t);
    fake_cpu_of(t)->regs.rsp = 0x7000000;
    struct lx_sigaction a = { 0x401234, LX_SA_SIGINFO | LX_SA_RESTORER, 0x401000, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&a), 0, 8, 0, 0), 0);
    static int32_t ptid, ctid;
    fake_engine_reset();
    int64_t c = sc(t, LKD_NR_clone, 0x350f00, 0x7ff000, P(&ptid), P(&ctid), 0, 0);
    LT_CHECK(c > 0);
    LT_EQ(fake_spawn_count, 1);
    lkd_task *child = fake_last_spawned;
    LT_CHECK(child != NULL && child->tid == c);
    if (!child) return;
    LT_CHECK(child->sighand == t->sighand);
    LT_EQ(sc(t, LKD_NR_tgkill, (uint64_t)t->proc->pid, (uint64_t)c, LX_SIGUSR1, 0, 0, 0), 0);
    LT_EQ(atomic_load(&t->flags) & LKD_TF_FATAL, 0);
    LT_CHECK(lkd_signal_pending(child));
    LT_CHECK(fake_last_kick == child);
    lkd_set_current(child);
    fake_cpu_of(child)->regs.rsp = 0x7ff000;
    LT_EQ(lkd_signal_next(child), 1);
    LT_EQ(fake_last_delivery.handler, 0x401234);
    LT_EQ(fake_last_delivery.info.si_code, LX_SI_TKILL);
    LT_EQ(fake_last_delivery.info.u.kill.pid, t->proc->pid);
    lkd_set_current(t);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    LT_RUN(sigkill_while_stopped);
    LT_RUN(thread_sigkill_kills_group);
    LT_RUN(rt_queue_limit);
    LT_RUN(sighand_clone_flags);
    LT_RUN(retarget_on_block_and_exit);
    LT_RUN(fork_child_signal_state);
    LT_RUN(concurrent_rt_senders);
    LT_RUN(clone_thread_shares_handler);
    return lt_finish();
}
