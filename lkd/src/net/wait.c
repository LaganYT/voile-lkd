/* wait.c: readiness of fd objects, interruptible host polling, poll/ppoll/select/pselect6.
 * Models fs/select.c of Linux 6.12 (do_sys_poll, do_select, poll_select_finish, core_sys_select).
 *
 * An object is waited for through one host descriptor: notify_fd when the object has no
 * kernel-visible readiness (eventfd, signalfd, timerfd, epoll), else host_fd. The truth reported to
 * the guest comes from ops->poll when the object has one, else from the host revents translated to
 * Linux bits. Objects with neither descriptor are always ready (DEFAULT_POLLMASK).
 */
#include <fcntl.h>
#include <unistd.h>

#include "net_priv.h"

#define DEFAULT_POLLMASK (LX_POLLIN | LX_POLLOUT | LX_POLLRDNORM | LX_POLLWRNORM)
#define POLL_IN_BITS (LX_POLLIN | LX_POLLRDNORM | LX_POLLRDBAND | LX_POLLRDHUP | LX_POLLPRI)
#define POLL_OUT_BITS (LX_POLLOUT | LX_POLLWRNORM | LX_POLLWRBAND)

uint32_t net_host_revents_to_lx(short h)
{
    uint32_t r = 0;
    if (h & POLLIN) r |= LX_POLLIN | LX_POLLRDNORM;
    if (h & POLLRDBAND) r |= LX_POLLRDBAND;
    if (h & POLLPRI) r |= LX_POLLPRI;
    if (h & POLLOUT) r |= LX_POLLOUT | LX_POLLWRNORM;
    if (h & POLLWRBAND) r |= LX_POLLWRBAND;
    if (h & POLLERR) r |= LX_POLLERR;
    if (h & POLLHUP) r |= LX_POLLHUP;
    if (h & POLLNVAL) r |= LX_POLLNVAL;
    return r;
}

int net_obj_waitsrc(lkd_fdobj *o, uint32_t want, int *hfd, short *hev)
{
    if (o->notify_fd >= 0) {
        *hfd = o->notify_fd;
        *hev = POLLIN;
        return 1;
    }
    if (o->host_fd >= 0) {
        short e = 0;
        if (want & (LX_POLLIN | LX_POLLRDNORM | LX_POLLRDBAND | LX_POLLRDHUP)) e |= POLLIN;
        if (want & LX_POLLPRI) e |= POLLPRI;
        if (want & POLL_OUT_BITS) e |= POLLOUT;
        *hfd = o->host_fd;
        *hev = e;
        return 1;
    }
    return 0;
}

uint32_t net_obj_revents(lkd_fdobj *o, uint32_t want, short hrev, int have_hrev)
{
    if (o->ops && o->ops->poll) return o->ops->poll(o, want | NET_POLL_ALWAYS);
    if (o->host_fd < 0) return DEFAULT_POLLMASK;
    if (!have_hrev) {
        int hfd;
        short hev;
        net_obj_waitsrc(o, want, &hfd, &hev);
        struct pollfd p = { .fd = hfd, .events = hev, .revents = 0 };
        if (poll(&p, 1, 0) < 0) return LX_POLLERR;
        hrev = p.revents;
    }
    return net_host_revents_to_lx(hrev);
}

/* ---- Interruptible host poll ------------------------------------------------------------------- */

static int timeout_ms(int64_t deadline_ns)
{
    if (deadline_ns < 0) return -1;
    int64_t left = deadline_ns - lkd_host_mono_ns();
    if (left <= 0) return 0;
    int64_t ms = (left + 999999) / 1000000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}

/* Same protocol as lkd_block_poll (core/block.c): interruption test before sleeping, the task's wake
 * pipe polled with the fds, drained on wake-up. No limit on the number of fds. */
int net_block_poll(lkd_task *t, struct pollfd *pf, unsigned n, int64_t deadline_ns)
{
    if (n < 1024) return lkd_block_poll(t, pf, n, deadline_ns);
    struct pollfd *local = malloc((n + 1) * sizeof *local);
    if (!local) return -LX_ENOMEM;
    int ret;
    atomic_fetch_or(&t->flags, LKD_TF_IN_BLOCK);
    for (;;) {
        if (lkd_sig_interrupted(t)) { ret = -LX_ERESTARTSYS; break; }
        memcpy(local, pf, n * sizeof *pf);
        local[n].fd = t->wake_rd;
        local[n].events = POLLIN;
        local[n].revents = 0;
        int r = poll(local, n + 1, timeout_ms(deadline_ns));
        if (r < 0) {
            if (errno == EINTR) continue;
            ret = (int)LKD_HOST_ERR();
            break;
        }
        if (local[n].revents & POLLIN) {
            char buf[64];
            while (read(t->wake_rd, buf, sizeof buf) > 0) { }
            if (lkd_sig_interrupted(t)) { ret = -LX_ERESTARTSYS; break; }
            r--;
        }
        if (r > 0) {
            int ready = 0;
            for (unsigned i = 0; i < n; i++) {
                pf[i].revents = local[i].revents;
                if (local[i].revents) ready++;
            }
            if (ready) { ret = ready; break; }
        }
        if (r == 0 && deadline_ns >= 0 && lkd_host_mono_ns() >= deadline_ns) { ret = 0; break; }
    }
    atomic_fetch_and(&t->flags, ~LKD_TF_IN_BLOCK);
    free(local);
    return ret;
}

int64_t net_deadline_from_ms(int64_t ms)
{
    if (ms < 0) return -1;
    return lkd_host_mono_ns() + ms * 1000000;
}

int net_ts_valid(const struct lx_timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000;
}

int64_t net_deadline_from_ts(const struct lx_timespec *ts)
{
    if (!ts) return -1;
    int64_t now = lkd_host_mono_ns();
    if (ts->tv_sec > (INT64_MAX - now) / 1000000000 - 1) return -1; /* beyond any horizon: forever */
    return now + ts->tv_sec * 1000000000 + ts->tv_nsec;
}

/* ---- Signal masks of ppoll/pselect6/epoll_pwait ------------------------------------------------ */

int net_install_sigmask(lkd_task *t, uint64_t gva, uint64_t size)
{
    if (!gva) return 0;
    if (size != LX_SIGSET_BYTES) return -LX_EINVAL;
    uint64_t m;
    if (lkd_user_read(gva, &m, sizeof m) < 0) return -LX_EFAULT;
    lkd_sig_set_temp_mask(t, m);
    return 1;
}

/* kernel/signal.c restore_saved_sigmask_unless(interrupted): keep the temporary mask for the signal
 * frame when the call was interrupted, restore it now otherwise. */
void net_finish_temp_mask(lkd_task *t, int64_t ret)
{
    if (!t->restore_sigmask) return;
    if (ret == -LX_ERESTARTNOHAND || ret == -LX_EINTR || ret == -LX_ERESTARTSYS) return;
    lkd_sig_set_mask(t, t->saved_sigmask);
    t->restore_sigmask = 0;
}

/* ---- lkd_net_wait_object (net.h) and the notify pipe ------------------------------------------- */

int lkd_net_wait_object(lkd_task *t, lkd_fdobj *o, uint32_t want, int64_t deadline_ns)
{
    uint32_t mask = want | NET_POLL_ALWAYS;
    for (;;) {
        uint32_t r = net_obj_revents(o, want, 0, 0) & mask;
        if (r) return (int)r;
        int hfd;
        short hev;
        if (!net_obj_waitsrc(o, want, &hfd, &hev)) {
            int s = lkd_block_sleep(t, deadline_ns);
            if (s < 0) return s;
            return 0;
        }
        int w = lkd_block_wait_fd(t, hfd, hev, deadline_ns);
        if (w < 0) return w;
        if (w == 0) {
            r = net_obj_revents(o, want, 0, 0) & mask;
            return (int)r;
        }
    }
}

int lkd_net_make_notify_pipe(int *rd, int *wr)
{
    int p[2];
    if (pipe(p) < 0) return (int)LKD_HOST_ERR();
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
        fcntl(p[i], F_SETNOSIGPIPE, 1);
    }
    *rd = p[0];
    *wr = p[1];
    return 0;
}

/* Signal queued: signalfd objects are woken by the sig module through their own notify pipes, and every
 * blocking wait of the target polls its wake pipe; nothing else to do here. */
void lkd_net_signal_queued(lkd_task *t) { (void)t; }

/* ---- poll / ppoll ------------------------------------------------------------------------------ */

struct pent { lkd_fdobj *o; uint32_t mask; int widx; };

static unsigned nofile_limit(lkd_task *t)
{
    uint64_t v = t && t->proc ? t->proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur : 1024;
    return v > (1u << 20) ? (1u << 20) : (unsigned)v;
}

/* do_sys_poll: returns the ready count, or -ERESTARTNOHAND when interrupted with nothing ready. */
static int64_t do_poll(lkd_task *t, uint64_t ufds, uint64_t nfds, int64_t deadline)
{
    if (nfds > nofile_limit(t)) return -LX_EINVAL;
    struct lx_pollfd *pfd = NULL;
    struct pent *pe = NULL;
    struct pollfd *hp = NULL;
    int64_t ret = 0;
    if (nfds) {
        pfd = malloc(nfds * sizeof *pfd);
        pe = calloc(nfds, sizeof *pe);
        hp = malloc(nfds * sizeof *hp);
        if (!pfd || !pe || !hp) { ret = -LX_ENOMEM; goto out; }
        if (lkd_user_read(ufds, pfd, nfds * sizeof *pfd) < 0) { ret = -LX_EFAULT; goto out; }
    }
    unsigned nh = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        pe[i].widx = -1;
        pfd[i].revents = 0;
        if (pfd[i].fd < 0) continue;
        pe[i].o = lkd_fd_get(t, pfd[i].fd, NULL);
        if (!pe[i].o) continue;
        pe[i].mask = (uint16_t)pfd[i].events | LX_POLLERR | LX_POLLHUP;
        int hfd;
        short hev;
        if (net_obj_waitsrc(pe[i].o, pe[i].mask, &hfd, &hev)) {
            hp[nh].fd = hfd;
            hp[nh].events = hev;
            hp[nh].revents = 0;
            pe[i].widx = (int)nh++;
        }
    }
    /* First pass without sleeping: one zero-timeout host poll for the whole set. */
    if (nh && poll(hp, nh, 0) < 0) { ret = LKD_HOST_ERR(); goto out; }
    for (;;) {
        int count = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            if (pfd[i].fd < 0) continue;
            uint32_t r;
            if (!pe[i].o) r = LX_POLLNVAL;
            else {
                short h = pe[i].widx >= 0 ? hp[pe[i].widx].revents : 0;
                r = net_obj_revents(pe[i].o, pe[i].mask, h, pe[i].widx >= 0) & pe[i].mask;
            }
            pfd[i].revents = (int16_t)r;
            if (r) count++;
        }
        if (count) { ret = count; break; }
        if (deadline == 0 || (deadline > 0 && lkd_host_mono_ns() >= deadline)) { ret = 0; break; }
        for (unsigned k = 0; k < nh; k++) hp[k].revents = 0;
        int w = net_block_poll(t, hp, nh, deadline);
        if (w == -LX_ERESTARTSYS) { ret = -LX_ERESTARTNOHAND; break; }
        if (w < 0) { ret = w; break; }
        if (w == 0) {
            /* Timed out: re-evaluate once more (objects without host readiness), then return. */
            if (nh && poll(hp, nh, 0) < 0) { ret = LKD_HOST_ERR(); break; }
            deadline = 0;
        }
    }
    if (ret >= 0) {
        /* Linux writes back every revents field (fs/select.c do_sys_poll: __put_user per entry). */
        for (uint64_t i = 0; i < nfds; i++) {
            uint64_t a = ufds + i * sizeof(struct lx_pollfd) + offsetof(struct lx_pollfd, revents);
            if (lkd_user_write(a, &pfd[i].revents, sizeof pfd[i].revents) < 0) { ret = -LX_EFAULT; break; }
        }
    }
out:
    if (pe) for (uint64_t i = 0; i < nfds; i++) if (pe[i].o) lkd_fdobj_put(pe[i].o);
    free(pfd);
    free(pe);
    free(hp);
    return ret;
}

/* restart_block of poll(2): fs/select.c do_restart_poll. arg[0] ufds, [1] nfds, [2] deadline (host
 * monotonic ns) or -1. */
static int64_t poll_restart(lkd_task *t)
{
    uint64_t ufds = t->restart.arg[0], nfds = t->restart.arg[1];
    int64_t deadline = (int64_t)t->restart.arg[2];
    int64_t r = do_poll(t, ufds, nfds, deadline);
    if (r == -LX_ERESTARTNOHAND) {
        t->restart.fn = poll_restart;
        r = -LX_ERESTART_RESTARTBLOCK;
    }
    return r;
}

LKD_SYSCALL(poll)
{
    int timeout = LKD_ARGI(2);
    int64_t deadline = timeout < 0 ? -1 : net_deadline_from_ms(timeout);
    int64_t r = do_poll(t, LKD_ARGP(0), LKD_ARGU(1), deadline);
    if (r == -LX_ERESTARTNOHAND && timeout >= 0) {
        /* sys_poll: a finite timeout restarts through restart_syscall with the same end time */
        t->restart.fn = poll_restart;
        t->restart.arg[0] = LKD_ARGP(0);
        t->restart.arg[1] = LKD_ARGU(1);
        t->restart.arg[2] = (uint64_t)deadline;
        r = -LX_ERESTART_RESTARTBLOCK;
    }
    return r;
}

/* poll_select_finish: write the remaining time back (PT_TIMESPEC / PT_TIMEVAL), and turn
 * -ERESTARTNOHAND into -EINTR when the write-back fails. A zero timeout is not updated. */
static int64_t finish_timeout(uint64_t gva, int64_t deadline, int is_timeval, int64_t ret)
{
    if (!gva || deadline < 0) return ret;
    int64_t left = deadline - lkd_host_mono_ns();
    if (left < 0) left = 0;
    struct lx_timespec ts = { left / 1000000000, left % 1000000000 };
    int bad;
    if (is_timeval) {
        int64_t tv[2] = { ts.tv_sec, ts.tv_nsec / 1000 };
        bad = lkd_user_write(gva, tv, sizeof tv) < 0;
    } else {
        bad = lkd_user_write(gva, &ts, sizeof ts) < 0;
    }
    if (bad && ret == -LX_ERESTARTNOHAND) ret = -LX_EINTR;
    return ret;
}

LKD_SYSCALL(ppoll)
{
    struct lx_timespec ts;
    int64_t deadline = -1;
    uint64_t tsp = LKD_ARGP(2);
    int zero = 0;
    if (tsp) {
        if (lkd_user_read(tsp, &ts, sizeof ts) < 0) return -LX_EFAULT;
        if (!net_ts_valid(&ts)) return -LX_EINVAL;
        deadline = net_deadline_from_ts(&ts);
        zero = ts.tv_sec == 0 && ts.tv_nsec == 0;
        if (zero) deadline = 0;
    }
    int m = net_install_sigmask(t, LKD_ARGP(3), LKD_ARGU(4));
    if (m < 0) return m;
    int64_t r = do_poll(t, LKD_ARGP(0), LKD_ARGU(1), deadline);
    if (!zero) r = finish_timeout(tsp, deadline, 0, r);
    net_finish_temp_mask(t, r);
    return r;
}

/* ---- select / pselect6 ------------------------------------------------------------------------- */

#define FDS_BYTES(n) ((((n) + 63) / 64) * 8)

/* core_sys_select + do_select: n is the highest fd + 1; sets are bitmaps of 64-bit words. */
static int64_t do_select(lkd_task *t, int n, uint64_t inp, uint64_t outp, uint64_t exp, int64_t deadline)
{
    if (n < 0) return -LX_EINVAL;
    unsigned lim = nofile_limit(t);
    if ((unsigned)n > lim) n = (int)lim; /* max_fds clamp: Linux uses the fdtable size */
    size_t nb = FDS_BYTES((size_t)n);
    uint64_t *in = calloc(1, nb + 8), *out = calloc(1, nb + 8), *ex = calloc(1, nb + 8);
    uint64_t *rin = calloc(1, nb + 8), *rout = calloc(1, nb + 8), *rex = calloc(1, nb + 8);
    struct pent *pe = calloc((size_t)n + 1, sizeof *pe);
    struct pollfd *hp = calloc((size_t)n + 1, sizeof *hp);
    int64_t ret = 0;
    if (!in || !out || !ex || !rin || !rout || !rex || !pe || !hp) { ret = -LX_ENOMEM; goto done; }
    if ((inp && lkd_user_read(inp, in, nb) < 0) || (outp && lkd_user_read(outp, out, nb) < 0) ||
        (exp && lkd_user_read(exp, ex, nb) < 0)) { ret = -LX_EFAULT; goto done; }
    unsigned nh = 0;
    for (int fd = 0; fd < n; fd++) {
        uint64_t bit = 1ull << (fd % 64);
        int w = fd / 64;
        pe[fd].widx = -1;
        if (!((in[w] | out[w] | ex[w]) & bit)) continue;
        pe[fd].o = lkd_fd_get(t, fd, NULL);
        if (!pe[fd].o) { ret = -LX_EBADF; goto done; } /* do_select: max_select_fd -EBADF */
        uint32_t mask = LX_POLLERR | LX_POLLHUP;
        if (in[w] & bit) mask |= LX_POLLIN | LX_POLLRDNORM | LX_POLLRDBAND | LX_POLLHUP;
        if (out[w] & bit) mask |= LX_POLLOUT | LX_POLLWRNORM | LX_POLLWRBAND;
        if (ex[w] & bit) mask |= LX_POLLPRI;
        pe[fd].mask = mask;
        int hfd;
        short hev;
        if (net_obj_waitsrc(pe[fd].o, mask, &hfd, &hev)) {
            hp[nh].fd = hfd;
            hp[nh].events = hev;
            pe[fd].widx = (int)nh++;
        }
    }
    if (nh && poll(hp, nh, 0) < 0) { ret = LKD_HOST_ERR(); goto done; }
    for (;;) {
        int count = 0;
        memset(rin, 0, nb); memset(rout, 0, nb); memset(rex, 0, nb);
        for (int fd = 0; fd < n; fd++) {
            if (!pe[fd].o) continue;
            uint64_t bit = 1ull << (fd % 64);
            int w = fd / 64;
            short h = pe[fd].widx >= 0 ? hp[pe[fd].widx].revents : 0;
            uint32_t r = net_obj_revents(pe[fd].o, pe[fd].mask, h, pe[fd].widx >= 0);
            /* fs/select.c POLLIN_SET, POLLOUT_SET, POLLEX_SET */
            if ((in[w] & bit) && (r & (LX_POLLRDNORM | LX_POLLRDBAND | LX_POLLIN | LX_POLLHUP | LX_POLLERR))) { rin[w] |= bit; count++; }
            if ((out[w] & bit) && (r & (LX_POLLWRBAND | LX_POLLWRNORM | LX_POLLOUT | LX_POLLERR))) { rout[w] |= bit; count++; }
            if ((ex[w] & bit) && (r & LX_POLLPRI)) { rex[w] |= bit; count++; }
        }
        if (count) { ret = count; break; }
        if (deadline == 0 || (deadline > 0 && lkd_host_mono_ns() >= deadline)) { ret = 0; break; }
        for (unsigned k = 0; k < nh; k++) hp[k].revents = 0;
        int w = net_block_poll(t, hp, nh, deadline);
        if (w == -LX_ERESTARTSYS) { ret = -LX_ERESTARTNOHAND; break; }
        if (w < 0) { ret = w; break; }
        if (w == 0) {
            if (nh && poll(hp, nh, 0) < 0) { ret = LKD_HOST_ERR(); break; }
            deadline = 0;
        }
    }
    if (ret >= 0) {
        if ((inp && lkd_user_write(inp, rin, nb) < 0) || (outp && lkd_user_write(outp, rout, nb) < 0) ||
            (exp && lkd_user_write(exp, rex, nb) < 0)) ret = -LX_EFAULT;
    }
done:
    if (pe) for (int fd = 0; fd < n; fd++) if (pe[fd].o) lkd_fdobj_put(pe[fd].o);
    free(in); free(out); free(ex); free(rin); free(rout); free(rex); free(pe); free(hp);
    return ret;
}

LKD_SYSCALL(select)
{
    uint64_t tvp = LKD_ARGP(4);
    int64_t deadline = -1;
    int zero = 0;
    if (tvp) {
        int64_t tv[2];
        if (lkd_user_read(tvp, tv, sizeof tv) < 0) return -LX_EFAULT;
        /* kern_select: poll_select_set_timeout rejects negative values and usec >= 1000000 */
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) return -LX_EINVAL;
        struct lx_timespec ts = { tv[0], tv[1] * 1000 };
        deadline = net_deadline_from_ts(&ts);
        zero = tv[0] == 0 && tv[1] == 0;
        if (zero) deadline = 0;
    }
    int64_t r = do_select(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGP(3), deadline);
    if (!zero) r = finish_timeout(tvp, deadline, 1, r);
    return r;
}

LKD_SYSCALL(pselect6)
{
    uint64_t tsp = LKD_ARGP(4);
    int64_t deadline = -1;
    int zero = 0;
    if (tsp) {
        struct lx_timespec ts;
        if (lkd_user_read(tsp, &ts, sizeof ts) < 0) return -LX_EFAULT;
        if (!net_ts_valid(&ts)) return -LX_EINVAL;
        deadline = net_deadline_from_ts(&ts);
        zero = ts.tv_sec == 0 && ts.tv_nsec == 0;
        if (zero) deadline = 0;
    }
    /* sixth argument: struct { const sigset_t *ss; size_t ss_len; } */
    uint64_t sig = LKD_ARGP(5);
    if (sig) {
        uint64_t pair[2];
        if (lkd_user_read(sig, pair, sizeof pair) < 0) return -LX_EFAULT;
        int m = net_install_sigmask(t, pair[0], pair[1]);
        if (m < 0) return m;
    }
    int64_t r = do_select(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGP(3), deadline);
    if (!zero) r = finish_timeout(tsp, deadline, 0, r);
    net_finish_temp_mask(t, r);
    return r;
}

void net_register_poll(void)
{
    LKD_REGISTER(poll);
    LKD_REGISTER(ppoll);
    LKD_REGISTER(select);
    LKD_REGISTER(pselect6);
}
