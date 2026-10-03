/* test_task_proc.c: clone, clone3, fork, vfork, exit, exit_group, wait4, waitid, reparenting and
 * the family registry. Thread cases use the fake engine (spawn_thread records the request, no host
 * thread runs guest code). Fork cases really fork this test process (at most 3 processes at a time)
 * and report from the child through pipes. Models kernel/fork.c and kernel/exit.c of Linux v6.12. */
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "task.h"


static int allow_all(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }

static lkd_task *T;

static int64_t sct(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, 0 } };
    return lkd_dispatch(t, &rq);
}
#define SC0(n) sct(T, LKD_NR_##n, 0, 0, 0, 0, 0)
#define SC1(n, a) sct(T, LKD_NR_##n, (uint64_t)(a), 0, 0, 0, 0)
#define SC2(n, a, b) sct(T, LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), 0, 0, 0)
#define SC3(n, a, b, c) sct(T, LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0)
#define SC4(n, a, b, c, d) sct(T, LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0)
#define SC5(n, a, b, c, d, e) sct(T, LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e))
#define P(x) ((uint64_t)(uintptr_t)(x))

#define GLIBC_THREAD_FLAGS (LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SIGHAND | LX_CLONE_THREAD | \
                            LX_CLONE_SYSVSEM | LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID)
#define GLIBC_FORK_FLAGS (LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID | LX_SIGCHLD)

/* The engine contract for a child process that ends: exit_group, then lkd_task_release says whether
 * the host process must _exit and with which code. */
static void child_exit_group(lkd_task *t, int code)
{
    sct(t, LKD_NR_exit_group, (uint64_t)code, 0, 0, 0, 0);
    int st = 0;
    if (lkd_task_release(t, &st)) _exit(st);
    _exit(99);
}

static void put32(int fd, int32_t v) { (void)!write(fd, &v, sizeof v); }
static int32_t get32(int fd)
{
    int32_t v = -12345;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -12345;
    return v;
}

/* clone(2) with glibc's pthread_create flags: the engine gets the new thread with RAX = 0, the new
 * stack and the TLS base; PARENT_SETTID is written before the thread runs; the tid is new and the
 * process is shared (copy_thread, kernel_clone). */
LT_TEST(thread_clone_glibc_flags)
{
    fake_engine_reset();
    struct fake_cpu *c = fake_cpu_of(T);
    c->regs.rip = 0x401000;
    c->regs.rax = 56;
    c->regs.rbx = 0x1111;
    int32_t ptid = 0;
    uint32_t ctid_word = 0;
    int64_t tid = SC5(clone, GLIBC_THREAD_FLAGS, 0x7000f000, P(&ptid), P(&ctid_word), 0x7000e000);
    LT_CHECK(tid > 0);
    LT_NE(tid, T->tid);
    LT_EQ(ptid, tid);
    LT_EQ(fake_spawn_count, 1);
    lkd_task *ch = fake_last_spawned;
    LT_CHECK(ch != NULL);
    if (!ch) return;
    LT_EQ(fake_last_spawn_regs.rax, 0);
    LT_EQ(fake_last_spawn_regs.rsp, 0x7000f000);
    LT_EQ(fake_last_spawn_regs.fs_base, 0x7000e000);
    LT_EQ(fake_last_spawn_regs.rip, 0x401000);
    LT_EQ(fake_last_spawn_regs.rbx, 0x1111);
    LT_CHECK(ch->proc == T->proc);
    LT_EQ(ch->tid, tid);
    LT_EQ(ch->clear_child_tid, P(&ctid_word));
    LT_EQ(T->proc->nthreads, 2);
    LT_EQ(sct(ch, LKD_NR_getpid, 0, 0, 0, 0, 0), T->proc->pid);
    LT_EQ(sct(ch, LKD_NR_gettid, 0, 0, 0, 0, 0), tid);
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup((int32_t)tid, &ti), 0);
    LT_EQ(ti.tgid, T->proc->pid);
    int32_t tids[8];
    LT_EQ(lkd_tid_list(T->proc->pid, tids, 8), 2);
    /* exit(2) of the thread: clear_child_tid gets 0 (and a futex wake) because the mm has other users */
    ctid_word = (uint32_t)tid;
    LT_EQ(sct(ch, LKD_NR_exit, 0, 0, 0, 0, 0), LKD_RET_EXIT_THREAD);
    LT_EQ(ctid_word, 0);
    LT_EQ(lkd_task_release(ch, NULL), 0);
    LT_EQ(T->proc->nthreads, 1);
    LT_ERRNO(lkd_tid_lookup((int32_t)tid, &ti), LX_ESRCH);
    LT_EQ(lkd_tid_list(T->proc->pid, tids, 8), 1);
}

/* copy_process() flag checks, clone3 argument checks (copy_clone_args_from_user, clone3_args_valid),
 * namespaces without CAP_SYS_ADMIN, CLONE_SETTLS outside the user range (EPERM), spawn failure. */
LT_TEST(clone_validation)
{
    fake_engine_reset();
    LT_ERRNO(SC2(clone, LX_CLONE_THREAD | LX_CLONE_VM, 0), LX_EINVAL);           /* THREAD needs SIGHAND */
    LT_ERRNO(SC2(clone, LX_CLONE_SIGHAND, 0), LX_EINVAL);                        /* SIGHAND needs VM */
    LT_ERRNO(SC2(clone, LX_CLONE_NEWNS | LX_CLONE_FS | LX_SIGCHLD, 0), LX_EINVAL);
    LT_ERRNO(SC2(clone, LX_CLONE_NEWUSER | LX_SIGCHLD, 0), LX_EPERM);
    LT_ERRNO(SC2(clone, LX_CLONE_NEWPID | LX_SIGCHLD, 0), LX_EPERM);
    LT_ERRNO(SC5(clone, GLIBC_THREAD_FLAGS, 0x7000f000, 0, 0, 0x00007ffffffff000ull), LX_EPERM);
    LT_ERRNO(SC2(clone, LX_CLONE_FILES | LX_SIGCHLD, 0), LX_EINVAL);             /* not shareable across hosts */
    LT_ERRNO(SC3(clone, LX_CLONE_PIDFD | LX_CLONE_PARENT_SETTID | LX_SIGCHLD, 0, 8), LX_EINVAL);
    LT_EQ(fake_spawn_count, 0);
    fake_spawn_result = -LX_EAGAIN;
    LT_ERRNO(SC5(clone, GLIBC_THREAD_FLAGS, 0x7000f000, 0, 0, 0x7000e000), LX_EAGAIN);
    fake_spawn_result = 0;
    LT_EQ(T->proc->nthreads, 1);                                                  /* nothing left behind */
    int saved = T->proc->nthreads;
    T->proc->nthreads = 300;                                                     /* machine-safety cap */
    LT_ERRNO(SC5(clone, GLIBC_THREAD_FLAGS, 0x7000f000, 0, 0, 0x7000e000), LX_EAGAIN);
    T->proc->nthreads = saved;
    struct lx_clone_args a;
    memset(&a, 0, sizeof a);
    LT_ERRNO(SC2(clone3, P(&a), 63), LX_EINVAL);
    LT_ERRNO(SC2(clone3, P(&a), 4097), LX_E2BIG);
    uint8_t big[96];
    memset(big, 0, sizeof big);
    big[90] = 1;
    LT_ERRNO(SC2(clone3, P(big), 96), LX_E2BIG);                                  /* unknown nonzero tail */
    a.exit_signal = 65;
    LT_ERRNO(SC2(clone3, P(&a), 88), LX_EINVAL);
    a.exit_signal = LX_SIGCHLD;
    a.flags = GLIBC_THREAD_FLAGS;
    LT_ERRNO(SC2(clone3, P(&a), 88), LX_EINVAL);                                  /* THREAD with exit signal */
    a.exit_signal = 0;
    a.stack = 0x7000f000;
    LT_ERRNO(SC2(clone3, P(&a), 88), LX_EINVAL);                                  /* stack without size */
    a.stack_size = 0x1000;
    a.flags = 0x11;                                                               /* CSIGNAL bits */
    LT_ERRNO(SC2(clone3, P(&a), 88), LX_EINVAL);
    int32_t set_tid = 500;
    a.flags = LX_SIGCHLD & 0;
    a.set_tid = P(&set_tid);
    a.set_tid_size = 1;
    LT_ERRNO(SC2(clone3, P(&a), 88), LX_EPERM);                                   /* CAP_CHECKPOINT_RESTORE */
    a.set_tid = 0; a.set_tid_size = 0;
    a.flags = GLIBC_THREAD_FLAGS;
    a.tls = 0x7000e000;
    int32_t ptid = 0;
    a.parent_tid = P(&ptid);
    int64_t tid = SC2(clone3, P(&a), 88);
    LT_CHECK(tid > 0);
    LT_EQ(ptid, tid);
    LT_EQ(fake_last_spawn_regs.rsp, 0x7000f000 + 0x1000);                         /* stack + stack_size */
    LT_EQ(fake_last_spawn_regs.fs_base, 0x7000e000);
    lkd_task *ch = fake_last_spawned;
    if (ch) {
        sct(ch, LKD_NR_exit, 0, 0, 0, 0, 0);
        LT_EQ(lkd_task_release(ch, NULL), 0);
    }
}

/* do_group_exit(): every other thread is marked to die and kicked; the releasing thread learns that
 * the host process must end, with the exit code for the host. */
LT_TEST(exit_group_kills_threads)
{
    fake_engine_reset();
    lkd_task *p = fake_make_task();          /* a separate guest process in this host process */
    lkd_task *th = fake_make_thread(p);
    LT_EQ(p->proc->nthreads, 2);
    LT_EQ(sct(p, LKD_NR_exit_group, 0x107, 0, 0, 0, 0), LKD_RET_EXIT_GROUP);
    LT_CHECK(atomic_load(&th->flags) & LKD_TF_FATAL);
    LT_CHECK(fake_kick_count >= 1);
    int st = -1;
    LT_EQ(lkd_task_release(p, &st), 1);       /* group exit: the host process ends */
    LT_EQ(st, 7);
    LT_EQ(lkd_task_release(th, &st), 1);
    LT_EQ(st, 7);
    lkd_proc_info pi;
    /* the first process of a family whose parent (init) does not exist is released at once */
    LT_ERRNO(lkd_proc_lookup(p->proc ? 0x3ffffff1 : 0, &pi), LX_ESRCH);
    lkd_set_current(T);
}

/* fork(2) (glibc flags CHILD_SETTID|CHILD_CLEARTID|SIGCHLD): a host process with its own pid, the
 * caller as parent, the tid written into the child's memory; wait4(2) returns the exit status
 * ((code & 0xff) << 8) and the child is gone afterwards (ECHILD). */
LT_TEST(fork_exit_wait4)
{
    int pfd[2];
    LT_EQ(pipe(pfd), 0);
    volatile int32_t ctid_word = 0;
    int32_t me = T->proc->pid;
    fflush(NULL);
    int64_t pid = SC4(clone, GLIBC_FORK_FLAGS, 0, 0, P(&ctid_word));
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getpid, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getppid, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)sct(c, LKD_NR_gettid, 0, 0, 0, 0, 0));
        put32(pfd[1], ctid_word);
        put32(pfd[1], c->proc->nthreads);
        put32(pfd[1], c->proc->host_pid == getpid());
        child_exit_group(c, 42);
    }
    LT_CHECK(pid > me);
    int32_t cpid = get32(pfd[0]), cppid = get32(pfd[0]), ctid = get32(pfd[0]), cword = get32(pfd[0]);
    int32_t cn = get32(pfd[0]), chost = get32(pfd[0]);
    LT_EQ(cpid, pid);
    LT_EQ(cppid, me);
    LT_EQ(ctid, pid);
    LT_EQ(cword, pid);                       /* CLONE_CHILD_SETTID in the child's memory */
    LT_EQ(ctid_word, 0);                     /* not in the parent's */
    LT_EQ(cn, 1);
    LT_EQ(chost, 1);
    lkd_proc_info pi;
    if (lkd_proc_lookup((int32_t)pid, &pi) == 0) LT_EQ(pi.ppid, me);
    else LT_CHECK(1); /* may already be reaped by nobody: still a zombie normally */
    int status = -1;
    struct lx_rusage ru;
    LT_EQ(SC4(wait4, pid, P(&status), 0, P(&ru)), pid);
    LT_EQ(status, 42 << 8);
    LT_CHECK(LX_WIFEXITED(status));
    LT_ERRNO(SC4(wait4, pid, P(&status), 0, 0), LX_ECHILD);
    LT_ERRNO(SC4(wait4, -1, P(&status), LX_WNOHANG, 0), LX_ECHILD);
    LT_ERRNO(lkd_proc_lookup((int32_t)pid, &pi), LX_ESRCH);
    struct lx_rusage kids;
    LT_EQ(SC2(getrusage, LX_RUSAGE_CHILDREN, P(&kids)), 0);
    LT_CHECK(kids.ru_maxrss > 0);             /* the reaped child's usage is accumulated */
    close(pfd[0]); close(pfd[1]);
}

/* wait4 options: WNOHANG on a running child returns 0; waitid(2) with WNOWAIT leaves the zombie;
 * waitid fills si_signo/si_code/si_pid/si_status; invalid options are EINVAL. */
LT_TEST(wait_options)
{
    int go[2];
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        get32(go[0]);
        child_exit_group(c, 3);
    }
    LT_CHECK(pid > 0);
    int status = -1;
    LT_EQ(SC4(wait4, pid, P(&status), LX_WNOHANG, 0), 0);
    struct lx_siginfo si;
    memset(&si, 0x55, sizeof si);
    LT_EQ(SC4(waitid, LX_P_PID, pid, P(&si), LX_WEXITED | LX_WNOHANG), 0);
    LT_EQ(si.si_signo, 0);                    /* nothing to report: zeroed */
    LT_EQ(si.u.chld.pid, 0);
    put32(go[1], 1);
    memset(&si, 0, sizeof si);
    LT_EQ(SC4(waitid, LX_P_PID, pid, P(&si), LX_WEXITED | LX_WNOWAIT), 0);
    LT_EQ(si.si_signo, LX_SIGCHLD);
    LT_EQ(si.si_code, LX_CLD_EXITED);
    LT_EQ(si.u.chld.pid, pid);
    LT_EQ(si.u.chld.status, 3);
    LT_EQ(si.u.chld.uid, 1000);
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup((int32_t)pid, &pi), 0);       /* still a zombie */
    LT_EQ(pi.state, 'Z');
    LT_EQ(SC4(waitid, LX_P_ALL, 0, P(&si), LX_WEXITED), 0);
    LT_EQ(si.u.chld.pid, pid);
    LT_ERRNO(lkd_proc_lookup((int32_t)pid, &pi), LX_ESRCH);
    LT_ERRNO(SC4(waitid, LX_P_ALL, 0, P(&si), LX_WNOHANG), LX_EINVAL);     /* no WEXITED/STOPPED/CONTINUED */
    LT_ERRNO(SC4(waitid, LX_P_PID, 0, P(&si), LX_WEXITED), LX_EINVAL);
    LT_ERRNO(SC4(waitid, LX_P_PIDFD, 3, P(&si), LX_WEXITED), LX_EBADF);
    LT_ERRNO(SC4(waitid, 7, 0, P(&si), LX_WEXITED), LX_EINVAL);
    LT_ERRNO(SC4(wait4, -1, P(&status), 0x10, 0), LX_EINVAL);
    LT_ERRNO(SC4(wait4, INT32_MIN, P(&status), 0, 0), LX_ESRCH);
    LT_ERRNO(SC4(waitid, LX_P_ALL, 0, P(&si), LX_WEXITED | LX_WNOHANG), LX_ECHILD);
    close(go[0]); close(go[1]);
}

/* A child host process killed by a host signal without running its exit path: the parent still gets
 * a truthful status (from the host wait status), WIFSIGNALED with SIGKILL. */
LT_TEST(host_killed_child_is_reported)
{
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        raise(SIGKILL);
        _exit(1);
    }
    LT_CHECK(pid > 0);
    int status = -1;
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_CHECK(LX_WIFSIGNALED(status));
    LT_EQ(LX_WTERMSIG(status), LX_SIGKILL);
}

/* A child created with exit signal 0 is a "clone" child: wait4 without __WCLONE does not see it,
 * __WALL does (eligible_child). */
LT_TEST(clone_child_needs_wclone)
{
    fflush(NULL);
    int64_t pid = SC2(clone, 0, 0);
    if (pid == 0) {
        alarm(20);
        child_exit_group(lkd_current(), 5);
    }
    LT_CHECK(pid > 0);
    int status = -1;
    LT_ERRNO(SC4(wait4, pid, P(&status), 0, 0), LX_ECHILD);
    LT_EQ(SC4(wait4, pid, P(&status), LX_WALL, 0), pid);
    LT_EQ(status, 5 << 8);
}

/* Orphans go to the nearest live ancestor marked PR_SET_CHILD_SUBREAPER (find_new_reaper), which
 * can then wait for them. */
LT_TEST(orphan_reparented_to_subreaper)
{
    int pfd[2], mid_pipe[2];
    LT_EQ(pipe(pfd), 0);
    LT_EQ(pipe(mid_pipe), 0);
    LT_EQ(SC2(prctl, LX_PR_SET_CHILD_SUBREAPER, 1), 0);
    int32_t me = T->proc->pid;
    fflush(NULL);
    int64_t mid = SC1(fork, 0);
    if (mid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        int64_t g = sct(c, LKD_NR_fork, 0, 0, 0, 0, 0);
        if (g == 0) {
            lkd_task *gc = lkd_current();
            int32_t pp = (int32_t)sct(gc, LKD_NR_getppid, 0, 0, 0, 0, 0);
            put32(mid_pipe[1], pp);                             /* the middle process, still alive */
            for (int i = 0; i < 400 && pp != me; i++) {
                usleep(10000);
                pp = (int32_t)sct(gc, LKD_NR_getppid, 0, 0, 0, 0, 0);
            }
            put32(pfd[1], pp);                                  /* after the middle process died */
            child_exit_group(gc, 9);
        }
        int32_t seen = get32(mid_pipe[0]);
        put32(pfd[1], (int32_t)g);
        put32(pfd[1], seen);
        child_exit_group(c, 1);
    }
    LT_CHECK(mid > 0);
    int32_t g = get32(pfd[0]), before = get32(pfd[0]), after = get32(pfd[0]);
    LT_EQ(before, (int32_t)mid);
    LT_EQ(after, me);
    int status = -1;
    LT_EQ(SC4(wait4, mid, P(&status), 0, 0), mid);
    LT_EQ(status, 1 << 8);
    LT_EQ(SC4(wait4, g, P(&status), 0, 0), g);
    LT_EQ(status, 9 << 8);
    LT_EQ(SC2(prctl, LX_PR_SET_CHILD_SUBREAPER, 0), 0);
    close(pfd[0]); close(pfd[1]); close(mid_pipe[0]); close(mid_pipe[1]);
}

/* setpgid(2) by the parent on its child, setsid(2) in a child that is not a group leader. */
LT_TEST(child_process_groups)
{
    int pfd[2], go[2];
    LT_EQ(pipe(pfd), 0);
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getpgrp, 0, 0, 0, 0, 0));
        get32(go[0]);
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getpgrp, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)sct(c, LKD_NR_setsid, 0, 0, 0, 0, 0));
        put32(pfd[1], (int32_t)sct(c, LKD_NR_getsid, 0, 0, 0, 0, 0));
        child_exit_group(c, 0);
    }
    int32_t pg0 = get32(pfd[0]);
    LT_EQ(pg0, T->proc->pid);                 /* inherited group */
    LT_EQ(SC2(setpgid, pid, pid), 0);         /* parent moves the child into its own group */
    LT_EQ(SC1(getpgid, pid), pid);
    put32(go[1], 1);
    LT_EQ(get32(pfd[0]), pid);
    LT_ERRNO(get32(pfd[0]), LX_EPERM);        /* a group leader cannot setsid */
    LT_EQ(get32(pfd[0]), T->proc->pid);       /* session unchanged */
    int status;
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_EQ(status, 0);
    close(pfd[0]); close(pfd[1]); close(go[0]); close(go[1]);
}

/* Job-control stop and continue of a child are reported once each by wait4(WUNTRACED) and
 * waitid(WCONTINUED) (wait_task_stopped, wait_task_continued). */
LT_TEST(stop_and_continue_reports)
{
    int go[2];
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        lkd_task_notify_stop(c, LX_SIGTSTP);
        get32(go[0]);
        lkd_task_notify_continue(c);
        get32(go[0]);
        child_exit_group(c, 0);
    }
    int status = -1;
    LT_EQ(SC4(wait4, pid, P(&status), LX_WUNTRACED, 0), pid);
    LT_CHECK(LX_WIFSTOPPED(status));
    LT_EQ(LX_WSTOPSIG(status), LX_SIGTSTP);
    LT_EQ(SC4(wait4, pid, P(&status), LX_WUNTRACED | LX_WNOHANG, 0), 0);   /* reported once */
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup((int32_t)pid, &pi), 0);
    LT_EQ(pi.state, 'T');
    put32(go[1], 1);
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    LT_EQ(SC4(waitid, LX_P_PID, pid, P(&si), LX_WCONTINUED), 0);
    LT_EQ(si.si_code, LX_CLD_CONTINUED);
    LT_EQ(si.u.chld.status, LX_SIGCONT);
    put32(go[1], 1);
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_EQ(status, 0);
    close(go[0]); close(go[1]);
}

/* A parent blocked in wait4 in another host process is woken by the child's report at once (request
 * ptrace-2: lkd_task_wake_waiters reaches the parent's host process through famsvc, and do_wait returns
 * from its sleep on the wake). Before, the parent saw the change only at its next 10 ms poll of the
 * family counter. 10 rounds: the child stops 3 ms after the parent starts waiting. */
LT_TEST(wait4_wakes_at_once_across_host_processes)
{
    int go[2];
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        for (int i = 0; i < 10; i++) {
            if (get32(go[0]) != 1) break;
            usleep(3000);
            lkd_task_notify_stop(c, LX_SIGTSTP);
        }
        get32(go[0]);
        child_exit_group(c, 0);
    }
    int status = -1, ok = 1;
    int64_t worst = 0, total = 0;
    for (int i = 0; i < 10 && ok; i++) {
        int64_t t0 = lkd_host_mono_ns();
        put32(go[1], 1);
        ok = SC4(wait4, pid, P(&status), LX_WUNTRACED, 0) == pid && LX_WIFSTOPPED(status);
        int64_t d = lkd_host_mono_ns() - t0;
        total += d;
        if (d > worst) worst = d;
    }
    LT_CHECK(ok);
    /* 10 x 3 ms of child delay; the old poll made each round about 10 ms */
    LT_CHECK(total < 65000000);
    if (total >= 65000000) fprintf(stderr, "10 rounds: %lld us, worst %lld us\n", (long long)(total / 1000), (long long)(worst / 1000));
    put32(go[1], 2);
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_EQ(status, 0);
    close(go[0]); close(go[1]);
}

/* Job control end to end with the real sig module (request task-2): the child stops itself with SIGSTOP,
 * its group stop completes in lkd_signal_next, and the parent's wait4(WUNTRACED) reports 0x137f once;
 * SIGCONT from the parent resumes it and waitid(WCONTINUED) reports CLD_CONTINUED (do_signal_stop,
 * wait_task_stopped and wait_task_continued, kernel/signal.c and kernel/exit.c v6.12). */
LT_TEST(sigstop_and_sigcont_reach_wait4)
{
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        sct(c, LKD_NR_kill, (uint64_t)c->proc->pid, LX_SIGSTOP, 0, 0, 0);
        while (lkd_signal_pending(c)) if (lkd_signal_next(c) < 0) _exit(90);   /* parks until SIGCONT */
        child_exit_group(c, 4);
    }
    int status = -1;
    LT_EQ(SC4(wait4, pid, P(&status), LX_WUNTRACED, 0), pid);
    LT_EQ(status, 0x137f);
    LT_EQ(SC4(wait4, pid, P(&status), LX_WUNTRACED | LX_WNOHANG, 0), 0);    /* reported once */
    LT_EQ(SC2(kill, pid, LX_SIGCONT), 0);
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    LT_EQ(SC4(waitid, LX_P_PID, pid, P(&si), LX_WCONTINUED), 0);
    LT_EQ(si.si_code, LX_CLD_CONTINUED);
    LT_EQ(si.u.chld.status, LX_SIGCONT);
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_EQ(status, 4 << 8);
}

struct vfork_arg { int64_t ret; volatile int done; };
static void *do_vfork(void *arg)
{
    struct vfork_arg *a = arg;
    lkd_set_current(T);
    a->ret = SC1(vfork, 0);
    a->done = 1;
    return NULL;
}

/* vfork(2): CLONE_VM|CLONE_VFORK. The child is a separate guest process sharing the parent's memory
 * (it runs as a host thread of this host process); the parent stays blocked until the child execs or
 * exits (wait_for_vfork_done); the child's memory writes are seen by the parent. */
LT_TEST(vfork_child_shares_memory_and_blocks_parent)
{
    fake_engine_reset();
    static volatile int shared_word;
    shared_word = 0;
    struct vfork_arg a = { 0, 0 };
    pthread_t th;
    pthread_create(&th, NULL, do_vfork, &a);
    for (int i = 0; i < 200 && !fake_last_spawned; i++) usleep(5000);
    lkd_task *c = fake_last_spawned;
    LT_CHECK(c != NULL);
    if (!c) { pthread_join(th, NULL); return; }
    LT_EQ(fake_last_spawn_regs.rax, 0);
    LT_CHECK(c->proc != T->proc);
    LT_EQ(sct(c, LKD_NR_getppid, 0, 0, 0, 0, 0), T->proc->pid);
    LT_EQ(sct(c, LKD_NR_getpid, 0, 0, 0, 0, 0), c->proc->pid);
    usleep(50000);
    LT_EQ(a.done, 0);                         /* parent still waiting */
    shared_word = 77;                         /* "child" writes memory (same address space) */
    LT_EQ(sct(c, LKD_NR_exit_group, 127, 0, 0, 0, 0), LKD_RET_EXIT_GROUP);
    int st = -1;
    LT_EQ(lkd_task_release(c, &st), 0);       /* the host process must NOT exit: it is the parent's */
    pthread_join(th, NULL);
    LT_EQ(a.done, 1);
    int32_t cpid = (int32_t)a.ret;
    LT_CHECK(cpid > 0);
    LT_EQ(shared_word, 77);
    int status = -1;
    LT_EQ(SC4(wait4, cpid, P(&status), 0, 0), cpid);
    LT_EQ(status, 127 << 8);
}

/* lkd_proc_lookup and lkd_proc_list show other processes of the family with their name, parent,
 * exe and cmdline (procfs needs this for /proc/<ppid>/{comm,exe,cmdline,status}). */
LT_TEST(registry_sees_other_processes)
{
    int pfd[2], go[2];
    LT_EQ(pipe(pfd), 0);
    LT_EQ(pipe(go), 0);
    SC2(prctl, LX_PR_SET_NAME, P("parentname"));
    int32_t parent = T->proc->pid;  /* in the child, T is the child itself after the identity switch */
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        sct(c, LKD_NR_prctl, LX_PR_SET_NAME, P("childname"), 0, 0, 0);
        lkd_proc_info pi;
        put32(pfd[1], lkd_proc_lookup(parent, &pi) == 0 && !strcmp(pi.comm, "parentname"));
        get32(go[0]);
        child_exit_group(c, 0);
    }
    LT_EQ(get32(pfd[0]), 1);                  /* the child reads its parent's record */
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup((int32_t)pid, &pi), 0);
    LT_STREQ(pi.comm, "childname");
    LT_EQ(pi.ppid, T->proc->pid);
    LT_CHECK(pi.host_pid > 0 && pi.host_pid != getpid());
    LT_EQ(pi.state, 'R');
    lkd_proc_info list[32];
    int n = lkd_proc_list(list, 32), seen = 0;
    for (int i = 0; i < n; i++) {
        if (list[i].pid == pid || list[i].pid == T->proc->pid) seen++;
        if (i) LT_CHECK(list[i - 1].pid < list[i].pid);
    }
    LT_EQ(seen, 2);
    int32_t tids[4];
    LT_EQ(lkd_tid_list((int32_t)pid, tids, 4), 1);
    LT_EQ(tids[0], pid);
    put32(go[1], 1);
    int status;
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    close(pfd[0]); close(pfd[1]); close(go[0]); close(go[1]);
}

/* A parent blocked in wait4 is shown sleeping in the registry (EAC reads /proc/<ppid>/status). */
LT_TEST(waiting_parent_shows_sleeping)
{
    int pfd[2];
    LT_EQ(pipe(pfd), 0);
    int32_t parent = T->proc->pid;
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        lkd_task *c = lkd_current();
        usleep(100000);
        lkd_proc_info pi;
        lkd_proc_lookup(parent, &pi);
        put32(pfd[1], pi.state);
        child_exit_group(c, 0);
    }
    int status;
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    LT_EQ(get32(pfd[0]), 'S');
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(T->proc->pid, &pi), 0);
    LT_EQ(pi.state, 'R');
    close(pfd[0]); close(pfd[1]);
}

/* A thread blocked in an LKD wait (read, poll, accept... all go through lkd_block_poll) is 'S' in the
 * registry, like Linux's TASK_INTERRUPTIBLE in /proc/<pid>/status (request task-1 point 4); it is 'R'
 * again afterwards. */
struct blk_arg { lkd_task *t; volatile int in; };
static void *blocked_sleeper(void *p)
{
    struct blk_arg *a = p;
    lkd_set_current(a->t);
    a->in = 1;
    (void)lkd_block_sleep(a->t, lkd_host_mono_ns() + 300000000ll);
    return NULL;
}
LT_TEST(blocked_thread_shows_sleeping)
{
    lkd_task *t2 = fake_make_thread(T);
    LT_CHECK(t2 != NULL);
    if (!t2) return;
    struct blk_arg a = { t2, 0 };
    pthread_t th;
    pthread_create(&th, NULL, blocked_sleeper, &a);
    for (int i = 0; i < 1000 && !a.in; i++) usleep(1000);
    usleep(50000);
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup(t2->tid, &ti), 0);
    LT_EQ(ti.state, 'S');
    pthread_join(th, NULL);
    LT_EQ(lkd_tid_lookup(t2->tid, &ti), 0);
    LT_EQ(ti.state, 'R');
}

/* A wait interrupted by a signal returns ERESTARTSYS (restartable, CONVENTIONS 4.2). */
static void *poke(void *arg)
{
    usleep(50000);
    atomic_fetch_or(&((lkd_task *)arg)->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(arg);
    return NULL;
}

LT_TEST(wait_is_interruptible)
{
    int go[2];
    LT_EQ(pipe(go), 0);
    fflush(NULL);
    int64_t pid = SC1(fork, 0);
    if (pid == 0) {
        alarm(20);
        get32(go[0]);
        child_exit_group(lkd_current(), 0);
    }
    pthread_t th;
    pthread_create(&th, NULL, poke, T);
    int status;
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), -LX_ERESTARTSYS);
    pthread_join(th, NULL);
    atomic_fetch_and(&T->flags, ~LKD_TF_SIGPENDING);
    put32(go[1], 1);
    LT_EQ(SC4(wait4, pid, P(&status), 0, 0), pid);
    close(go[0]); close(go[1]);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    lkd_usermem_set_check_fn(allow_all);
    T = fake_make_task();
    LT_RUN(thread_clone_glibc_flags);
    LT_RUN(clone_validation);
    LT_RUN(exit_group_kills_threads);
    LT_RUN(fork_exit_wait4);
    LT_RUN(wait_options);
    LT_RUN(host_killed_child_is_reported);
    LT_RUN(clone_child_needs_wclone);
    LT_RUN(orphan_reparented_to_subreaper);
    LT_RUN(child_process_groups);
    LT_RUN(stop_and_continue_reports);
    LT_RUN(wait4_wakes_at_once_across_host_processes);
    LT_RUN(sigstop_and_sigcont_reach_wait4);
    LT_RUN(vfork_child_shares_memory_and_blocks_parent);
    LT_RUN(registry_sees_other_processes);
    LT_RUN(waiting_parent_shows_sleeping);
    LT_RUN(blocked_thread_shows_sleeping);
    LT_RUN(wait_is_interruptible);
    return lt_finish();
}
