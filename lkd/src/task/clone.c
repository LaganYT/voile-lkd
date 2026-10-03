/* clone.c: clone, clone3, fork, vfork, and the task module's fork hooks.
 *
 * Three shapes (validation and flag semantics from kernel/fork.c copy_process()/kernel_clone(), v6.12):
 *   CLONE_THREAD            a new host thread through ops->spawn_thread, same guest process.
 *   CLONE_VM, no THREAD     a new guest process whose memory is the parent's: it cannot be another host
 *                           process, so it runs as a host thread of the parent's host process
 *                           ("cohosted") until it calls execve, where it moves to a host fork() child
 *                           (exec.c). vfork() and glibc posix_spawn() take this path: the parent
 *                           really waits until the child execs or exits, and the child's writes to
 *                           memory (posix_spawn's error report) are seen by the parent, as on Linux.
 *   no CLONE_VM             a host fork() through lkd_fork_host(); task's fork_child hook switches the
 *                           calling task to the child identity (pid allocated by the parent before
 *                           the fork, registry record created before the fork).
 * CLONE_FILES or CLONE_FS without CLONE_VM cannot be honored across two host processes: -EINVAL.
 */
#include <unistd.h>

#include "fd.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task_priv.h"
#include "vfs.h"

int task_new_wake_pipe(lkd_task *t);
void tasks_fork_child_unlock_all(lkd_task *t);

#define CSIGNAL 0xffull
#define TASK_SIZE_MAX 0x00007ffffffff000ull
#define NS_FLAGS (LX_CLONE_NEWNS | LX_CLONE_NEWUTS | LX_CLONE_NEWIPC | LX_CLONE_NEWUSER | LX_CLONE_NEWPID | \
                  LX_CLONE_NEWNET | LX_CLONE_NEWCGROUP | LX_CLONE_NEWTIME)
#define LEGACY_FLAGS 0xffffffffull

/* Fork context: set by the forking thread right before lkd_fork_host. Thread-local, so it survives in
 * the child (the forking thread is the child's only thread) and needs no lock. */
static _Thread_local struct {
    int active;
    int migrate;              /* exec of a cohosted process: keep the identity, become a host process */
    struct lkd_proc *child_proc;
    int32_t child_pid;
    uint64_t flags;
    uint64_t ctid;
} g_fctx;

struct kclone {
    uint64_t flags;
    int exit_signal;
    uint64_t stack;   /* new stack pointer (top), 0 = keep */
    uint64_t ptid, ctid, tls, pidfd;
};

/* ---- Fork hooks ----------------------------------------------------------------------------------- */

void lkd_task_fork_child(lkd_task *t)
{
    tasks_fork_child_unlock_all(t);
    tasks_reset_after_fork(t);
    wait_forget_host_children();
    task_new_wake_pipe(t);
    if (g_fctx.active && g_fctx.migrate) {
        struct lkd_proc *p = t->proc;
        p->host_pid = getpid();
        p->wait->cohosted = 0;
        fam_lock();
        struct fam_proc *r = fam_proc_find(p->pid);
        if (r) {
            r->info.host_pid = p->host_pid;
            atomic_fetch_and(&r->flags, ~RF_COHOSTED);
            p->wait->rec = r;
        }
        fam_unlock();
        g_fctx.active = 0;
        return;
    }
    struct lkd_proc *np;
    int32_t pid;
    uint64_t flags = LX_SIGCHLD, ctid = 0;
    int32_t creator = t->tid;
    if (g_fctx.active) {
        np = g_fctx.child_proc;
        pid = g_fctx.child_pid;
        flags = g_fctx.flags;
        ctid = g_fctx.ctid;
    } else {
        /* lkd_fork_host() called directly (engine or test): the child picks its own identity. */
        pid = lkd_pid_alloc();
        np = pid > 0 ? proc_new_from(t, pid, LX_SIGCHLD) : NULL;
        if (!np) { lkd_log("fork child: no identity available, exiting"); _exit(127); }
    }
    np->host_pid = getpid();
    np->threads = t;
    np->nthreads = 1;
    t->proc = np;
    t->tid = pid;
    t->next_in_proc = NULL;
    t->clear_child_tid = (flags & LX_CLONE_CHILD_CLEARTID) ? ctid : 0;
    t->robust_list_head = t->robust_list_len = 0;  /* copy_process(): p->robust_list = NULL */
    t->pdeathsig = 0;                               /* p->pdeath_signal = 0 */
    t->restart.fn = NULL;
    t->exit_signal_to_parent = (int32_t)(flags & CSIGNAL);
    atomic_fetch_and(&t->flags, ~(LKD_TF_EXITING | LKD_TF_EXEC | LKD_TF_IN_BLOCK));
    fam_lock();
    struct fam_proc *r = fam_proc_find(pid);
    if (!r) proc_register_locked(np, t, pid, t->comm, creator);
    r = fam_proc_find(pid);
    if (r) { np->wait->rec = r; r->info.host_pid = np->host_pid; }
    if (!fam_tid_find(pid)) fam_tid_new(pid, pid);
    task_publish_locked(t);
    fam_unlock();
    g_fctx.active = 0;
}

/* ---- Validation ------------------------------------------------------------------------------------- */

static int64_t validate(const struct kclone *k)
{
    uint64_t f = k->flags;
    if ((f & (LX_CLONE_NEWNS | LX_CLONE_FS)) == (LX_CLONE_NEWNS | LX_CLONE_FS)) return -LX_EINVAL;
    if ((f & (LX_CLONE_NEWUSER | LX_CLONE_FS)) == (LX_CLONE_NEWUSER | LX_CLONE_FS)) return -LX_EINVAL;
    if ((f & LX_CLONE_THREAD) && !(f & LX_CLONE_SIGHAND)) return -LX_EINVAL;
    if ((f & LX_CLONE_SIGHAND) && !(f & LX_CLONE_VM)) return -LX_EINVAL;
    if ((f & LX_CLONE_THREAD) && (f & (LX_CLONE_NEWUSER | LX_CLONE_NEWPID))) return -LX_EINVAL;
    if ((f & LX_CLONE_PIDFD) && (f & LX_CLONE_DETACHED)) return -LX_EINVAL;
    if (f & NS_FLAGS) return -LX_EPERM; /* namespaces need CAP_SYS_ADMIN; user namespaces not provided */
    if (f & LX_CLONE_PIDFD) {
        lkd_log_once("clone with CLONE_PIDFD: pidfds are not provided");
        return -LX_EINVAL;
    }
    if ((f & LX_CLONE_SETTLS) && k->tls >= TASK_SIZE_MAX) return -LX_EPERM; /* do_arch_prctl_64(ARCH_SET_FS) */
    if (!(f & LX_CLONE_VM) && (f & (LX_CLONE_FILES | LX_CLONE_FS))) {
        lkd_log_once("clone flags 0x%llx: CLONE_FILES/CLONE_FS without CLONE_VM cannot be shared across host processes",
                      (unsigned long long)f);
        return -LX_EINVAL;
    }
    return 0;
}

static int ptrace_event_of(const struct kclone *k)
{
    if (k->flags & LX_CLONE_UNTRACED) return 0;
    if (k->flags & LX_CLONE_VFORK) return LX_PTRACE_EVENT_VFORK;
    if (k->exit_signal != LX_SIGCHLD) return LX_PTRACE_EVENT_CLONE;
    return LX_PTRACE_EVENT_FORK;
}

static void put_objects(lkd_task *c)
{
    if (c->mm) lkd_mm_put(c->mm);
    if (c->fdt) lkd_fdtable_put(c->fdt);
    if (c->fsctx) lkd_fsctx_put(c->fsctx);
    if (c->sighand) lkd_sighand_put(c->sighand);
    c->mm = NULL; c->fdt = NULL; c->fsctx = NULL; c->sighand = NULL;
}

static int start_regs(lkd_task *t, const struct kclone *k, lkd_regs *r)
{
    const lkd_engine_ops *ops = lkd_engine();
    if (!ops || ops->get_regs(t, r) < 0) return -LX_EAGAIN;
    r->rax = 0;
    if (k->stack) r->rsp = k->stack;
    if (k->flags & LX_CLONE_SETTLS) { r->fs_base = k->tls; r->fs = 0; }
    return 0;
}

static int64_t check_child_limits(lkd_task *t)
{
    fam_lock();
    int kids = fam_count_children(t->proc->pid), live = fam_count_live();
    fam_unlock();
    if (kids >= TASK_MAX_CHILDREN || live >= TASK_MAX_LIVE_PROCS) {
        lkd_log_once("clone: process limit reached (children %d, family %d): -EAGAIN", kids, live);
        return -LX_EAGAIN;
    }
    return 0;
}

/* ---- Thread and vm-sharing process --------------------------------------------------------------------- */

static int64_t clone_in_process(lkd_task *t, const struct kclone *k)
{
    int thread = (k->flags & LX_CLONE_THREAD) != 0;
    if (thread) {
        pthread_mutex_lock(&t->proc->lock);
        int n = t->proc->nthreads;
        pthread_mutex_unlock(&t->proc->lock);
        if (n >= TASK_MAX_THREADS_PER_PROC) {
            lkd_log_once("clone: %d threads in one process, refusing more (-EAGAIN)", n);
            return -LX_EAGAIN;
        }
    } else {
        int64_t e = check_child_limits(t);
        if (e < 0) return e;
    }
    lkd_regs regs;
    if (start_regs(t, k, &regs) < 0) return -LX_EAGAIN;
    uint64_t f = k->flags | (thread ? 0 : (uint64_t)k->exit_signal);
    lkd_task *c = lkd_task_alloc(t, f);
    if (!c) return -LX_ENOMEM;
    if (!thread) c->proc->wait->cohosted = 1;
    c->mm = lkd_mm_clone(t->mm, f);
    c->fdt = lkd_fdtable_clone(t->fdt, f);
    c->fsctx = lkd_fsctx_clone(t->fsctx, f);
    c->sighand = lkd_sighand_clone(t->sighand, f);
    if ((t->mm && !c->mm) || (t->fdt && !c->fdt) || (t->fsctx && !c->fsctx) || (t->sighand && !c->sighand)) {
        put_objects(c);
        lkd_task_put(c);
        return -LX_ENOMEM;
    }
    lkd_ptrace_task_init(c, t, f);
    c->clear_child_tid = (f & LX_CLONE_CHILD_CLEARTID) ? k->ctid : 0;
    c->start_fs_base = regs.fs_base;
    c->start_gs_base = regs.gs_base;
    lkd_task_publish(c);
    int32_t id = c->tid;
    if (!thread) {
        fam_lock();
        struct fam_proc *r = c->proc->wait->rec;
        if (r && r->info.pid == c->proc->pid) { r->creator_tid = t->tid; r->info.exit_signal = k->exit_signal; }
        fam_unlock();
    }
    if (f & LX_CLONE_PARENT_SETTID) lkd_user_write(k->ptid, &id, sizeof id);
    if (f & LX_CLONE_CHILD_SETTID) lkd_user_write(k->ctid, &id, sizeof id); /* same memory (CLONE_VM) */
    int e = lkd_engine()->spawn_thread(c, &regs);
    if (e < 0) {
        lkd_task_unpublish(c);
        put_objects(c);
        lkd_task_put(c);
        return e;
    }
    int ev = ptrace_event_of(k);
    if (ev) lkd_ptrace_event(t, ev, (uint64_t)id);
    if (!thread && (k->flags & LX_CLONE_VFORK)) {
        wait_vfork_parent(t, id);
        if (!(k->flags & LX_CLONE_UNTRACED)) lkd_ptrace_event(t, LX_PTRACE_EVENT_VFORK_DONE, (uint64_t)id);
    }
    return id;
}

/* ---- Host fork -------------------------------------------------------------------------------------- */

static int64_t clone_fork(lkd_task *t, const struct kclone *k)
{
    int64_t e = check_child_limits(t);
    if (e < 0) return e;
    int32_t pid = lkd_pid_alloc();
    if (pid < 0) return pid;
    uint64_t f = k->flags | (uint64_t)k->exit_signal;
    struct lkd_proc *np = proc_new_from(t, pid, f);
    if (!np) return -LX_ENOMEM;
    fam_lock();
    int re = proc_register_locked(np, NULL, pid, t->comm, t->tid);
    struct fam_proc *r = fam_proc_find(pid);
    if (r) { r->host_parent = getpid(); r->info.exit_signal = k->exit_signal; r->info.state = LKD_PS_RUNNING; }
    fam_unlock();
    if (re < 0) {
        fam_lock();
        fam_proc_free(fam_proc_find(pid));
        fam_unlock();
        proc_free(np);
        return re;
    }
    g_fctx.active = 1;
    g_fctx.migrate = 0;
    g_fctx.child_proc = np;
    g_fctx.child_pid = pid;
    g_fctx.flags = f;
    g_fctx.ctid = k->ctid;
    pid_t hp = lkd_fork_host(t, f);
    g_fctx.active = 0;
    if (hp < 0) {
        fam_lock();
        fam_proc_free(fam_proc_find(pid));
        fam_unlock();
        proc_free(np);
        return hp;
    }
    if (hp == 0) {
        /* child: t is now the child (task fork_child hook). See ptrace.h: the tracer link copied from
         * the parent is in t->ptrace when ptrace decides auto-attach. */
        lkd_ptrace_task_init(t, t, f);
        if (f & LX_CLONE_CHILD_SETTID) lkd_user_write(k->ctid, &pid, sizeof pid); /* schedule_tail() */
        return 0;
    }
    proc_free(np); /* the parent's copy; the child owns its own */
    fam_lock();
    r = fam_proc_find(pid);
    if (r && !r->info.host_pid) r->info.host_pid = hp;
    else if (r) r->info.host_pid = hp;
    fam_unlock();
    wait_note_host_child(hp, pid);
    if (f & LX_CLONE_PARENT_SETTID) lkd_user_write(k->ptid, &pid, sizeof pid);
    int ev = ptrace_event_of(k);
    if (ev) lkd_ptrace_event(t, ev, (uint64_t)pid);
    if (k->flags & LX_CLONE_VFORK) {
        wait_vfork_parent(t, pid);
        if (!(k->flags & LX_CLONE_UNTRACED)) lkd_ptrace_event(t, LX_PTRACE_EVENT_VFORK_DONE, (uint64_t)pid);
    }
    fam_bump();
    return pid;
}

/* exec of a cohosted process: move it to a forked host process. Returns the host pid in the old host
 * process, 0 in the new one, or -errno. */
pid_t clone_migrate_fork(lkd_task *t)
{
    g_fctx.active = 1;
    g_fctx.migrate = 1;
    pid_t hp = lkd_fork_host(t, LX_SIGCHLD);
    g_fctx.active = 0;
    g_fctx.migrate = 0;
    return hp;
}

static int64_t do_clone(lkd_task *t, struct kclone *k)
{
    int64_t e = validate(k);
    if (e < 0) return e;
    if (k->flags & LX_CLONE_THREAD) k->exit_signal = 0;
    if (k->flags & LX_CLONE_VM) return clone_in_process(t, k);
    return clone_fork(t, k);
}

/* ---- Syscalls ----------------------------------------------------------------------------------------- */

LKD_SYSCALL(clone)
{
    /* x86-64: clone(flags, newsp, parent_tidptr, child_tidptr, tls) */
    uint64_t raw = LKD_ARGU(0) & LEGACY_FLAGS;
    struct kclone k = { raw & ~CSIGNAL, (int)(raw & CSIGNAL), LKD_ARGU(1), LKD_ARGP(2), LKD_ARGP(3), LKD_ARGU(4), LKD_ARGP(2) };
    if ((k.flags & LX_CLONE_PIDFD) && (k.flags & LX_CLONE_PARENT_SETTID)) return -LX_EINVAL; /* same pointer */
    if (k.exit_signal > LX_NSIG) return -LX_EINVAL;
    return do_clone(t, &k);
}

LKD_SYSCALL(fork)
{
    struct kclone k = { 0, LX_SIGCHLD, 0, 0, 0, 0, 0 };
    return do_clone(t, &k);
}

LKD_SYSCALL(vfork)
{
    struct kclone k = { LX_CLONE_VFORK | LX_CLONE_VM, LX_SIGCHLD, 0, 0, 0, 0, 0 };
    return do_clone(t, &k);
}

LKD_SYSCALL(clone3)
{
    uint64_t uargs = LKD_ARGP(0), size = LKD_ARGU(1);
    struct lx_clone_args a;
    memset(&a, 0, sizeof a);
    /* copy_clone_args_from_user() */
    if (size > 4096) return -LX_E2BIG;
    if (size < LX_CLONE_ARGS_SIZE_VER0) return -LX_EINVAL;
    if (lkd_user_read(uargs, &a, size < sizeof a ? size : sizeof a)) return -LX_EFAULT;
    for (uint64_t off = sizeof a; off < size; off++) {
        uint8_t b;
        if (lkd_user_read(uargs + off, &b, 1)) return -LX_EFAULT;
        if (b) return -LX_E2BIG;
    }
    if (a.set_tid_size > 32) return -LX_EINVAL;           /* MAX_PID_NS_LEVEL */
    if (!a.set_tid && a.set_tid_size) return -LX_EINVAL;
    if (a.set_tid && !a.set_tid_size) return -LX_EINVAL;
    if ((a.exit_signal & ~CSIGNAL) || a.exit_signal > LX_NSIG) return -LX_EINVAL;
    if ((a.flags & LX_CLONE_INTO_CGROUP) && (a.cgroup > 0x7fffffff || size < LX_CLONE_ARGS_SIZE_VER2)) return -LX_EINVAL;
    /* clone3_args_valid() */
    if (a.flags & ~(LEGACY_FLAGS | LX_CLONE_CLEAR_SIGHAND | LX_CLONE_INTO_CGROUP)) return -LX_EINVAL;
    if (a.flags & (LX_CLONE_DETACHED | (CSIGNAL & ~LX_CLONE_NEWTIME))) return -LX_EINVAL;
    if ((a.flags & (LX_CLONE_SIGHAND | LX_CLONE_CLEAR_SIGHAND)) == (LX_CLONE_SIGHAND | LX_CLONE_CLEAR_SIGHAND)) return -LX_EINVAL;
    if ((a.flags & (LX_CLONE_THREAD | LX_CLONE_PARENT)) && a.exit_signal) return -LX_EINVAL;
    if ((a.stack == 0) != (a.stack_size == 0)) return -LX_EINVAL; /* clone3_stack_valid() */
    if (a.set_tid_size) return -LX_EPERM;      /* choosing pids needs CAP_CHECKPOINT_RESTORE */
    if (a.flags & LX_CLONE_INTO_CGROUP) return -LX_EBADF; /* no cgroup file system in LKD */
    if ((a.flags & LX_CLONE_PIDFD) && (a.flags & LX_CLONE_PARENT_SETTID) && a.pidfd == a.parent_tid) return -LX_EINVAL;
    struct kclone k = { a.flags, (int)a.exit_signal, a.stack ? a.stack + a.stack_size : 0, a.parent_tid, a.child_tid,
                        a.tls, a.pidfd };
    return do_clone(t, &k);
}

void task_clone_register(void)
{
    LKD_REGISTER(clone);
    LKD_REGISTER(clone3);
    LKD_REGISTER(fork);
    LKD_REGISTER(vfork);
}
