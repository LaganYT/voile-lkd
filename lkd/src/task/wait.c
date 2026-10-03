/* wait.c: process death (zombie, reparenting, parent notification) and wait4/waitid.
 *
 * Models kernel/exit.c of Linux v6.12: exit_notify(), forget_original_parent(), find_new_reaper(),
 * do_notify_parent(), do_wait(), wait_task_zombie(), wait_task_stopped(), wait_task_continued().
 *
 * Children are records of the family registry whose ppid is the waiter's pid. A waiter rescans when the
 * registry change counter moves, and at least every 10 ms (a child in another host process cannot wake
 * the waiter's pipe directly). Host children (created by lkd_fork_host here) are also host-reaped with
 * waitpid: their exit is detected even if the child died without running its exit path (killed by a
 * host signal, engine crash); the wait status then comes from the host status, never invented.
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ptrace.h"
#include "sig.h"
#include "task_priv.h"

/* ---- Host children -------------------------------------------------------------------------------- */

struct hostkid { pid_t host; int32_t guest; int reaped; int status; };
#define MAX_HOSTKIDS 256
static pthread_mutex_t g_kids_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hostkid g_kids[MAX_HOSTKIDS];
static int g_nkids;

void wait_note_host_child(pid_t host_pid, int32_t guest_pid)
{
    pthread_mutex_lock(&g_kids_lock);
    for (int i = 0; i < g_nkids; i++)
        if (g_kids[i].host == host_pid) { g_kids[i].guest = guest_pid; pthread_mutex_unlock(&g_kids_lock); return; }
    if (g_nkids < MAX_HOSTKIDS) g_kids[g_nkids++] = (struct hostkid){ host_pid, guest_pid, 0, 0 };
    pthread_mutex_unlock(&g_kids_lock);
}

void wait_forget_host_children(void)
{
    lkd_fork_reinit_mutex(&g_kids_lock, 0);
    g_nkids = 0;
}

static int status_from_host(int st)
{
    if (WIFSIGNALED(st)) {
        int s = lkd_signo_from_host(WTERMSIG(st));
        if (!s) s = LX_SIGKILL;
        return s | (WCOREDUMP(st) ? 0x80 : 0);
    }
    if (WIFEXITED(st)) return (WEXITSTATUS(st) & 0xff) << 8;
    return LX_SIGKILL;
}

static int cld_code_of(int status)
{
    if (LX_WIFEXITED(status)) return LX_CLD_EXITED;
    return (status & 0x80) ? LX_CLD_DUMPED : LX_CLD_KILLED;
}

/* Poll host children without blocking; children that died without publishing become zombies. */
static void poll_host_children(void)
{
    struct { int32_t guest; int status; } dead[MAX_HOSTKIDS];
    int nd = 0;
    pthread_mutex_lock(&g_kids_lock);
    for (int i = 0; i < g_nkids; i++) {
        if (g_kids[i].reaped) continue;
        int st;
        pid_t r = waitpid(g_kids[i].host, &st, WNOHANG);
        if (r == g_kids[i].host) {
            g_kids[i].reaped = 1;
            g_kids[i].status = st;
            dead[nd].guest = g_kids[i].guest;
            dead[nd].status = status_from_host(st);
            nd++;
        } else if (r < 0 && errno == ECHILD) {
            g_kids[i].reaped = 1; /* not ours any more (reaped elsewhere) */
        }
    }
    /* compact: drop entries that are reaped and whose guest record no longer exists */
    pthread_mutex_unlock(&g_kids_lock);
    if (!nd) return;
    int32_t notify_parent[MAX_HOSTKIDS], notify_child[MAX_HOSTKIDS], notify_status[MAX_HOSTKIDS], nn = 0;
    fam_lock();
    for (int i = 0; i < nd; i++) {
        struct fam_proc *r = fam_proc_find(dead[i].guest);
        if (!r || (atomic_load(&r->flags) & RF_ZOMBIE)) continue;
        /* also the case of a vfork child that moved to this host child: same record */
        r->info.state = LKD_PS_ZOMBIE;
        r->info.wait_status = dead[i].status;
        r->info.nthreads = 0;
        r->cld_code = cld_code_of(dead[i].status);
        atomic_fetch_or(&r->flags, RF_ZOMBIE | RF_VFORK_DONE);
        lkd_log("guest pid %d (host child) died without exit notification, host status -> wait status 0x%x",
                dead[i].guest, dead[i].status);
        notify_parent[nn] = r->info.ppid;
        notify_child[nn] = r->info.pid;
        notify_status[nn] = dead[i].status;
        nn++;
    }
    fam_unlock();
    for (int i = 0; i < nn; i++) {
        int st = notify_status[i];
        lkd_sig_child_event(notify_parent[i], notify_child[i], 0, cld_code_of(st),
                            LX_WIFEXITED(st) ? LX_WEXITSTATUS(st) : LX_WTERMSIG(st));
    }
    fam_bump();
}

/* After a zombie record was reaped: reap the host process too if it is our host child. */
static void host_reap(int32_t guest)
{
    pthread_mutex_lock(&g_kids_lock);
    for (int i = 0; i < g_nkids; i++) {
        if (g_kids[i].guest != guest) continue;
        if (!g_kids[i].reaped) {
            /* The child published its zombie just before _exit: give it a moment. */
            for (int k = 0; k < 200; k++) {
                int st;
                pid_t r = waitpid(g_kids[i].host, &st, WNOHANG);
                if (r == g_kids[i].host || (r < 0 && errno == ECHILD)) { g_kids[i].reaped = 1; break; }
                usleep(5000);
            }
        }
        if (g_kids[i].reaped) g_kids[i] = g_kids[--g_nkids];
        break;
    }
    pthread_mutex_unlock(&g_kids_lock);
}

/* ---- Death of a process ----------------------------------------------------------------------------- */

static int32_t find_reaper_locked(int32_t pid)
{
    struct fam_proc *r = fam_proc_find(pid);
    int32_t p = r ? r->info.ppid : 1;
    for (int guard = 0; p > 1 && guard < FAM_MAX_PROCS; guard++) {
        struct fam_proc *a = fam_proc_find(p);
        if (!a) break;
        if (!(atomic_load(&a->flags) & RF_ZOMBIE) && a->info.child_subreaper) return p;
        p = a->info.ppid;
    }
    return 1;
}

/* do_notify_parent(): returns 1 when the child must be released at once (parent ignores SIGCHLD or
 * has SA_NOCLDWAIT, decided by the sig module). */
static int notify_parent(int32_t ppid, int32_t pid, uint32_t uid, int exit_signal, int status)
{
    int code = cld_code_of(status);
    int si_status = LX_WIFEXITED(status) ? LX_WEXITSTATUS(status) : LX_WTERMSIG(status);
    if (exit_signal == LX_SIGCHLD) return lkd_sig_child_event(ppid, pid, uid, code, si_status) == 1;
    if (exit_signal > 0 && exit_signal <= LX_NSIG) {
        struct lx_siginfo si;
        memset(&si, 0, sizeof si);
        si.si_signo = exit_signal;
        si.si_code = code;
        si.u.chld.pid = pid;
        si.u.chld.uid = uid;
        si.u.chld.status = si_status;
        lkd_sig_send_pid(NULL, ppid, exit_signal, &si);
    }
    return 0;
}

int proc_die(lkd_task *t, int status)
{
    struct lkd_proc *p = t->proc;
    struct lkd_waitstate *w = p->wait;
    pthread_mutex_lock(&p->lock);
    int done = w->zombie_done;
    w->zombie_done = 1;
    pthread_mutex_unlock(&p->lock);
    if (done) return !w->cohosted;
    itimers_proc_exit(p);
    struct lx_rusage ru;
    wait_rusage_now(&ru);

    struct note { int32_t to, pid, sig, status, exit_signal; uint32_t uid; } notes[TASK_MAX_CHILDREN * 2 + 8];
    int nn = 0;
    struct { int32_t pid, sig; } pdeath[TASK_MAX_CHILDREN];
    int npd = 0;
    int32_t me = p->pid, parent = 0;
    uint32_t uid = p->cred.uid;
    int release_me = 0;
    pid_t host = getpid();

    fam_lock();
    struct fam_proc *r = w->rec && w->rec->info.pid == me ? w->rec : fam_proc_find(me);
    int32_t reaper = find_reaper_locked(me);
    int have_init = fam_proc_find(1) != NULL;
    if (r) {
        r->info.state = LKD_PS_ZOMBIE;
        r->info.wait_status = status;
        r->info.nthreads = 0;
        r->cld_code = cld_code_of(status);
        r->ru = ru;
        atomic_fetch_or(&r->flags, RF_ZOMBIE | RF_VFORK_DONE);
        parent = r->info.ppid;
        for (int i = 0; i < FAM_MAX_TIDS; i++) {
            struct fam_tid *tr = fam_tid_at(i);
            if (!tr->info.tid || tr->info.tgid != me) continue;
            if (tr->info.tid == me) tr->info.state = LKD_PS_ZOMBIE;
            else fam_tid_free(tr);
        }
    }
    for (int i = 0; i < FAM_MAX_PROCS; i++) {
        struct fam_proc *c = fam_proc_at(i);
        if (!c->info.pid || c == r) continue;
        uint32_t cf = atomic_load(&c->flags);
        if (c->info.ppid == me) {
            /* forget_original_parent(): every child goes to the reaper, gets its pdeath signal */
            __atomic_store_n(&c->info.ppid, reaper, __ATOMIC_RELEASE);
            if (c->pdeathsig && !(cf & RF_ZOMBIE) && npd < TASK_MAX_CHILDREN) {
                pdeath[npd].pid = c->info.pid;
                pdeath[npd].sig = c->pdeathsig;
                npd++;
            }
            if (cf & RF_ZOMBIE) {
                if (reaper == 1 && !have_init) fam_proc_free(c); /* nobody can ever reap it */
                else if (nn < (int)(sizeof notes / sizeof notes[0]))
                    notes[nn++] = (struct note){ reaper, c->info.pid, 0, c->info.wait_status, c->info.exit_signal, c->info.uid };
            }
        } else if (!w->cohosted && (cf & RF_COHOSTED) && !(cf & RF_ZOMBIE) && c->info.host_pid == host) {
            /* A vm-sharing child process lives in our host process: it ends with it (documented gap:
             * Linux would let it run). Reported as killed by SIGKILL. */
            c->info.state = LKD_PS_ZOMBIE;
            c->info.wait_status = LX_SIGKILL;
            c->info.nthreads = 0;
            c->cld_code = LX_CLD_KILLED;
            atomic_fetch_or(&c->flags, RF_ZOMBIE | RF_VFORK_DONE);
            if (nn < (int)(sizeof notes / sizeof notes[0]))
                notes[nn++] = (struct note){ c->info.ppid, c->info.pid, 0, LX_SIGKILL, c->info.exit_signal, c->info.uid };
        }
    }
    if (r && (parent <= 0 || (parent == 1 && !have_init) || !fam_proc_find(parent))) release_me = 1;
    fam_unlock();

    for (int i = 0; i < npd; i++) lkd_sig_send_pid(t, pdeath[i].pid, pdeath[i].sig, NULL);
    for (int i = 0; i < nn; i++) {
        if (notify_parent(notes[i].to, notes[i].pid, notes[i].uid, notes[i].exit_signal ? notes[i].exit_signal : LX_SIGCHLD,
                          notes[i].status)) {
            fam_lock();
            struct fam_proc *c = fam_proc_find(notes[i].pid);
            if (c && (atomic_load(&c->flags) & RF_ZOMBIE)) fam_proc_free(c);
            fam_unlock();
        }
        lkd_task_wake_waiters(notes[i].to);
    }
    if (r && !release_me && notify_parent(parent, me, uid, p->exit_signal, status)) release_me = 1;
    if (release_me) {
        fam_lock();
        struct fam_proc *c = fam_proc_find(me);
        if (c && (atomic_load(&c->flags) & RF_ZOMBIE)) fam_proc_free(c);
        fam_unlock();
        w->rec = NULL;
    }
    if (parent > 0) lkd_task_wake_waiters(parent);
    else fam_bump();
    return !w->cohosted;
}

/* ---- wait4 / waitid ---------------------------------------------------------------------------------- */

struct wsel {
    int which;        /* LX_P_ALL, LX_P_PID, LX_P_PGID */
    int32_t id;
    unsigned options;
};

struct wres {
    int32_t pid;
    int32_t status;   /* wait4 status */
    int code;         /* CLD_* */
    int si_status;
    uint32_t uid;
    struct lx_rusage ru;
};

static int eligible(lkd_task *t, struct fam_proc *c, const struct wsel *s)
{
    if (s->which == LX_P_PID && c->info.pid != s->id) return 0;
    if (s->which == LX_P_PGID && c->info.pgid != s->id) return 0;
    if ((s->options & LX_WNOTHREAD) && c->creator_tid && c->creator_tid != t->tid) return 0;
    /* eligible_child(): __WALL waits for all; otherwise a "clone" child (exit signal other than
     * SIGCHLD) is waited for only with __WCLONE, and the others only without it. */
    if (!(s->options & LX_WALL)) {
        int clone_child = c->info.exit_signal != LX_SIGCHLD;
        if (clone_child != !!(s->options & LX_WCLONE)) return 0;
    }
    return 1;
}

/* One scan. Returns 1 with *res filled, 0 if eligible children exist but none is ready, -ECHILD. */
static int scan(lkd_task *t, const struct wsel *s, struct wres *res)
{
    int32_t me = t->proc->pid;
    int have = 0, found = 0, reaped_pid = 0;
    fam_lock();
    for (int i = 0; i < FAM_MAX_PROCS && !found; i++) {
        struct fam_proc *c = fam_proc_at(i);
        if (!c->info.pid || c->info.ppid != me || c->info.pid == me) continue;
        if (!eligible(t, c, s)) continue;
        have = 1;
        uint32_t f = atomic_load(&c->flags);
        memset(res, 0, sizeof *res);
        res->pid = c->info.pid;
        res->uid = c->info.uid;
        if ((f & RF_ZOMBIE) && (s->options & LX_WEXITED)) {
            res->status = c->info.wait_status;
            res->code = c->cld_code ? c->cld_code : cld_code_of(c->info.wait_status);
            res->si_status = LX_WIFEXITED(res->status) ? LX_WEXITSTATUS(res->status) : LX_WTERMSIG(res->status);
            res->ru = c->ru;
            lx_rusage_add(&res->ru, &c->cru);
            if (!(s->options & LX_WNOWAIT)) {
                struct fam_proc *self = t->proc->wait->rec;
                if (self && self->info.pid == me) {
                    lx_rusage_add(&self->cru, &c->ru);
                    lx_rusage_add(&self->cru, &c->cru);
                }
                reaped_pid = c->info.pid;
                fam_proc_free(c);
            }
            found = 1;
        } else if (!(f & RF_ZOMBIE) && (f & RF_STOP_PENDING) && (s->options & LX_WSTOPPED)) {
            res->status = c->stop_status;
            res->code = LX_CLD_STOPPED;
            res->si_status = LX_WSTOPSIG(c->stop_status);
            if (!(s->options & LX_WNOWAIT)) atomic_fetch_and(&c->flags, ~RF_STOP_PENDING);
            found = 1;
        } else if (!(f & RF_ZOMBIE) && (f & RF_CONT_PENDING) && (s->options & LX_WCONTINUED)) {
            res->status = LX_W_CONTINUED;
            res->code = LX_CLD_CONTINUED;
            res->si_status = LX_SIGCONT;
            if (!(s->options & LX_WNOWAIT)) atomic_fetch_and(&c->flags, ~RF_CONT_PENDING);
            found = 1;
        }
    }
    fam_unlock();
    if (reaped_pid) host_reap(reaped_pid);
    if (found) return 1;
    /* ptrace stops and exits of tracees (rank 61, called without the registry lock) */
    int32_t sel = s->which == LX_P_ALL ? -1 : s->which == LX_P_PID ? s->id : -s->id;
    struct lkd_wait_event ev;
    memset(&ev, 0, sizeof ev);
    if (lkd_ptrace_wait_event(t, sel, s->options, &ev) == 1) {
        memset(res, 0, sizeof *res);
        res->pid = ev.pid;
        res->status = ev.status;
        res->code = ev.code;
        res->uid = ev.uid;
        res->ru = ev.ru;
        /* wait_task_stopped (kernel/exit.c v6.12 l.1358): infop->status = exit_code, the whole stop
         * code (0x405 for PTRACE_EVENT_EXEC, 0x85 for a TRACESYSGOOD syscall stop), not 8 bits */
        res->si_status = (ev.code == LX_CLD_EXITED) ? LX_WEXITSTATUS(ev.status)
                       : (ev.code == LX_CLD_TRAPPED || ev.code == LX_CLD_STOPPED) ? (int)((uint32_t)ev.status >> 8)
                       : LX_WTERMSIG(ev.status);
        return 1;
    }
    /* ECHILD unless a child or a tracee matches the selector (do_wait, ptrace-2) */
    if (have || lkd_ptrace_wait_has(t, sel, s->options)) return 0;
    return -LX_ECHILD;
}

static int64_t do_wait(lkd_task *t, struct wsel *s, struct wres *res)
{
    if (fam_ready() < 0) return -LX_ECHILD;
    int marked = 0;
    int64_t ret;
    for (;;) {
        poll_host_children();
        uint32_t seq = fam_seq();
        int r = scan(t, s, res);
        if (r != 0) { ret = r; break; }
        if (s->options & LX_WNOHANG) { ret = 0; break; }
        if (!marked) {
            task_set_xflags(t, LT_IN_WAIT, 0);
            fam_lock();
            struct fam_tid *tr = fam_tid_find(t->tid);
            if (tr) tr->info.state = LKD_PS_SLEEPING;
            struct fam_proc *pr = t->tid == t->proc->pid && t->proc->wait->rec ? t->proc->wait->rec : NULL;
            if (pr && pr->info.pid == t->proc->pid) pr->info.state = LKD_PS_SLEEPING;
            fam_unlock();
            marked = 1;
        }
        int interrupted = 0;
        /* Sleep until a wake (lkd_task_wake_waiters writes the wake pipe of every LT_IN_WAIT task) or
         * 10 ms (host children reaped by poll_host_children, missed wakes). Before ptrace-2 the 10 ms
         * slice was slept to its end after a wake: up to 11 ms lost per ptrace stop. */
        for (int i = 0; i < 10 && fam_seq() == seq; i++) {
            int w = lkd_block_wait_wake(t, lkd_host_mono_ns() + 10000000);
            if (w == -LX_ERESTARTSYS) { interrupted = 1; break; }
            if (w > 0) break;
        }
        if (interrupted) { ret = -LX_ERESTARTSYS; break; }
    }
    if (marked) {
        task_set_xflags(t, 0, LT_IN_WAIT);
        fam_lock();
        task_publish_locked(t);
        fam_unlock();
    }
    return ret;
}

#define WAIT4_OK (LX_WNOHANG | LX_WUNTRACED | LX_WCONTINUED | LX_WNOTHREAD | LX_WCLONE | LX_WALL)
#define WAITID_OK (LX_WNOHANG | LX_WNOWAIT | LX_WEXITED | LX_WSTOPPED | LX_WCONTINUED | LX_WNOTHREAD | LX_WCLONE | LX_WALL)

LKD_SYSCALL(wait4)
{
    int32_t upid = LKD_ARGI(0);
    unsigned opt = LKD_ARGUI(2);
    if (opt & ~(unsigned)WAIT4_OK) return -LX_EINVAL;
    if (upid == INT32_MIN) return -LX_ESRCH; /* -INT_MIN overflows */
    struct wsel s = { LX_P_ALL, 0, opt | LX_WEXITED };
    if (upid > 0) { s.which = LX_P_PID; s.id = upid; }
    else if (upid == 0) { lkd_proc_refresh_ids(t->proc); s.which = LX_P_PGID; s.id = t->proc->pgid; }
    else if (upid < -1) { s.which = LX_P_PGID; s.id = -upid; }
    struct wres res;
    int64_t r = do_wait(t, &s, &res);
    if (r <= 0) return r;
    if (LKD_ARGP(1) && lkd_user_write(LKD_ARGP(1), &res.status, sizeof res.status)) return -LX_EFAULT;
    if (LKD_ARGP(3) && lkd_user_write(LKD_ARGP(3), &res.ru, sizeof res.ru)) return -LX_EFAULT;
    return res.pid;
}

LKD_SYSCALL(waitid)
{
    int which = LKD_ARGI(0);
    int32_t id = LKD_ARGI(1);
    uint64_t infop = LKD_ARGP(2);
    unsigned opt = LKD_ARGUI(3);
    if (opt & ~(unsigned)WAITID_OK) return -LX_EINVAL;
    if (!(opt & (LX_WEXITED | LX_WSTOPPED | LX_WCONTINUED))) return -LX_EINVAL;
    struct wsel s = { which, id, opt };
    switch (which) {
    case LX_P_ALL: s.id = 0; break;
    case LX_P_PID: if (id <= 0) return -LX_EINVAL; break;
    case LX_P_PGID:
        if (id < 0) return -LX_EINVAL;
        if (id == 0) { lkd_proc_refresh_ids(t->proc); s.id = t->proc->pgid; }
        break;
    case LX_P_PIDFD:
        return -LX_EBADF; /* no pidfd exists in LKD (pidfd_open answers ENOSYS) */
    default:
        return -LX_EINVAL;
    }
    struct wres res;
    memset(&res, 0, sizeof res);
    int64_t r = do_wait(t, &s, &res);
    if (r < 0) return r;
    if (r > 0 && LKD_ARGP(4) && lkd_user_write(LKD_ARGP(4), &res.ru, sizeof res.ru)) return -LX_EFAULT;
    if (!infop) return 0;
    /* SYSCALL_DEFINE5(waitid): with nothing to report (WNOHANG) the fields are written as zero */
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    if (r > 0) {
        si.si_signo = LX_SIGCHLD;
        si.si_code = res.code;
        si.u.chld.pid = res.pid;
        si.u.chld.uid = res.uid;
        si.u.chld.status = res.si_status;
    }
    /* the kernel writes signo, errno, code, pid, uid, status only (16 + 12 bytes) */
    if (lkd_user_write(infop, &si, 16 + 12)) return -LX_EFAULT;
    return 0;
}

/* The parent of a CLONE_VFORK child waits, killable only, until the child releases the memory
 * (exec or exit): kernel/fork.c wait_for_vfork_done(). */
void wait_vfork_parent(lkd_task *t, int32_t child_pid)
{
    for (;;) {
        fam_lock();
        struct fam_proc *c = fam_proc_find(child_pid);
        int done = !c || (atomic_load(&c->flags) & (RF_VFORK_DONE | RF_ZOMBIE));
        fam_unlock();
        if (done) return;
        if (atomic_load(&t->flags) & LKD_TF_FATAL) return;
        poll_host_children();
        usleep(200);
    }
}

void task_wait_register(void)
{
    LKD_REGISTER(wait4);
    LKD_REGISTER(waitid);
}
