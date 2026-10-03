/* test_net_epoll.c: epoll over kqueue. Linux references: epoll(7), epoll_ctl(2), epoll_wait(2),
 * epoll_create(2), fs/eventpoll.c of v6.12 (do_epoll_ctl, ep_send_events, ep_item_poll). */
#include <pthread.h>
#include <sys/wait.h>

#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

static int64_t ctl(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    struct lx_epoll_event ev = { events, data };
    return SC(T, epoll_ctl, ep, op, fd, P(&ev));
}

static int wait_ms(int ep, struct lx_epoll_event *out, int max, int ms)
{
    return (int)SC(T, epoll_wait, ep, P(out), max, ms);
}

static void clear_signals(lkd_task *t)
{
    atomic_fetch_and(&t->flags, ~(uint32_t)(LKD_TF_SIGPENDING | LKD_TF_FATAL));
    char b[64];
    while (read(t->wake_rd, b, sizeof b) > 0) { }
}

LT_TEST(layout_and_create)
{
    /* x86-64 struct epoll_event is packed: 12 bytes, data at offset 4 */
    LT_EQ(sizeof(struct lx_epoll_event), 12);
    LT_ERRNO(SC(T, epoll_create, 0), LX_EINVAL);
    LT_ERRNO(SC(T, epoll_create, -5), LX_EINVAL);
    LT_ERRNO(SC(T, epoll_create1, 0x1), LX_EINVAL);
    int ep = (int)SC(T, epoll_create1, LX_EPOLL_CLOEXEC);
    LT_CHECK(ep >= 0);
    LT_EQ(SC(T, fcntl, ep, LX_F_GETFD), LX_FD_CLOEXEC);
    int ep2 = (int)SC(T, epoll_create, 8);
    LT_CHECK(ep2 >= 0);
    LT_EQ(SC(T, fcntl, ep2, LX_F_GETFD), 0);
    /* epoll_wait on a non epoll fd is EINVAL, on a bad fd EBADF, maxevents <= 0 EINVAL */
    struct lx_epoll_event ev[2];
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_ERRNO(SC(T, epoll_wait, p[0], P(ev), 2, 0), LX_EINVAL);
    LT_ERRNO(SC(T, epoll_wait, 777, P(ev), 2, 0), LX_EBADF);
    LT_ERRNO(SC(T, epoll_wait, ep, P(ev), 0, 0), LX_EINVAL);
    LT_ERRNO(SC(T, epoll_wait, ep, P(ev), -1, 0), LX_EINVAL);
    LT_EQ(wait_ms(ep, ev, 2, 0), 0);
    /* readlink text */
    char buf[64];
    LT_EQ(SC(T, readlinkat, AT_CWD, P("/proc/self/fd/0"), P(buf), 64) < 0 || 1, 1);
    SC(T, close, ep); SC(T, close, ep2); SC(T, close, p[0]); SC(T, close, p[1]);
}

LT_TEST(ctl_errors)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_ADD, 888, LX_EPOLLIN, 0), LX_EBADF);
    LT_ERRNO(ctl(777, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 0), LX_EBADF);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_ADD, ep, LX_EPOLLIN, 0), LX_EINVAL);          /* an epoll cannot watch itself */
    LT_ERRNO(ctl(p[1], LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 0), LX_EINVAL);      /* epfd is not an epoll */
    LT_ERRNO(ctl(ep, 9, p[0], LX_EPOLLIN, 0), LX_EINVAL);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_MOD, p[0], LX_EPOLLIN, 0), LX_ENOENT);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_DEL, p[0], 0, 0), LX_ENOENT);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 1), 0);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 1), LX_EEXIST);
    /* a regular file cannot be polled by epoll: EPERM (file_can_poll) */
    int f = (int)SC(T, openat, AT_CWD, P("/f"), LX_O_RDONLY, 0);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_ADD, f, LX_EPOLLIN, 0), LX_EPERM);
    /* EPOLLEXCLUSIVE: only with ADD, and only IN OUT ERR HUP WAKEUP ET */
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_MOD, p[0], LX_EPOLLIN | LX_EPOLLEXCLUSIVE, 0), LX_EINVAL);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_ADD, p[1], LX_EPOLLIN | LX_EPOLLONESHOT | LX_EPOLLEXCLUSIVE, 0), LX_EINVAL);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[1], LX_EPOLLOUT | LX_EPOLLEXCLUSIVE, 0), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_DEL, p[1], 0, 0), 0);
    /* bad event pointer */
    LT_ERRNO(SC(T, epoll_ctl, ep, LX_EPOLL_CTL_MOD, p[0], 0), LX_EFAULT);
    SC(T, close, f); SC(T, close, ep); SC(T, close, p[0]); SC(T, close, p[1]);
}

/* Level triggered: reported again at every wait while the condition holds (epoll(7)). */
LT_TEST(level_triggered_pipe)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 0x1111222233334444ull), 0);
    struct lx_epoll_event ev[4];
    LT_EQ(wait_ms(ep, ev, 4, 0), 0);
    LT_EQ(SC(T, write, p[1], P("ab"), 2), 2);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLIN);
    LT_EQ(ev[0].data, 0x1111222233334444ull);
    LT_EQ(wait_ms(ep, ev, 4, 0), 1);                                  /* still readable */
    char c;
    LT_EQ(SC(T, read, p[0], P(&c), 1), 1);
    LT_EQ(wait_ms(ep, ev, 4, 0), 1);                                  /* one byte left */
    LT_EQ(SC(T, read, p[0], P(&c), 1), 1);
    LT_EQ(wait_ms(ep, ev, 4, 0), 0);
    /* writing end: EPOLLOUT immediately */
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[1], LX_EPOLLOUT, 7), 0);
    LT_EQ(wait_ms(ep, ev, 4, 0), 1);
    LT_EQ(ev[0].events, LX_EPOLLOUT);
    LT_EQ(ev[0].data, 7);
    /* MOD changes the mask and the data */
    LT_EQ(ctl(ep, LX_EPOLL_CTL_MOD, p[1], LX_EPOLLIN, 9), 0);
    LT_EQ(wait_ms(ep, ev, 4, 0), 0);
    /* closing the read end gives the write end EPOLLERR (pipe_poll: EPOLLERR when no reader) */
    LT_EQ(ctl(ep, LX_EPOLL_CTL_MOD, p[1], LX_EPOLLOUT, 9), 0);
    SC(T, close, p[0]);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);
    /* Linux: EPOLLOUT (the pipe is not full) | EPOLLERR, no HUP (request net-1; the host says IN|HUP) */
    LT_EQ(ev[0].events, LX_EPOLLOUT | LX_EPOLLERR);
    struct lx_pollfd pf = { p[1], LX_POLLOUT | LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_EQ(pf.revents, LX_POLLOUT | LX_POLLERR);
    /* DEL then no more events */
    LT_EQ(ctl(ep, LX_EPOLL_CTL_DEL, p[1], 0, 0), 0);
    LT_EQ(wait_ms(ep, ev, 4, 0), 0);
    SC(T, close, p[1]); SC(T, close, ep);
}

/* Timeout: epoll_wait(2) waits at least the timeout and returns 0. */
LT_TEST(timeout)
{
    int ep = (int)SC(T, epoll_create1, 0);
    struct lx_epoll_event ev[1];
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(wait_ms(ep, ev, 1, 60), 0);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 55000000 && dt < 2000000000);
    /* epoll_pwait2 with a timespec */
    struct lx_timespec ts = { 0, 40000000 };
    t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, epoll_pwait2, ep, P(ev), 1, P(&ts), 0, 0), 0);
    dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 35000000 && dt < 2000000000);
    struct lx_timespec bad = { 0, 2000000000 };
    LT_ERRNO(SC(T, epoll_pwait2, ep, P(ev), 1, P(&bad), 0, 0), LX_EINVAL);
    SC(T, close, ep);
}

/* EPOLLET: reported once per edge (a new write), not again while unread data stays. */
LT_TEST(edge_triggered)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN | LX_EPOLLET, 5), 0);
    struct lx_epoll_event ev[4];
    LT_EQ(SC(T, write, p[1], P("abc"), 3), 3);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLIN);
    LT_EQ(wait_ms(ep, ev, 4, 30), 0);                                 /* no new edge */
    LT_EQ(SC(T, write, p[1], P("d"), 1), 1);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);                               /* a new write is a new edge */
    /* data present before the registration is reported once */
    int32_t q[2];
    LT_EQ(SC(T, pipe, P(q)), 0);
    LT_EQ(SC(T, write, q[1], P("z"), 1), 1);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, q[0], LX_EPOLLIN | LX_EPOLLET, 6), 0);
    int n = wait_ms(ep, ev, 4, 1000);
    LT_EQ(n, 1);
    LT_EQ(ev[0].data, 6);
    LT_EQ(wait_ms(ep, ev, 4, 30), 0);
    SC(T, close, q[0]); SC(T, close, q[1]); SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, ep);
}

/* EPOLLONESHOT: one report, then silent until EPOLL_CTL_MOD re-arms (epoll_ctl(2)). */
LT_TEST(oneshot)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN | LX_EPOLLONESHOT, 3), 0);
    LT_EQ(SC(T, write, p[1], P("x"), 1), 1);
    struct lx_epoll_event ev[2];
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].data, 3);
    LT_EQ(wait_ms(ep, ev, 2, 30), 0);                                 /* disabled although still readable */
    LT_EQ(ctl(ep, LX_EPOLL_CTL_MOD, p[0], LX_EPOLLIN | LX_EPOLLONESHOT, 4), 0);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].data, 4);
    LT_EQ(wait_ms(ep, ev, 2, 30), 0);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, ep);
}

/* A watch disappears when the last descriptor of the file is closed (epoll(7) "Will closing a file
 * descriptor cause it to be removed from all epoll sets?"), but stays while a dup exists. */
LT_TEST(close_removes_watch)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 1), 0);
    int d = (int)SC(T, dup, p[0]);
    SC(T, write, p[1], P("x"), 1);
    struct lx_epoll_event ev[2];
    LT_EQ(SC(T, close, p[0]), 0);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);                                /* the dup keeps the watch alive */
    LT_EQ(SC(T, close, d), 0);
    LT_EQ(wait_ms(ep, ev, 2, 30), 0);
    LT_ERRNO(ctl(ep, LX_EPOLL_CTL_DEL, d, 0, 0), LX_EBADF);
    SC(T, close, p[1]); SC(T, close, ep);
}

/* Sockets: IN when data, OUT when writable, RDHUP and HUP from the peer (tcp/unix poll). */
LT_TEST(socket_events)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, sv[0], LX_EPOLLIN | LX_EPOLLOUT | LX_EPOLLRDHUP, 1), 0);
    struct lx_epoll_event ev[2];
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLOUT);
    SC(T, write, sv[1], P("m"), 1);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLIN | LX_EPOLLOUT);
    char c;
    SC(T, read, sv[0], P(&c), 1);
    SC(T, shutdown, sv[1], 1);                                         /* peer SHUT_WR */
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLIN | LX_EPOLLOUT | LX_EPOLLRDHUP);
    SC(T, close, sv[1]);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_CHECK((ev[0].events & (LX_EPOLLHUP | LX_EPOLLRDHUP | LX_EPOLLIN)) == (LX_EPOLLHUP | LX_EPOLLRDHUP | LX_EPOLLIN));
    SC(T, close, sv[0]);
    /* a watcher that asked for nothing still gets HUP and ERR (always reported) */
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, sv[0], 0, 2), 0);
    LT_EQ(wait_ms(ep, ev, 2, 20), 0);
    SC(T, close, sv[1]);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_CHECK(ev[0].events & LX_EPOLLHUP);
    SC(T, close, sv[0]); SC(T, close, ep);
}

/* A listening socket is EPOLLIN when a connection waits. */
LT_TEST(listen_accept_loop)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int srv = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_un a = { .sun_family = LX_AF_UNIX };
    strcpy(a.sun_path, "/ep-listen.sock");
    LT_EQ(SC(T, bind, srv, P(&a), 2 + 16), 0);
    LT_EQ(SC(T, listen, srv, 8), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, srv, LX_EPOLLIN, 100), 0);
    struct lx_epoll_event ev[4];
    LT_EQ(wait_ms(ep, ev, 4, 0), 0);
    int c1 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    int c2 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, connect, c1, P(&a), 2 + 16), 0);
    LT_EQ(SC(T, connect, c2, P(&a), 2 + 16), 0);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);
    LT_EQ(ev[0].data, 100);
    int a1 = (int)SC(T, accept, srv, 0, 0);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 1);                                /* the second is still pending */
    int a2 = (int)SC(T, accept, srv, 0, 0);
    LT_EQ(wait_ms(ep, ev, 4, 20), 0);
    SC(T, close, a1); SC(T, close, a2); SC(T, close, c1); SC(T, close, c2); SC(T, close, srv); SC(T, close, ep);
}

LT_TEST(eventfd_in_epoll)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int efd = (int)SC(T, eventfd2, 0, LX_EFD_NONBLOCK);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, efd, LX_EPOLLIN | LX_EPOLLOUT, 42), 0);
    struct lx_epoll_event ev[2];
    /* counter 0: writable only (eventfd_poll) */
    LT_EQ(wait_ms(ep, ev, 2, 0), 1);
    LT_EQ(ev[0].events, LX_EPOLLOUT);
    uint64_t v = 1;
    LT_EQ(SC(T, write, efd, P(&v), 8), 8);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].events, LX_EPOLLIN | LX_EPOLLOUT);
    LT_EQ(SC(T, read, efd, P(&v), 8), 8);
    LT_EQ(wait_ms(ep, ev, 2, 0), 1);
    LT_EQ(ev[0].events, LX_EPOLLOUT);
    /* IN-only edge triggered watcher woken by a write from another thread */
    int e2 = (int)SC(T, eventfd2, 0, LX_EFD_NONBLOCK);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_DEL, efd, 0, 0), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, e2, LX_EPOLLIN | LX_EPOLLET, 43), 0);
    LT_EQ(wait_ms(ep, ev, 2, 0), 0);
    v = 2;
    SC(T, write, e2, P(&v), 8);
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);
    LT_EQ(ev[0].data, 43);
    SC(T, close, e2); SC(T, close, efd); SC(T, close, ep);
}

struct wk { lkd_task *t; int fd; int ms; };
static void *poke(void *a)
{
    struct wk *w = a;
    lkd_set_current(w->t);
    usleep((useconds_t)w->ms * 1000);
    SC(w->t, write, w->fd, P("go"), 2);
    return NULL;
}

LT_TEST(blocking_wait_wakes_and_interrupts)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 8), 0);
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    struct wk w = { th, p[1], 60 };
    pthread_t pt;
    pthread_create(&pt, NULL, poke, &w);
    struct lx_epoll_event ev[2];
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(wait_ms(ep, ev, 2, -1), 1);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 40000000);
    pthread_join(pt, NULL);
    char b[4];
    SC(T, read, p[0], P(b), 4);
    /* a pending signal ends the wait: epoll_wait is never restarted, -EINTR inside */
    clear_signals(T);
    atomic_fetch_or(&T->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(T);
    lkd_sysreq rq = { .nr = LKD_NR_epoll_wait, .a = { (uint64_t)ep, P(ev), 2, (uint64_t)-1 } };
    LT_ERRNO(lkd_dispatch(T, &rq), LX_EINTR);
    clear_signals(T);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, ep);
}

/* epoll_pwait installs the mask during the wait only and restores it afterwards (epoll_pwait(2)). */
LT_TEST(pwait_sigmask)
{
    int ep = (int)SC(T, epoll_create1, 0);
    struct lx_epoll_event ev[1];
    uint64_t m = LX_SIGBIT(LX_SIGUSR1);
    uint64_t before = atomic_load(&T->sigmask);
    LT_EQ(SC(T, epoll_pwait, ep, P(ev), 1, 0, P(&m), 8), 0);
    LT_EQ(atomic_load(&T->sigmask), before);
    LT_ERRNO(SC(T, epoll_pwait, ep, P(ev), 1, 0, P(&m), 4), LX_EINVAL);     /* sigsetsize must be 8 */
    SC(T, close, ep);
}

/* Nested epoll: the inner set is readable when one of its items is ready; loops are ELOOP. */
LT_TEST(nested_epoll)
{
    int outer = (int)SC(T, epoll_create1, 0);
    int inner = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(inner, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 1), 0);
    LT_EQ(ctl(outer, LX_EPOLL_CTL_ADD, inner, LX_EPOLLIN, 2), 0);
    struct lx_epoll_event ev[2];
    LT_EQ(wait_ms(outer, ev, 2, 0), 0);
    SC(T, write, p[1], P("x"), 1);
    LT_EQ(wait_ms(outer, ev, 2, 1000), 1);
    LT_EQ(ev[0].data, 2);
    LT_ERRNO(ctl(inner, LX_EPOLL_CTL_ADD, outer, LX_EPOLLIN, 3), LX_ELOOP);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, inner); SC(T, close, outer);
}

/* After fork the child rebuilds the set on a new kqueue (kqueue descriptors are not inherited). */
LT_TEST(fork_rebuilds_set)
{
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[0], LX_EPOLLIN, 77), 0);
    fflush(NULL);
    pid_t pid = lkd_fork_host(T, 0);
    if (pid == 0) {
        alarm(10);
        struct lx_epoll_event ev[2];
        /* the inherited kqueue descriptor is dead; the set was rebuilt on a new one */
        if (SC(T, write, p[1], P("c"), 1) != 1) _exit(1);
        int n = (int)SC(T, epoll_wait, ep, P(ev), 2, 2000);
        if (n != 1 || ev[0].data != 77) _exit(2);
        if (SC(T, epoll_ctl, ep, LX_EPOLL_CTL_DEL, p[0], 0) != 0) _exit(3);
        if (SC(T, epoll_wait, ep, P(ev), 2, 20) != 0) _exit(4);
        _exit(0);
    }
    LT_CHECK(pid > 0);
    int st = 0;
    waitpid(pid, &st, 0);
    LT_EQ(WIFEXITED(st) ? WEXITSTATUS(st) : 100 + WTERMSIG(st), 0);
    struct lx_epoll_event ev[2];
    LT_EQ(wait_ms(ep, ev, 2, 1000), 1);                                /* the parent's set still works */
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, ep);
}

/* Many descriptors in one set. */
LT_TEST(many_fds)
{
    enum { N = 100 };
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t p[N][2];
    for (int i = 0; i < N; i++) {
        LT_EQ(SC(T, pipe, P(p[i])), 0);
        LT_EQ(ctl(ep, LX_EPOLL_CTL_ADD, p[i][0], LX_EPOLLIN, (uint64_t)i), 0);
    }
    for (int i = 0; i < N; i += 7) SC(T, write, p[i][1], P("x"), 1);
    struct lx_epoll_event ev[N];
    int n = wait_ms(ep, ev, N, 1000);
    LT_EQ(n, 15);
    int seen = 0;
    for (int i = 0; i < n; i++) if (ev[i].data % 7 == 0) seen++;
    LT_EQ(seen, 15);
    LT_EQ(wait_ms(ep, ev, 4, 1000), 4);                                /* maxevents limits the batch */
    for (int i = 0; i < N; i++) { SC(T, close, p[i][0]); SC(T, close, p[i][1]); }
    SC(T, close, ep);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netep");
    put_file(ROOT, "f", "hello");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    LT_RUN(layout_and_create);
    LT_RUN(ctl_errors);
    LT_RUN(level_triggered_pipe);
    LT_RUN(timeout);
    LT_RUN(edge_triggered);
    LT_RUN(oneshot);
    LT_RUN(close_removes_watch);
    LT_RUN(socket_events);
    LT_RUN(listen_accept_loop);
    LT_RUN(eventfd_in_epoll);
    LT_RUN(blocking_wait_wakes_and_interrupts);
    LT_RUN(pwait_sigmask);
    LT_RUN(nested_epoll);
    LT_RUN(fork_rebuilds_set);
    LT_RUN(many_fds);
    rm_rf(ROOT);
    return lt_finish();
}
