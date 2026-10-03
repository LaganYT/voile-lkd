/* test_net_events.c: poll/ppoll/select/pselect6, eventfd, timerfd, inotify. Linux references: poll(2),
 * select(2), eventfd(2), timerfd_create(2), inotify(7), fs/select.c, fs/eventfd.c, fs/timerfd.c,
 * fs/notify/inotify of v6.12. */
#include <pthread.h>
#include <sys/wait.h>

#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

static void clear_signals(lkd_task *t)
{
    atomic_fetch_and(&t->flags, ~(uint32_t)(LKD_TF_SIGPENDING | LKD_TF_FATAL));
    char b[64];
    while (read(t->wake_rd, b, sizeof b) > 0) { }
}

/* ---- poll / select ------------------------------------------------------------------------------ */

LT_TEST(poll_basics)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    struct lx_pollfd pf[3] = { { p[0], LX_POLLIN, 0x7fff }, { -1, LX_POLLIN, 0x7fff }, { 999, LX_POLLIN, 0 } };
    /* a negative fd is skipped and its revents is zeroed; a closed fd gives POLLNVAL */
    LT_EQ(SC(T, poll, P(pf), 3, 0), 1);
    LT_EQ(pf[0].revents, 0);
    LT_EQ(pf[1].revents, 0);
    LT_EQ(pf[2].revents, LX_POLLNVAL);
    SC(T, write, p[1], P("x"), 1);
    pf[2].fd = -1;
    LT_EQ(SC(T, poll, P(pf), 3, 100), 1);
    LT_EQ(pf[0].revents, LX_POLLIN);
    /* the timeout is honoured */
    char c;
    SC(T, read, p[0], P(&c), 1);
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, poll, P(pf), 1, 50), 0);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 45000000 && dt < 2000000000);
    /* closing the write end gives POLLHUP on the read end even when it was not asked */
    SC(T, close, p[1]);
    struct lx_pollfd one = { p[0], 0, 0 };
    LT_EQ(SC(T, poll, P(&one), 1, 100), 1);
    LT_EQ(one.revents, LX_POLLHUP);
    /* nfds above RLIMIT_NOFILE is EINVAL; a bad array pointer is EFAULT */
    LT_ERRNO(SC(T, poll, P(&one), 100000000ull, 0), LX_EINVAL);
    LT_ERRNO(SC(T, poll, 0x10, 1, 0), LX_EFAULT);
    SC(T, close, p[0]);
}

LT_TEST(ppoll_timespec_and_mask)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    struct lx_pollfd pf = { p[0], LX_POLLIN, 0 };
    struct lx_timespec ts = { 0, 30000000 };
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, ppoll, P(&pf), 1, P(&ts), 0, 0), 0);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 25000000);
    /* ppoll(2): the timespec is updated with the time left when the call returns early */
    SC(T, write, p[1], P("x"), 1);
    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    LT_EQ(SC(T, ppoll, P(&pf), 1, P(&ts), 0, 0), 1);
    LT_EQ(ts.tv_sec, 4);
    LT_CHECK(ts.tv_nsec > 0);
    /* a negative timespec is EINVAL; a zero timespec polls once */
    struct lx_timespec neg = { -1, 0 }, zero = { 0, 0 };
    LT_ERRNO(SC(T, ppoll, P(&pf), 1, P(&neg), 0, 0), LX_EINVAL);
    LT_EQ(SC(T, ppoll, P(&pf), 1, P(&zero), 0, 0), 1);
    uint64_t m = LX_SIGBIT(LX_SIGUSR1);
    uint64_t before = atomic_load(&T->sigmask);
    LT_EQ(SC(T, ppoll, P(&pf), 1, P(&zero), P(&m), 8), 1);
    LT_EQ(atomic_load(&T->sigmask), before);                       /* restored after the call */
    LT_ERRNO(SC(T, ppoll, P(&pf), 1, P(&zero), P(&m), 4), LX_EINVAL);
    SC(T, close, p[0]); SC(T, close, p[1]);
}

LT_TEST(select_basics)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    uint64_t rd = 1ull << p[0], wr = 1ull << p[1];
    struct { int64_t s, us; } tv = { 0, 0 };
    /* empty pipe: write end writable, read end not */
    uint64_t r2 = rd, w2 = wr | rd;
    LT_EQ(SC(T, select, p[1] + 1, P(&r2), P(&w2), 0, P(&tv)), 1);
    LT_EQ(r2, 0);
    LT_EQ(w2, wr);
    SC(T, write, p[1], P("z"), 1);
    r2 = rd;
    LT_EQ(SC(T, select, p[1] + 1, P(&r2), 0, 0, P(&tv)), 1);
    LT_EQ(r2, rd);
    /* a closed descriptor in a set is EBADF, a negative nfds is EINVAL */
    uint64_t bad = 1ull << 40;
    LT_ERRNO(SC(T, select, 41, P(&bad), 0, 0, P(&tv)), LX_EBADF);
    LT_ERRNO(SC(T, select, -1, 0, 0, 0, P(&tv)), LX_EINVAL);
    /* select(2): the timeout is updated on Linux */
    char c;
    SC(T, read, p[0], P(&c), 1);
    struct { int64_t s, us; } t2 = { 0, 40000 };
    r2 = rd;
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, select, p[0] + 1, P(&r2), 0, 0, P(&t2)), 0);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 35000000);
    LT_EQ(r2, 0);                                                   /* sets are cleared on timeout */
    LT_EQ(t2.s, 0);
    LT_EQ(t2.us, 0);
    /* pselect6 takes a timespec and the mask pair */
    struct lx_timespec ts = { 0, 20000000 };
    r2 = rd;
    uint64_t m = LX_SIGBIT(LX_SIGUSR2);
    uint64_t sig[2] = { P(&m), 8 };
    LT_EQ(SC(T, pselect6, p[0] + 1, P(&r2), 0, 0, P(&ts), P(sig)), 0);
    SC(T, close, p[0]); SC(T, close, p[1]);
}

/* A blocking poll is woken by data written from another thread and ended by an interruption. */
struct wr { lkd_task *t; int fd; int ms; };
static void *delayed_write(void *a)
{
    struct wr *w = a;
    lkd_set_current(w->t);
    usleep((useconds_t)w->ms * 1000);
    SC(w->t, write, w->fd, P("w"), 1);
    return NULL;
}

LT_TEST(blocking_poll_wake_and_interrupt)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    struct wr w = { th, p[1], 50 };
    pthread_t pt;
    pthread_create(&pt, NULL, delayed_write, &w);
    struct lx_pollfd pf = { p[0], LX_POLLIN, 0 };
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, poll, P(&pf), 1, -1), 1);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 30000000);
    pthread_join(pt, NULL);
    char c;
    SC(T, read, p[0], P(&c), 1);
    clear_signals(T);
    atomic_fetch_or(&T->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(T);
    lkd_sysreq rq = { .nr = LKD_NR_poll, .a = { P(&pf), 1, (uint64_t)-1 } };
    int64_t r = lkd_dispatch(T, &rq);
    /* poll(2): -EINTR to the guest; inside LKD the infinite form is ERESTARTNOHAND */
    LT_CHECK(r == -LX_ERESTARTNOHAND || r == -LX_EINTR);
    clear_signals(T);
    SC(T, close, p[0]); SC(T, close, p[1]);
}

/* ---- eventfd ------------------------------------------------------------------------------------ */

LT_TEST(eventfd_semantics)
{
    LT_ERRNO(SC(T, eventfd2, 0, 0x7), LX_EINVAL);
    int fd = (int)SC(T, eventfd2, 3, LX_EFD_NONBLOCK | LX_EFD_CLOEXEC);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFD), LX_FD_CLOEXEC);
    uint64_t v = 0;
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 3);                                                    /* read returns the counter and resets it */
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);
    LT_ERRNO(SC(T, read, fd, P(&v), 4), LX_EINVAL);                 /* buffer smaller than 8 bytes */
    v = 5;
    LT_EQ(SC(T, write, fd, P(&v), 8), 8);
    v = 7;
    LT_EQ(SC(T, write, fd, P(&v), 8), 8);
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 12);
    v = UINT64_MAX;
    LT_ERRNO(SC(T, write, fd, P(&v), 8), LX_EINVAL);                /* 0xffffffffffffffff is invalid */
    v = UINT64_MAX - 1;
    LT_EQ(SC(T, write, fd, P(&v), 8), 8);
    v = 1;
    LT_ERRNO(SC(T, write, fd, P(&v), 8), LX_EAGAIN);                /* would overflow */
    struct lx_pollfd pf = { fd, LX_POLLIN | LX_POLLOUT, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_EQ(pf.revents, LX_POLLIN);                                   /* counter at max: not writable */
    SC(T, read, fd, P(&v), 8);
    /* fdinfo-style identity */
    struct lx_stat st;
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, 0);                               /* anon_inode: no file type bits */
    SC(T, close, fd);
}

LT_TEST(eventfd_semaphore)
{
    int fd = (int)SC(T, eventfd2, 3, LX_EFD_SEMAPHORE | LX_EFD_NONBLOCK);
    uint64_t v = 0;
    for (int i = 0; i < 3; i++) {
        LT_EQ(SC(T, read, fd, P(&v), 8), 8);
        LT_EQ(v, 1);                                                /* EFD_SEMAPHORE: decrements by one */
    }
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);
    SC(T, close, fd);
}

struct efdw { lkd_task *t; int fd; };
static void *efd_writer(void *a)
{
    struct efdw *x = a;
    lkd_set_current(x->t);
    usleep(50000);
    uint64_t v = 9;
    SC(x->t, write, x->fd, P(&v), 8);
    return NULL;
}

LT_TEST(eventfd_blocking_read)
{
    int fd = (int)SC(T, eventfd2, 0, 0);
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    struct efdw ww = { th, fd };
    pthread_t pt;
    pthread_create(&pt, NULL, efd_writer, &ww);
    uint64_t v = 0;
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 9);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 30000000);
    pthread_join(pt, NULL);
    SC(T, close, fd);
}

/* The counter is shared across fork (Wine's synchronization objects): a child write is seen by the
 * parent and the parent's write by the child. */
LT_TEST(eventfd_across_fork)
{
    int fd = (int)SC(T, eventfd2, 0, LX_EFD_NONBLOCK);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    fflush(NULL);
    pid_t pid = lkd_fork_host(T, 0);
    if (pid == 0) {
        alarm(10);
        uint64_t v = 40;
        if (SC(T, write, fd, P(&v), 8) != 8) _exit(1);
        char c;
        if (SC(T, read, p[0], P(&c), 1) != 1) _exit(2);              /* wait for the parent */
        v = 0;
        if (SC(T, read, fd, P(&v), 8) != 8 || v != 2) _exit(3);      /* the parent's 2, the 40 was consumed */
        _exit(0);
    }
    LT_CHECK(pid > 0);
    /* wait until the child's 40 shows up */
    struct lx_pollfd pf = { fd, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 5000), 1);
    uint64_t v = 0;
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 40);
    v = 2;
    LT_EQ(SC(T, write, fd, P(&v), 8), 8);
    LT_EQ(SC(T, write, p[1], P("g"), 1), 1);
    int st = 0;
    waitpid(pid, &st, 0);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st), 0);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, fd);
}

/* ---- timerfd ------------------------------------------------------------------------------------ */

LT_TEST(timerfd_create_errors)
{
    LT_ERRNO(SC(T, timerfd_create, 99, 0), LX_EINVAL);
    LT_ERRNO(SC(T, timerfd_create, LX_CLOCK_MONOTONIC, 0x7), LX_EINVAL);
    LT_ERRNO(SC(T, timerfd_create, LX_CLOCK_REALTIME_ALARM, 0), LX_EPERM);     /* CAP_WAKE_ALARM */
    LT_ERRNO(SC(T, timerfd_create, LX_CLOCK_PROCESS_CPUTIME_ID, 0), LX_EINVAL);
    int fd = (int)SC(T, timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK | LX_TFD_CLOEXEC);
    LT_CHECK(fd >= 0);
    struct lx_itimerspec its;
    LT_EQ(SC(T, timerfd_gettime, fd, P(&its)), 0);
    LT_EQ(its.it_value.tv_sec, 0);
    LT_EQ(its.it_value.tv_nsec, 0);
    struct lx_itimerspec bad = { { 0, 0 }, { 0, 2000000000 } };
    LT_ERRNO(SC(T, timerfd_settime, fd, 0, P(&bad), 0), LX_EINVAL);
    LT_ERRNO(SC(T, timerfd_settime, fd, 0x10, P(&its), 0), LX_EINVAL);
    LT_ERRNO(SC(T, timerfd_settime, fd, LX_TFD_TIMER_CANCEL_ON_SET, P(&its), 0), LX_EINVAL);   /* needs ABSTIME and REALTIME */
    int32_t p[2];
    SC(T, pipe, P(p));
    LT_ERRNO(SC(T, timerfd_gettime, p[0], P(&its)), LX_EINVAL);
    LT_ERRNO(SC(T, timerfd_settime, 999, 0, P(&its), 0), LX_EBADF);
    uint64_t v;
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);                 /* disarmed */
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, fd);
}

LT_TEST(timerfd_oneshot_and_periodic)
{
    int fd = (int)SC(T, timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
    struct lx_itimerspec its = { { 0, 0 }, { 0, 40000000 } }, old;
    LT_EQ(SC(T, timerfd_settime, fd, 0, P(&its), P(&old)), 0);
    LT_EQ(old.it_value.tv_nsec, 0);
    uint64_t v = 0;
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);
    struct lx_itimerspec cur;
    LT_EQ(SC(T, timerfd_gettime, fd, P(&cur)), 0);
    LT_CHECK(cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 40000000);
    struct lx_pollfd pf = { fd, LX_POLLIN, 0 };
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, poll, P(&pf), 1, 2000), 1);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 35000000);
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 1);
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);                 /* one shot: disarmed after the tick */
    /* periodic: it_interval 20 ms, first expiry 20 ms; sleeping 110 ms accumulates several ticks */
    struct lx_itimerspec per = { { 0, 20000000 }, { 0, 20000000 } };
    LT_EQ(SC(T, timerfd_settime, fd, 0, P(&per), 0), 0);
    usleep(110000);
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_CHECK(v >= 4 && v <= 6);                                     /* timerfd_read returns the expirations since the last read */
    LT_EQ(SC(T, timerfd_gettime, fd, P(&cur)), 0);
    LT_EQ(cur.it_interval.tv_nsec, 20000000);
    LT_CHECK(cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 20000000);
    /* settime returns the previous setting */
    struct lx_itimerspec off = { { 0, 0 }, { 0, 0 } };
    LT_EQ(SC(T, timerfd_settime, fd, 0, P(&off), P(&old)), 0);
    LT_EQ(old.it_interval.tv_nsec, 20000000);
    usleep(50000);
    LT_ERRNO(SC(T, read, fd, P(&v), 8), LX_EAGAIN);                 /* disarmed: nothing */
    SC(T, close, fd);
}

LT_TEST(timerfd_absolute_and_blocking)
{
    int fd = (int)SC(T, timerfd_create, LX_CLOCK_REALTIME, 0);
    struct lx_timespec now;
    LT_EQ(SC(T, clock_gettime, LX_CLOCK_REALTIME, P(&now)), 0);
    int64_t ns = now.tv_nsec + 40000000;
    struct lx_itimerspec abs = { { 0, 0 }, { now.tv_sec + ns / 1000000000, ns % 1000000000 } };
    LT_EQ(SC(T, timerfd_settime, fd, LX_TFD_TIMER_ABSTIME, P(&abs), 0), 0);
    uint64_t v = 0;
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);                            /* blocking read waits for the tick */
    LT_EQ(v, 1);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 25000000 && dt < 2000000000);
    /* an absolute time in the past fires at once */
    struct lx_itimerspec past = { { 0, 0 }, { 1, 0 } };
    LT_EQ(SC(T, timerfd_settime, fd, LX_TFD_TIMER_ABSTIME, P(&past), 0), 0);
    struct lx_pollfd pf = { fd, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 1000), 1);
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(v, 1);
    SC(T, close, fd);
}

LT_TEST(timerfd_with_epoll)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int fd = (int)SC(T, timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
    struct lx_epoll_event ev = { LX_EPOLLIN, 77 }, out[2];
    LT_EQ(SC(T, epoll_ctl, ep, LX_EPOLL_CTL_ADD, fd, P(&ev)), 0);
    struct lx_itimerspec its = { { 0, 0 }, { 0, 30000000 } };
    LT_EQ(SC(T, timerfd_settime, fd, 0, P(&its), 0), 0);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 0), 0);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 2000), 1);
    LT_EQ(out[0].data, 77);
    uint64_t v;
    LT_EQ(SC(T, read, fd, P(&v), 8), 8);
    LT_EQ(SC(T, epoll_wait, ep, P(out), 2, 0), 0);                  /* consumed: not ready any more */
    SC(T, close, fd); SC(T, close, ep);
}

LT_TEST(timerfd_after_fork)
{
    int fd = (int)SC(T, timerfd_create, LX_CLOCK_MONOTONIC, LX_TFD_NONBLOCK);
    struct lx_itimerspec its = { { 0, 0 }, { 0, 60000000 } };
    LT_EQ(SC(T, timerfd_settime, fd, 0, P(&its), 0), 0);
    fflush(NULL);
    pid_t pid = lkd_fork_host(T, 0);
    if (pid == 0) {
        alarm(10);
        struct lx_pollfd pf = { fd, LX_POLLIN, 0 };
        if (SC(T, poll, P(&pf), 1, 2000) != 1) _exit(1);              /* the child's kqueue was rebuilt and re-armed */
        uint64_t v = 0;
        if (SC(T, read, fd, P(&v), 8) != 8 || v != 1) _exit(2);
        _exit(0);
    }
    LT_CHECK(pid > 0);
    int st = 0;
    waitpid(pid, &st, 0);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st), 0);
    struct lx_pollfd pf = { fd, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 1000), 1);                          /* the parent's timer is unaffected */
    SC(T, close, fd);
}

/* ---- inotify ------------------------------------------------------------------------------------ */

static int read_events(int fd, struct lx_inotify_event **evs, uint8_t *buf, size_t cap, int max)
{
    int64_t n = SC(T, read, fd, P(buf), cap);
    if (n <= 0) return (int)n;
    int k = 0;
    for (int64_t off = 0; off < n && k < max;) {
        struct lx_inotify_event *e = (struct lx_inotify_event *)(buf + off);
        evs[k++] = e;
        off += sizeof *e + e->len;
    }
    return k;
}

LT_TEST(inotify_errors_and_layout)
{
    LT_EQ(sizeof(struct lx_inotify_event), 16);
    LT_ERRNO(SC(T, inotify_init1, 0x7), LX_EINVAL);
    int in = (int)SC(T, inotify_init1, LX_IN_NONBLOCK | LX_IN_CLOEXEC);
    LT_CHECK(in >= 0);
    LT_EQ(SC(T, fcntl, in, LX_F_GETFD), LX_FD_CLOEXEC);
    uint8_t buf[256];
    LT_ERRNO(SC(T, read, in, P(buf), sizeof buf), LX_EAGAIN);
    LT_ERRNO(SC(T, inotify_add_watch, in, P("/w"), 0), LX_EINVAL);                       /* no event bit */
    LT_ERRNO(SC(T, inotify_add_watch, in, P("/w"), LX_IN_MODIFY | LX_IN_MASK_ADD | LX_IN_MASK_CREATE), LX_EINVAL);
    LT_ERRNO(SC(T, inotify_add_watch, in, P("/nonexistent"), LX_IN_MODIFY), LX_ENOENT);
    LT_ERRNO(SC(T, inotify_add_watch, in, P("/w/a.txt"), LX_IN_MODIFY | LX_IN_ONLYDIR), LX_ENOTDIR);
    LT_ERRNO(SC(T, inotify_add_watch, 999, P("/w"), LX_IN_MODIFY), LX_EBADF);
    int32_t p[2];
    SC(T, pipe, P(p));
    LT_ERRNO(SC(T, inotify_add_watch, p[0], P("/w"), LX_IN_MODIFY), LX_EINVAL);
    LT_ERRNO(SC(T, inotify_rm_watch, in, 77), LX_EINVAL);
    int wd = (int)SC(T, inotify_add_watch, in, P("/w"), LX_IN_CREATE);
    LT_EQ(wd, 1);
    LT_ERRNO(SC(T, inotify_add_watch, in, P("/w"), LX_IN_CREATE | LX_IN_MASK_CREATE), LX_EEXIST);
    LT_EQ(SC(T, inotify_add_watch, in, P("/w"), LX_IN_DELETE), wd);                      /* same object: same wd, mask replaced */
    LT_EQ(SC(T, inotify_rm_watch, in, wd), 0);
    /* removal queues IN_IGNORED */
    struct lx_inotify_event *evs[4];
    LT_EQ(read_events(in, evs, buf, sizeof buf, 4), 1);
    LT_EQ(evs[0]->wd, wd);
    LT_EQ(evs[0]->mask, LX_IN_IGNORED);
    LT_EQ(evs[0]->len, 0);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, in);
}

LT_TEST(inotify_directory_events)
{
    int in = (int)SC(T, inotify_init1, LX_IN_NONBLOCK);
    int wd = (int)SC(T, inotify_add_watch, in, P("/w"), LX_IN_CREATE | LX_IN_DELETE | LX_IN_MOVED_FROM | LX_IN_MOVED_TO | LX_IN_MODIFY | LX_IN_ATTRIB);
    LT_CHECK(wd > 0);
    uint8_t buf[1024];
    struct lx_inotify_event *evs[8];
    /* create a file through the guest syscall layer */
    int f = (int)SC(T, openat, AT_CWD, P("/w/new.txt"), LX_O_WRONLY | LX_O_CREAT, 0644);
    LT_CHECK(f >= 0);
    usleep(30000);
    int n = read_events(in, evs, buf, sizeof buf, 8);
    LT_CHECK(n >= 1);
    LT_EQ(evs[0]->wd, wd);
    LT_EQ(evs[0]->mask, LX_IN_CREATE);
    LT_STREQ(evs[0]->name, "new.txt");
    LT_EQ(evs[0]->len % 16, 0);                                       /* name padded to a multiple of 16, NUL included */
    LT_EQ(evs[0]->len, 16);
    /* modify the new file (a child watch is added by the rescan) */
    LT_EQ(SC(T, write, f, P("data"), 4), 4);
    usleep(30000);
    n = read_events(in, evs, buf, sizeof buf, 8);
    LT_CHECK(n >= 1);
    LT_EQ(evs[0]->mask, LX_IN_MODIFY);
    LT_STREQ(evs[0]->name, "new.txt");
    /* rename inside the directory: MOVED_FROM and MOVED_TO share a cookie */
    LT_EQ(SC(T, rename, P("/w/new.txt"), P("/w/moved.txt")), 0);
    usleep(30000);
    n = read_events(in, evs, buf, sizeof buf, 8);
    LT_EQ(n, 2);
    LT_EQ(evs[0]->mask, LX_IN_MOVED_FROM);
    LT_STREQ(evs[0]->name, "new.txt");
    LT_EQ(evs[1]->mask, LX_IN_MOVED_TO);
    LT_STREQ(evs[1]->name, "moved.txt");
    LT_CHECK(evs[0]->cookie != 0);
    LT_EQ(evs[0]->cookie, evs[1]->cookie);
    /* unlink */
    LT_EQ(SC(T, unlink, P("/w/moved.txt")), 0);
    usleep(30000);
    n = read_events(in, evs, buf, sizeof buf, 8);
    LT_CHECK(n >= 1);
    int saw_delete = 0;
    for (int i = 0; i < n; i++) if (evs[i]->mask == LX_IN_DELETE && !strcmp(evs[i]->name, "moved.txt")) saw_delete = 1;
    LT_EQ(saw_delete, 1);
    /* a subdirectory carries IN_ISDIR */
    LT_EQ(SC(T, mkdir, P("/w/sub"), 0755), 0);
    usleep(30000);
    n = read_events(in, evs, buf, sizeof buf, 8);
    LT_EQ(n, 1);
    LT_EQ(evs[0]->mask, LX_IN_CREATE | LX_IN_ISDIR);
    LT_STREQ(evs[0]->name, "sub");
    SC(T, rmdir, P("/w/sub"));
    SC(T, close, f); SC(T, close, in);
}

LT_TEST(inotify_file_events_and_poll)
{
    int in = (int)SC(T, inotify_init1, LX_IN_NONBLOCK);
    int wd = (int)SC(T, inotify_add_watch, in, P("/w/a.txt"), LX_IN_MODIFY | LX_IN_ATTRIB | LX_IN_DELETE_SELF | LX_IN_ONESHOT);
    LT_CHECK(wd > 0);
    struct lx_pollfd pf = { in, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 0);
    int f = (int)SC(T, openat, AT_CWD, P("/w/a.txt"), LX_O_WRONLY | LX_O_APPEND, 0);
    LT_EQ(SC(T, write, f, P("hello"), 5), 5);
    LT_EQ(SC(T, poll, P(&pf), 1, 2000), 1);
    LT_EQ(pf.revents, LX_POLLIN);
    int32_t avail = 0;
    LT_EQ(SC(T, ioctl, in, LX_FIONREAD, P(&avail)), 0);
    LT_EQ(avail, 32);                                                 /* IN_MODIFY and the IN_IGNORED of ONESHOT, 16 bytes each */
    uint8_t buf[128];
    struct lx_inotify_event *evs[4];
    int n = read_events(in, evs, buf, sizeof buf, 4);
    LT_CHECK(n >= 1);
    LT_EQ(evs[0]->wd, wd);
    LT_EQ(evs[0]->mask, LX_IN_MODIFY);
    LT_EQ(evs[0]->len, 0);
    /* IN_ONESHOT removes the watch after the first event: IN_IGNORED follows */
    LT_EQ(n, 2);
    LT_EQ(evs[1]->mask, LX_IN_IGNORED);
    LT_EQ(evs[1]->wd, wd);
    /* a buffer smaller than the first event is EINVAL */
    int wd2 = (int)SC(T, inotify_add_watch, in, P("/w/a.txt"), LX_IN_DELETE_SELF);
    LT_CHECK(wd2 > 0);
    SC(T, close, f);
    LT_EQ(SC(T, unlink, P("/w/a.txt")), 0);
    LT_EQ(SC(T, poll, P(&pf), 1, 2000), 1);
    LT_ERRNO(SC(T, read, in, P(buf), 8), LX_EINVAL);
    n = read_events(in, evs, buf, sizeof buf, 4);
    LT_EQ(n, 2);
    LT_EQ(evs[0]->mask, LX_IN_DELETE_SELF);
    LT_EQ(evs[1]->mask, LX_IN_IGNORED);
    SC(T, close, in);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netev");
    put_dir(ROOT, "w");
    put_file(ROOT, "w/a.txt", "x");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    LT_RUN(poll_basics);
    LT_RUN(ppoll_timespec_and_mask);
    LT_RUN(select_basics);
    LT_RUN(blocking_poll_wake_and_interrupt);
    LT_RUN(eventfd_semantics);
    LT_RUN(eventfd_semaphore);
    LT_RUN(eventfd_blocking_read);
    LT_RUN(eventfd_across_fork);
    LT_RUN(timerfd_create_errors);
    LT_RUN(timerfd_oneshot_and_periodic);
    LT_RUN(timerfd_absolute_and_blocking);
    LT_RUN(timerfd_with_epoll);
    LT_RUN(timerfd_after_fork);
    LT_RUN(inotify_errors_and_layout);
    LT_RUN(inotify_directory_events);
    LT_RUN(inotify_file_events_and_poll);
    rm_rf(ROOT);
    return lt_finish();
}
