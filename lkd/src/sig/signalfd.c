/* signalfd.c: signalfd objects. Owner: sig. Models fs/signalfd.c v6.12 (signalfd_copyinfo,
 * signalfd_dequeue, signalfd_poll, do_signalfd4) and kernel/signal.c siginfo_layout.
 *
 * The object has no host descriptor. Its readiness is signalled through notify_fd (a pipe read end,
 * fd.h): every signal queued in the process for a signal in an object's mask writes one byte to that
 * object's pipe. poll() drains the pipe, recomputes the truth, and puts a byte back while ready, so a
 * level-triggered watcher of notify_fd keeps seeing the object readable. State is per process memory:
 * across a guest fork each process has its own copy (fd.h). */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "net.h"
#include "sig_priv.h"

struct sfd {
    struct sfd *next;
    struct lkd_proc *proc;
    struct lkd_sigproc *sp;
    _Atomic uint64_t mask;
    int notify_rd, notify_wr;
};

static const lkd_fd_ops sfd_ops;

void sig_sfd_notify_locked(struct lkd_sigproc *sp, int sig)
{
    for (struct sfd *s = sp->sfds; s; s = s->next) {
        if (!(atomic_load(&s->mask) & LX_SIGBIT(sig))) continue;
        char c = 1;
        (void)!write(s->notify_wr, &c, 1); /* nonblocking: a full pipe is already readable */
    }
}

static void drain(int fd)
{
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) { }
}

/* kernel/signal.c siginfo_layout */
enum { SIL_KILL, SIL_TIMER, SIL_POLL, SIL_FAULT, SIL_CHLD, SIL_RT, SIL_SYS };
static int siginfo_layout(int sig, int code)
{
    static const struct { int limit, layout; } filter[] = {
        [LX_SIGILL] = { 11, SIL_FAULT }, [LX_SIGFPE] = { 15, SIL_FAULT }, [LX_SIGSEGV] = { 10, SIL_FAULT },
        [LX_SIGBUS] = { 5, SIL_FAULT }, [LX_SIGTRAP] = { 6, SIL_FAULT }, [LX_SIGCHLD] = { 6, SIL_CHLD },
        [LX_SIGPOLL] = { 6, SIL_POLL }, [LX_SIGSYS] = { 2, SIL_SYS },
    };
    int layout = SIL_KILL;
    if (code > LX_SI_USER && code < LX_SI_KERNEL) {
        if (sig < (int)(sizeof filter / sizeof filter[0]) && filter[sig].limit && code <= filter[sig].limit)
            layout = filter[sig].layout;
        else if (code <= 6)
            layout = SIL_POLL;
    } else {
        if (code == LX_SI_TIMER) layout = SIL_TIMER;
        else if (code == LX_SI_SIGIO) layout = SIL_POLL;
        else if (code < 0) layout = SIL_RT;
    }
    return layout;
}

/* signalfd_copyinfo */
void sig_signalfd_info(const struct lx_siginfo *k, struct lx_signalfd_siginfo *n);
void sig_signalfd_info(const struct lx_siginfo *k, struct lx_signalfd_siginfo *n)
{
    memset(n, 0, sizeof *n);
    n->ssi_signo = (uint32_t)k->si_signo;
    n->ssi_errno = k->si_errno;
    n->ssi_code = k->si_code;
    switch (siginfo_layout(k->si_signo, k->si_code)) {
    case SIL_KILL:
        n->ssi_pid = (uint32_t)k->u.kill.pid; n->ssi_uid = k->u.kill.uid; break;
    case SIL_TIMER:
        n->ssi_tid = (uint32_t)k->u.timer.tid; n->ssi_overrun = (uint32_t)k->u.timer.overrun;
        n->ssi_ptr = k->u.timer.sigval; n->ssi_int = (int32_t)k->u.timer.sigval; break;
    case SIL_POLL:
        n->ssi_band = (uint32_t)k->u.poll.band; n->ssi_fd = k->u.poll.fd; break;
    case SIL_FAULT:
        n->ssi_addr = k->u.fault.addr; break;
    case SIL_CHLD:
        n->ssi_pid = (uint32_t)k->u.chld.pid; n->ssi_uid = k->u.chld.uid; n->ssi_status = k->u.chld.status;
        n->ssi_utime = (uint64_t)k->u.chld.utime; n->ssi_stime = (uint64_t)k->u.chld.stime; break;
    case SIL_RT:
        n->ssi_pid = (uint32_t)k->u.rt.pid; n->ssi_uid = k->u.rt.uid;
        n->ssi_ptr = k->u.rt.sigval; n->ssi_int = (int32_t)k->u.rt.sigval; break;
    case SIL_SYS:
        n->ssi_call_addr = k->u.sys.call_addr; n->ssi_syscall = k->u.sys.syscall; n->ssi_arch = k->u.sys.arch; break;
    }
}

static int64_t sfd_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{
    (void)offset;
    struct sfd *s = o->priv;
    uint64_t n = count / sizeof(struct lx_signalfd_siginfo);
    if (!n) return -LX_EINVAL;
    if (t->proc != s->proc) return -LX_EINVAL; /* only readers of the creating process see its queues */
    int nonblock = (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0;
    uint64_t done = 0;
    for (;;) {
        drain(s->notify_rd);
        struct lx_siginfo info;
        sig_lock(s->sp);
        int sig = sig_dequeue_locked(t, s->sp, atomic_load(&s->mask), &info);
        sig_recalc_locked(t, s->sp);
        int more = sig && ((atomic_load(&t->pending.bits) | atomic_load(&s->sp->shared_bits)) & atomic_load(&s->mask));
        sig_unlock(s->sp);
        if (more) { char c = 1; (void)!write(s->notify_wr, &c, 1); }
        if (sig) {
            struct lx_signalfd_siginfo rec;
            sig_signalfd_info(&info, &rec);
            if (lkd_user_write(gva + done * sizeof rec, &rec, sizeof rec) < 0) return done ? (int64_t)(done * sizeof rec) : -LX_EFAULT;
            done++;
            if (done == n) break;
            nonblock = 1;
            continue;
        }
        if (done) break;
        if (nonblock) return -LX_EAGAIN;
        int r = lkd_block_wait_fd(t, s->notify_rd, POLLIN, -1);
        if (r < 0) return r; /* -ERESTARTSYS: read is restartable */
    }
    return (int64_t)(done * sizeof(struct lx_signalfd_siginfo));
}

static uint32_t sfd_poll(lkd_fdobj *o, uint32_t want)
{
    (void)want;
    struct sfd *s = o->priv;
    lkd_task *cur = lkd_current();
    drain(s->notify_rd);
    uint64_t bits = atomic_load(&s->sp->shared_bits);
    if (cur && cur->proc == s->proc) bits |= atomic_load(&cur->pending.bits);
    uint32_t ev = (bits & atomic_load(&s->mask)) ? LX_POLLIN : 0;
    if (ev) { char c = 1; (void)!write(s->notify_wr, &c, 1); }
    return ev;
}

static int sfd_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    (void)o;
    int n = snprintf(buf, cap, "anon_inode:[signalfd]");
    return n < 0 ? -LX_EINVAL : n;
}

static int sfd_fdinfo(lkd_fdobj *o, char *buf, size_t cap)
{
    struct sfd *s = o->priv;
    int n = snprintf(buf, cap, "sigmask:\t%016llx\n", (unsigned long long)atomic_load(&s->mask));
    return n < 0 ? 0 : n;
}

static void sfd_release(lkd_fdobj *o)
{
    struct sfd *s = o->priv;
    if (!s) return;
    sig_lock(s->sp);
    for (struct sfd **pp = &s->sp->sfds; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; break; }
    sig_unlock(s->sp);
    close(s->notify_wr);
    /* notify_rd is o->notify_fd: closed by the fd layer */
    free(s);
    o->priv = NULL;
}

/* SCM_RIGHTS: a record with the mask. A signalfd reads the signals of the process that reads it
 * (fs/signalfd.c signalfd_dequeue works on current), so the receiver gets one with the same mask in its own
 * process. A mask changed later by signalfd(fd, ...) in one process is not seen by the other (fd.h). */
static int sfd_export(lkd_fdobj *o, int *hfds, int cap)
{
    (void)cap;
    struct sfd *s = o->priv;
    uint64_t mask = atomic_load(&s->mask);
    return lkd_fdrec_export(LKD_FDREC_SIGNALFD, atomic_load(&o->status_flags), &mask, sizeof mask, &hfds[0]);
}

static const lkd_fd_ops sfd_ops = {
    .name = "signalfd",
    .read = sfd_read,
    .poll = sfd_poll,
    .describe = sfd_describe,
    .fdinfo = sfd_fdinfo,
    .release = sfd_release,
    .export_fds = sfd_export,
};

/* A signalfd of t's process with this mask (already without SIGKILL and SIGSTOP) and status flags. */
static int sfd_create(lkd_task *t, uint64_t mask, uint32_t status_flags, lkd_fdobj **out)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sfd *s = calloc(1, sizeof *s);
    if (!s) return -LX_ENOMEM;
    int rd, wr;
    int e = lkd_net_make_notify_pipe(&rd, &wr);
    if (e < 0) { free(s); return e; }
    s->proc = t->proc;
    s->sp = sp;
    atomic_init(&s->mask, mask);
    s->notify_rd = rd;
    s->notify_wr = wr;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_SIGNALFD, &sfd_ops, -1, status_flags, s);
    if (!o) { close(rd); close(wr); free(s); return -LX_ENOMEM; }
    o->notify_fd = rd;
    sig_lock(sp);
    s->next = sp->sfds;
    sp->sfds = s;
    if ((atomic_load(&t->pending.bits) | atomic_load(&sp->shared_bits)) & mask) { char c = 1; (void)!write(wr, &c, 1); }
    sig_unlock(sp);
    *out = o;
    return 0;
}

/* LKD_FDREC_SIGNALFD: the mask, in the receiving process. */
static int sfd_rec_import(lkd_task *t, uint32_t fl, const void *data, size_t len, lkd_fdobj **out)
{
    uint64_t mask;
    if (len != sizeof mask || !t || !t->proc) return -LX_EINVAL;
    memcpy(&mask, data, sizeof mask);
    return sfd_create(t, mask & ~LX_SIG_UNBLOCKABLE, LX_O_RDWR | (fl & LX_O_NONBLOCK), out);
}

static int64_t do_signalfd4(lkd_task *t, int fd, uint64_t umask, uint64_t sizemask, uint32_t flags)
{
    if (flags & ~(uint32_t)(LX_SFD_CLOEXEC | LX_SFD_NONBLOCK)) return -LX_EINVAL;
    if (sizemask != LX_SIGSET_BYTES) return -LX_EINVAL;
    uint64_t mask;
    if (lkd_user_read(umask, &mask, sizeof mask) < 0) return -LX_EFAULT;
    mask &= ~LX_SIG_UNBLOCKABLE;
    struct lkd_sigproc *sp = sig_proc(t->proc);
    if (fd == -1) {
        lkd_fdobj *o;
        int e = sfd_create(t, mask, LX_O_RDWR | (flags & LX_SFD_NONBLOCK), &o);
        if (e < 0) return e;
        int nfd = lkd_fd_alloc(t, o, 0, (flags & LX_SFD_CLOEXEC) != 0);
        lkd_fdobj_put(o);
        return nfd;
    }
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    if (o->ops != &sfd_ops || !o->priv) { lkd_fdobj_put(o); return -LX_EINVAL; }
    struct sfd *s = o->priv;
    sig_lock(sp);
    atomic_store(&s->mask, mask);
    sig_unlock(sp);
    char c = 1;
    (void)!write(s->notify_wr, &c, 1); /* readers re-evaluate with the new mask */
    lkd_fdobj_put(o);
    return fd;
}

LKD_SYSCALL(signalfd4) { return do_signalfd4(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), LKD_ARGUI(3)); }
LKD_SYSCALL(signalfd) { return do_signalfd4(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), 0); }

void sig_sfd_fork_child(struct lkd_sigproc *sp, struct lkd_proc *p)
{
    for (struct sfd *s = sp->sfds; s; s = s->next) s->proc = p;
}

void sig_sfd_register(void)
{
    LKD_REGISTER(signalfd);
    LKD_REGISTER(signalfd4);
    lkd_fdrec_register(LKD_FDREC_SIGNALFD, sfd_rec_import);
}
