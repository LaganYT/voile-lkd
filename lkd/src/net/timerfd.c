/* timerfd.c: timerfd_create, timerfd_settime, timerfd_gettime. Models fs/timerfd.c of Linux 6.12
 * (timerfd_read, timerfd_poll, do_timerfd_settime, do_timerfd_gettime, timerfd_setup).
 *
 * The expiration count is computed from the clock (next expiry and interval in the timer's clock), so
 * it is exact whatever the wake-up latency. A private kqueue with one EVFILT_TIMER (one shot, armed at the
 * next expiry) is the wait source: its descriptor is readable once the timer fired, which is what poll,
 * select and epoll wait on. After a read the timer is re-armed at the next expiry of a periodic timer.
 *
 * Clocks (same host clocks as the task module): REALTIME, MONOTONIC and BOOTTIME. The _ALARM variants
 * need CAP_WAKE_ALARM (-EPERM). TFD_TIMER_CANCEL_ON_SET is accepted only with ABSTIME on REALTIME and
 * is not acted upon: LKD does not notice a clock step (documented gap), so ECANCELED never occurs.
 * State is per process: a fork child gets its own copy, rebuilt on a new kqueue (kqueue is not inherited).
 */
#include <fcntl.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>

#include "net_priv.h"

struct tfd {
    int clk;                        /* Linux clock id */
    int kq;                         /* private kqueue, also o->host_fd */
    int64_t next;                   /* next expiry in the clock, ns; 0 = disarmed */
    int64_t interval;               /* ns, 0 = one shot */
    uint64_t ticks_missed;          /* expirations already accounted by earlier reads/settime (unused) */
    struct net_kqobj kq_reg;
    int absolute_realtime;          /* armed in ABSTIME on REALTIME (cancel-on-set candidate) */
};

int net_clock_valid_timerfd(int clk)
{
    return clk == LX_CLOCK_REALTIME || clk == LX_CLOCK_MONOTONIC || clk == LX_CLOCK_BOOTTIME;
}

int64_t net_clock_now(int clk)
{
    clockid_t h = clk == LX_CLOCK_REALTIME ? CLOCK_REALTIME : clk == LX_CLOCK_MONOTONIC ? CLOCK_UPTIME_RAW : CLOCK_MONOTONIC_RAW;
    return (int64_t)clock_gettime_nsec_np(h);
}

static struct tfd *TFD(lkd_fdobj *o) { return o && o->ops == &net_timerfd_ops ? o->priv : NULL; }

/* Arm the kqueue timer to fire at the next expiry (o->lock held). */
static void tfd_arm_host(struct tfd *d)
{
    struct kevent kev;
    /* delete first: a fired but unread one-shot knote must not make the descriptor readable again */
    EV_SET(&kev, 1, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
    kevent(d->kq, &kev, 1, NULL, 0, NULL);
    if (!d->next) return;
    int64_t left = d->next - net_clock_now(d->clk);
    if (left < 1) left = 1;
    EV_SET(&kev, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS, left, 0);
    kevent(d->kq, &kev, 1, NULL, 0, NULL);
}

static void tfd_drain_host(struct tfd *d)
{
    struct kevent out;
    struct timespec z = { 0, 0 };
    while (kevent(d->kq, NULL, 0, &out, 1, &z) > 0) { }
}

/* Expirations due now (without consuming). */
static uint64_t tfd_due(struct tfd *d, int64_t now)
{
    if (!d->next || now < d->next) return 0;
    if (!d->interval) return 1;
    return 1 + (uint64_t)((now - d->next) / d->interval);
}

static uint32_t tfd_poll(lkd_fdobj *o, uint32_t want)
{
    (void)want;
    struct tfd *d = o->priv;
    pthread_mutex_lock(&o->lock);
    uint64_t due = tfd_due(d, net_clock_now(d->clk));
    pthread_mutex_unlock(&o->lock);
    return due ? (LX_POLLIN | LX_POLLRDNORM) : 0;
}

static int nonblocking(lkd_fdobj *o) { return (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0; }

static int64_t tfd_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    if (count < 8) return -LX_EINVAL;
    struct tfd *d = o->priv;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        int64_t now = net_clock_now(d->clk);
        uint64_t due = tfd_due(d, now);
        if (due) {
            if (d->interval) d->next += (int64_t)due * d->interval;
            else d->next = 0;
            tfd_drain_host(d);
            tfd_arm_host(d);
            pthread_mutex_unlock(&o->lock);
            return lkd_user_write(gva, &due, 8) < 0 ? -LX_EFAULT : 8;
        }
        pthread_mutex_unlock(&o->lock);
        if (nonblocking(o)) return -LX_EAGAIN;
        int w = lkd_block_wait_fd(t, d->kq, POLLIN, -1);
        if (w < 0) return w;
        if (!d->next) {
            /* disarmed while waiting: wait for settime to arm it again (timerfd_read keeps sleeping) */
            int s = lkd_block_sleep(t, lkd_host_mono_ns() + 5000000);
            if (s < 0) return s;
        }
    }
}

static int tfd_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    (void)o;
    int n = snprintf(buf, cap, "anon_inode:[timerfd]");
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static int tfd_fdinfo(lkd_fdobj *o, char *buf, size_t cap)
{
    struct tfd *d = o->priv;
    pthread_mutex_lock(&o->lock);
    int64_t now = net_clock_now(d->clk);
    int64_t left = d->next ? (d->next > now ? d->next - now : 0) : 0;
    uint64_t ticks = tfd_due(d, now);
    int n = snprintf(buf, cap, "clockid: %d\nticks: %llu\nsettime flags: 0%o\nit_value: (%lld, %lld)\nit_interval: (%lld, %lld)\n",
                     d->clk, (unsigned long long)ticks, d->absolute_realtime ? 1 : 0, (long long)(left / 1000000000),
                     (long long)(left % 1000000000), (long long)(d->interval / 1000000000), (long long)(d->interval % 1000000000));
    pthread_mutex_unlock(&o->lock);
    return n < (int)cap ? n : (int)cap - 1;
}

static int tfd_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out) { (void)t; return net_anon_fstat(o, out); }

static int64_t tfd_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)arg;
    return cmd == LX_F_SETFL ? 0 : -LX_EINVAL;
}

static void tfd_release(lkd_fdobj *o)
{
    struct tfd *d = o->priv;
    if (!d) return;
    net_kqobj_del(&d->kq_reg);
    o->host_fd = -1;                /* closed here: lkd_fdobj_put would close the (possibly rebuilt) fd too */
    if (d->kq >= 0) close(d->kq);
    free(d);
    o->priv = NULL;
}

const lkd_fd_ops net_timerfd_ops = {
    .name = "timerfd",
    .read = tfd_read,
    .fcntl = tfd_fcntl,
    .poll = tfd_poll,
    .fstat = tfd_fstat,
    .describe = tfd_describe,
    .fdinfo = tfd_fdinfo,
    .release = tfd_release,
};

/* Fork child: a new kqueue, re-armed at the same absolute expiry (order 0: before epoll sets). */
static void tfd_rebuild(lkd_fdobj *o)
{
    struct tfd *d = o->priv;
    int kq = kqueue();
    if (kq < 0) return;
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    d->kq = kq;
    o->host_fd = kq;
    tfd_arm_host(d);
}

LKD_SYSCALL(timerfd_create)
{
    int clk = LKD_ARGI(0), flags = LKD_ARGI(1);
    if (flags & ~(LX_TFD_CLOEXEC | LX_TFD_NONBLOCK)) return -LX_EINVAL;
    if (clk == LX_CLOCK_REALTIME_ALARM || clk == LX_CLOCK_BOOTTIME_ALARM) return -LX_EPERM;   /* CAP_WAKE_ALARM */
    if (!net_clock_valid_timerfd(clk)) return -LX_EINVAL;
    struct tfd *d = calloc(1, sizeof *d);
    if (!d) return -LX_ENOMEM;
    d->clk = clk;
    d->kq = kqueue();
    if (d->kq < 0) { free(d); return LKD_HOST_ERR(); }
    fcntl(d->kq, F_SETFD, FD_CLOEXEC);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_TIMERFD, &net_timerfd_ops, d->kq, LX_O_RDWR | (flags & LX_TFD_NONBLOCK), d);
    if (!o) { close(d->kq); free(d); return -LX_ENOMEM; }
    d->kq_reg.o = o;
    d->kq_reg.order = 0;
    d->kq_reg.rebuild = tfd_rebuild;
    net_kqobj_add(&d->kq_reg);
    return net_install(t, o, (flags & LX_TFD_CLOEXEC) != 0);
}

static void put_its(struct tfd *d, int64_t now, struct lx_itimerspec *its)
{
    int64_t left = d->next ? (d->next > now ? d->next - now : 0) : 0;
    /* timerfd_get_remaining: an armed timer that already expired reports the time to the next tick */
    if (d->next && d->next <= now && d->interval) {
        int64_t over = (now - d->next) % d->interval;
        left = d->interval - over;
    }
    its->it_value.tv_sec = left / 1000000000;
    its->it_value.tv_nsec = left % 1000000000;
    its->it_interval.tv_sec = d->interval / 1000000000;
    its->it_interval.tv_nsec = d->interval % 1000000000;
}

static int ts_ok(const struct lx_timespec *ts) { return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000; }
static int64_t ts_ns(const struct lx_timespec *ts)
{
    if (ts->tv_sec > INT64_MAX / 1000000000 - 2) return INT64_MAX / 2;
    return ts->tv_sec * 1000000000 + ts->tv_nsec;
}

LKD_SYSCALL(timerfd_settime)
{
    int fd = LKD_ARGI(0), flags = LKD_ARGI(1);
    uint64_t newp = LKD_ARGP(2), oldp = LKD_ARGP(3);
    if (flags & ~(LX_TFD_TIMER_ABSTIME | LX_TFD_TIMER_CANCEL_ON_SET)) return -LX_EINVAL;
    struct lx_itimerspec n;
    if (lkd_user_read(newp, &n, sizeof n) < 0) return -LX_EFAULT;
    if (!ts_ok(&n.it_value) || !ts_ok(&n.it_interval)) return -LX_EINVAL;
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    struct tfd *d = TFD(o);
    if (!d) { lkd_fdobj_put(o); return -LX_EINVAL; }
    if ((flags & LX_TFD_TIMER_CANCEL_ON_SET) && (!(flags & LX_TFD_TIMER_ABSTIME) || d->clk != LX_CLOCK_REALTIME)) {
        lkd_fdobj_put(o);
        return -LX_EINVAL;
    }
    pthread_mutex_lock(&o->lock);
    int64_t now = net_clock_now(d->clk);
    struct lx_itimerspec old;
    put_its(d, now, &old);
    int64_t value = ts_ns(&n.it_value), interval = ts_ns(&n.it_interval);
    if (value == 0) {
        d->next = 0;                              /* disarm */
        d->interval = 0;
    } else {
        d->next = (flags & LX_TFD_TIMER_ABSTIME) ? value : now + value;
        if (d->next <= 0) d->next = 1;            /* an absolute time in the past fires at once */
        d->interval = interval;
    }
    d->absolute_realtime = (flags & LX_TFD_TIMER_ABSTIME) && d->clk == LX_CLOCK_REALTIME;
    tfd_drain_host(d);
    tfd_arm_host(d);
    pthread_mutex_unlock(&o->lock);
    lkd_fdobj_put(o);
    if (oldp && lkd_user_write(oldp, &old, sizeof old) < 0) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(timerfd_gettime)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    struct tfd *d = TFD(o);
    if (!d) { lkd_fdobj_put(o); return -LX_EINVAL; }
    struct lx_itimerspec its;
    pthread_mutex_lock(&o->lock);
    put_its(d, net_clock_now(d->clk), &its);
    pthread_mutex_unlock(&o->lock);
    lkd_fdobj_put(o);
    return lkd_user_write(LKD_ARGP(1), &its, sizeof its) < 0 ? -LX_EFAULT : 0;
}

void net_register_timerfd(void)
{
    LKD_REGISTER(timerfd_create);
    LKD_REGISTER(timerfd_settime);
    LKD_REGISTER(timerfd_gettime);
}
