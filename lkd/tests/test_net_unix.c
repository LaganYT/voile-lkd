/* test_net_unix.c: AF_UNIX sockets through the syscall layer. Linux references: unix(7), socket(2),
 * socketpair(2), bind(2), listen(2), accept(2), connect(2), sendmsg(2), recvmsg(2), cmsg(3),
 * net/unix/af_unix.c and net/core/scm.c of v6.12. */
#include <pthread.h>
#include <poll.h>

#include "../src/include/net.h"
#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

/* The tests write to closed sockets on purpose: drop the SIGPIPE that LKD queued for the thread. */
/* Consume a pending SIGPIPE (rt_sigtimedwait with a zero timeout); returns the signal number or <0. */
static int64_t take_sigpipe(void)
{
    uint64_t set = LX_SIGBIT(LX_SIGPIPE);
    struct lx_timespec zero = { 0, 0 };
    return SC(T, rt_sigtimedwait, P(&set), 0, P(&zero), 8);
}

static void clear_signals(lkd_task *t)
{
    atomic_fetch_and(&t->flags, ~(uint32_t)(LKD_TF_SIGPENDING | LKD_TF_FATAL));
    char b[64];
    while (read(t->wake_rd, b, sizeof b) > 0) { }
}

#define SOCKADDR_UN(name, path) struct lx_sockaddr_un name = { .sun_family = LX_AF_UNIX }; strcpy(name.sun_path, path)
#define UNLEN(path) ((uint32_t)(2 + strlen(path) + 1))

LT_TEST(socket_creation_errors)
{
    /* socket(2): unknown family EAFNOSUPPORT, bad type ESOCKTNOSUPPORT, bad protocol EPROTONOSUPPORT */
    LT_ERRNO(SC(T, socket, 99, LX_SOCK_STREAM, 0), LX_EAFNOSUPPORT);
    LT_ERRNO(SC(T, socket, LX_AF_UNIX, 9, 0), LX_ESOCKTNOSUPPORT);
    LT_ERRNO(SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 5), LX_EPROTONOSUPPORT);
    LT_ERRNO(SC(T, socket, LX_AF_UNIX, 0x1000 | LX_SOCK_STREAM, 0), LX_EINVAL);
    /* AF_NETLINK: NETLINK_ROUTE only (ifaces.c, test_net_ifaces.c); other protocols and AF_PACKET refused */
    LT_ERRNO(SC(T, socket, LX_AF_NETLINK, LX_SOCK_RAW, 15 /* NETLINK_KOBJECT_UEVENT */), LX_EPROTONOSUPPORT);
    LT_ERRNO(SC(T, socket, LX_AF_NETLINK, LX_SOCK_STREAM, 0), LX_ESOCKTNOSUPPORT);
    LT_ERRNO(SC(T, socket, LX_AF_PACKET, LX_SOCK_RAW, 0), LX_EPERM);
    /* SOCK_SEQPACKET on AF_UNIX: honest ESOCKTNOSUPPORT (the host has none) */
    LT_ERRNO(SC(T, socket, LX_AF_UNIX, LX_SOCK_SEQPACKET, 0), LX_ESOCKTNOSUPPORT);
    int fd = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM | LX_SOCK_CLOEXEC | LX_SOCK_NONBLOCK, 0);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFL) & LX_O_NONBLOCK, LX_O_NONBLOCK);
    LT_EQ(SC(T, close, fd), 0);
    /* not a socket */
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_ERRNO(SC(T, listen, p[0], 1), LX_ENOTSOCK);
    LT_ERRNO(SC(T, bind, 999, 0, 0), LX_EBADF);
    SC(T, close, p[0]); SC(T, close, p[1]);
}

/* socketpair(2) stream: data both ways, EOF after the peer closes, EPIPE after the peer closes. */
LT_TEST(socketpair_stream)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    LT_EQ(SC(T, write, sv[0], P("hello"), 5), 5);
    char b[16] = { 0 };
    LT_EQ(SC(T, read, sv[1], P(b), sizeof b), 5);
    LT_STREQ(b, "hello");
    LT_EQ(SC(T, sendto, sv[1], P("xyz"), 3, 0, 0, 0), 3);
    LT_EQ(SC(T, recvfrom, sv[0], P(b), sizeof b, 0, 0, 0), 3);
    /* nonblocking read with no data: EAGAIN */
    SC(T, fcntl, sv[0], LX_F_SETFL, LX_O_NONBLOCK);
    LT_ERRNO(SC(T, read, sv[0], P(b), sizeof b), LX_EAGAIN);
    LT_EQ(SC(T, fcntl, sv[0], LX_F_GETFL) & LX_O_NONBLOCK, LX_O_NONBLOCK);
    /* SO_TYPE / SO_DOMAIN / SO_PROTOCOL */
    int32_t v = 0, vl = 4;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_TYPE, P(&v), P(&vl)), 0);
    LT_EQ(v, LX_SOCK_STREAM);
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_DOMAIN, P(&v), P(&vl)), 0);
    LT_EQ(v, LX_AF_UNIX);
    /* shutdown(SHUT_WR) gives the peer EOF (unix(7)) while it can still write back */
    LT_EQ(SC(T, shutdown, sv[0], 1), 0);
    SC(T, fcntl, sv[1], LX_F_SETFL, LX_O_NONBLOCK);
    LT_EQ(SC(T, read, sv[1], P(b), sizeof b), 0);
    LT_EQ(SC(T, write, sv[1], P("back"), 4), 4);
    LT_EQ(SC(T, read, sv[0], P(b), sizeof b), 4);
    LT_ERRNO(SC(T, shutdown, sv[0], 7), LX_EINVAL);
    LT_EQ(SC(T, close, sv[1]), 0);
    LT_EQ(SC(T, read, sv[0], P(b), sizeof b), 0);       /* EOF */
    LT_ERRNO(SC(T, write, sv[0], P("x"), 1), LX_EPIPE);
    /* send(2): a plain write raised SIGPIPE; MSG_NOSIGNAL suppresses it */
    LT_EQ(take_sigpipe(), LX_SIGPIPE);
    LT_ERRNO(take_sigpipe(), LX_EAGAIN);
    LT_ERRNO(SC(T, sendto, sv[0], P("x"), 1, LX_MSG_NOSIGNAL, 0, 0), LX_EPIPE);
    LT_ERRNO(take_sigpipe(), LX_EAGAIN);
    LT_ERRNO(SC(T, sendto, sv[0], P("x"), 1, 0, 0, 0), LX_EPIPE);
    LT_EQ(take_sigpipe(), LX_SIGPIPE);
    clear_signals(T);
    SC(T, close, sv[0]);
}

/* SOCK_DGRAM keeps message boundaries; a short buffer truncates and MSG_TRUNC returns the real size. */
LT_TEST(socketpair_dgram)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_DGRAM, 0, P(sv)), 0);
    LT_EQ(SC(T, write, sv[0], P("aaaa"), 4), 4);
    LT_EQ(SC(T, write, sv[0], P("bbbbbb"), 6), 6);
    char b[16] = { 0 };
    LT_EQ(SC(T, recvfrom, sv[1], P(b), sizeof b, 0, 0, 0), 4);
    LT_EQ(SC(T, recvfrom, sv[1], P(b), 2, LX_MSG_TRUNC, 0, 0), 6);   /* recv(2): MSG_TRUNC returns the real length */
    SC(T, fcntl, sv[1], LX_F_SETFL, LX_O_NONBLOCK);
    LT_ERRNO(SC(T, recvfrom, sv[1], P(b), sizeof b, 0, 0, 0), LX_EAGAIN);
    LT_ERRNO(SC(T, recvfrom, sv[1], P(b), sizeof b, LX_MSG_DONTWAIT, 0, 0), LX_EAGAIN);
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

LT_TEST(bind_listen_accept_connect)
{
    int srv = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    SOCKADDR_UN(a, "/srv.sock");
    LT_ERRNO(SC(T, listen, srv, 5), LX_EINVAL);                       /* unbound */
    LT_EQ(SC(T, bind, srv, P(&a), UNLEN("/srv.sock")), 0);
    LT_ERRNO(SC(T, bind, srv, P(&a), UNLEN("/srv.sock")), LX_EINVAL); /* already bound */
    int s2 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_ERRNO(SC(T, bind, s2, P(&a), UNLEN("/srv.sock")), LX_EADDRINUSE);
    /* the socket file exists in the guest file system with S_IFSOCK */
    struct lx_stat st;
    LT_EQ(SC(T, stat, P("/srv.sock"), P(&st)), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFSOCK);
    LT_EQ(SC(T, listen, srv, 5), 0);
    int32_t v = 0, vl = 4;
    LT_EQ(SC(T, getsockopt, srv, LX_SOL_SOCKET, LX_SO_ACCEPTCONN, P(&v), P(&vl)), 0);
    LT_EQ(v, 1);
    /* getsockname returns the path as given */
    struct lx_sockaddr_un got;
    int32_t gl = sizeof got;
    memset(&got, 0xff, sizeof got);
    LT_EQ(SC(T, getsockname, srv, P(&got), P(&gl)), 0);
    LT_EQ(gl, 2 + 10);
    LT_EQ(got.sun_family, LX_AF_UNIX);
    LT_STREQ(got.sun_path, "/srv.sock");
    /* connect, accept with the peer address (the client is unbound: namelen 2 on Linux) */
    LT_EQ(SC(T, connect, s2, P(&a), UNLEN("/srv.sock")), 0);
    struct lx_sockaddr_un pa;
    int32_t pl = sizeof pa;
    int acc = (int)SC(T, accept4, srv, P(&pa), P(&pl), LX_SOCK_CLOEXEC);
    LT_CHECK(acc >= 0);
    LT_EQ(pl, 2);                                                  /* unix_getname of an unbound peer */
    LT_EQ(SC(T, fcntl, acc, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, write, s2, P("ping"), 4), 4);
    char b[8] = { 0 };
    LT_EQ(SC(T, read, acc, P(b), sizeof b), 4);
    LT_STREQ(b, "ping");
    /* getpeername on the client reports the server path */
    memset(&got, 0, sizeof got);
    gl = sizeof got;
    LT_EQ(SC(T, getpeername, s2, P(&got), P(&gl)), 0);
    LT_STREQ(got.sun_path, "/srv.sock");
    /* a short buffer truncates the address but reports the full length (move_addr_to_user) */
    char small[4];
    gl = 4;
    LT_EQ(SC(T, getsockname, srv, P(small), P(&gl)), 0);
    LT_EQ(gl, 12);
    LT_EQ(small[0], LX_AF_UNIX);
    /* nonblocking accept with nothing pending */
    SC(T, fcntl, srv, LX_F_SETFL, LX_O_NONBLOCK);
    LT_ERRNO(SC(T, accept, srv, 0, 0), LX_EAGAIN);
    /* connect to a path nobody listens on, and to a missing path */
    SOCKADDR_UN(miss, "/nothing.sock");
    int c3 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_ERRNO(SC(T, connect, c3, P(&miss), UNLEN("/nothing.sock")), LX_ENOENT);
    put_file(ROOT, "plain", "x");
    put_dir(ROOT, "sub");
    put_file(ROOT, "sub/inner.txt", "hello");
    SOCKADDR_UN(pl2, "/plain");
    LT_ERRNO(SC(T, connect, c3, P(&pl2), UNLEN("/plain")), LX_ECONNREFUSED);   /* connect(2) on a non-socket: ECONNREFUSED */
    LT_ERRNO(SC(T, connect, c3, P(&a), 1), LX_EINVAL);
    SC(T, close, c3); SC(T, close, acc); SC(T, close, s2); SC(T, close, srv);
}

LT_TEST(dgram_named)
{
    int a = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0);
    int b = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0);
    SOCKADDR_UN(an, "/dg-a.sock");
    SOCKADDR_UN(bn, "/dg-b.sock");
    LT_EQ(SC(T, bind, a, P(&an), UNLEN("/dg-a.sock")), 0);
    LT_EQ(SC(T, bind, b, P(&bn), UNLEN("/dg-b.sock")), 0);
    /* sendto a named peer; recvfrom reports the sender's bound name */
    LT_EQ(SC(T, sendto, b, P("dgram"), 5, 0, P(&an), UNLEN("/dg-a.sock")), 5);
    char buf[16] = { 0 };
    struct lx_sockaddr_un from;
    int32_t fl = sizeof from;
    memset(&from, 0, sizeof from);
    LT_EQ(SC(T, recvfrom, a, P(buf), sizeof buf, 0, P(&from), P(&fl)), 5);
    LT_STREQ(buf, "dgram");
    LT_EQ(from.sun_family, LX_AF_UNIX);
    LT_STREQ(from.sun_path, "/dg-b.sock");
    LT_EQ(fl, 2 + 11);
    /* connect then plain send */
    LT_EQ(SC(T, connect, b, P(&an), UNLEN("/dg-a.sock")), 0);
    LT_EQ(SC(T, write, b, P("two"), 3), 3);
    LT_EQ(SC(T, read, a, P(buf), sizeof buf), 3);
    SC(T, close, a); SC(T, close, b);
}

LT_TEST(abstract_namespace)
{
    /* unix(7): sun_path[0] == 0 is the abstract namespace; getsockname reports the name verbatim */
    uint8_t addr[2 + 1 + 6];
    uint16_t fam = LX_AF_UNIX;
    memcpy(addr, &fam, 2);
    addr[2] = 0;
    memcpy(addr + 3, "wineab", 6);
    int s = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    int s2 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, bind, s, P(addr), sizeof addr), 0);
    LT_ERRNO(SC(T, bind, s2, P(addr), sizeof addr), LX_EADDRINUSE);
    LT_EQ(SC(T, listen, s, 2), 0);
    LT_EQ(SC(T, connect, s2, P(addr), sizeof addr), 0);
    uint8_t got[32] = { 0 };
    int32_t gl = sizeof got;
    LT_EQ(SC(T, getsockname, s, P(got), P(&gl)), 0);
    LT_EQ(gl, 9);
    LT_MEMEQ(got + 3, "wineab", 6);
    LT_EQ(got[2], 0);
    gl = sizeof got;
    LT_EQ(SC(T, getpeername, s2, P(got), P(&gl)), 0);
    LT_EQ(gl, 9);
    LT_MEMEQ(got + 3, "wineab", 6);
    /* autobind: bind with only the family assigns a 5 hex digit abstract name */
    int s3 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0);
    LT_EQ(SC(T, bind, s3, P(&fam), 2), 0);
    gl = sizeof got;
    memset(got, 0xff, sizeof got);
    LT_EQ(SC(T, getsockname, s3, P(got), P(&gl)), 0);
    LT_EQ(gl, 8);
    LT_EQ(got[2], 0);
    SC(T, close, s3); SC(T, close, s2); SC(T, close, s);
    /* the name is free again after the owner closed */
    int s4 = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, bind, s4, P(addr), sizeof addr), 0);
    SC(T, close, s4);
}

/* A guest path longer than the 104 bytes of Darwin sun_path still binds: the host path is split into a
 * directory and a leaf (the host root of the tests is already long). */
LT_TEST(long_path_bind)
{
    SC(T, mkdir, P("/d1234567890123456789012345678901234567890"), 0755);
    char gp[128] = "/d1234567890123456789012345678901234567890/s";
    for (int i = 0; i < 40; i++) strcat(gp, "x");
    strcat(gp, ".sock");
    LT_CHECK(strlen(gp) < 108);
    SOCKADDR_UN(a, "");
    strcpy(a.sun_path, gp);
    int s = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, bind, s, P(&a), UNLEN(gp)), 0);
    LT_EQ(SC(T, listen, s, 1), 0);
    int c = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, connect, c, P(&a), UNLEN(gp)), 0);
    int acc = (int)SC(T, accept, s, 0, 0);
    LT_CHECK(acc >= 0);
    LT_EQ(SC(T, write, c, P("long"), 4), 4);
    char b[8] = { 0 };
    LT_EQ(SC(T, read, acc, P(b), sizeof b), 4);
    struct lx_sockaddr_un got;
    int32_t gl = sizeof got;
    LT_EQ(SC(T, getpeername, c, P(&got), P(&gl)), 0);
    LT_STREQ(got.sun_path, gp);
    SC(T, close, acc); SC(T, close, c); SC(T, close, s);
}

/* ---- SCM_RIGHTS ---------------------------------------------------------------------------------- */

static int64_t send_fds(int sock, const int32_t *fds, int n, const char *payload)
{
    struct { struct lx_cmsghdr h; int32_t fd[8]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    ctl.h.cmsg_len = LX_CMSG_LEN(n * 4);
    ctl.h.cmsg_level = LX_SOL_SOCKET;
    ctl.h.cmsg_type = LX_SCM_RIGHTS;
    memcpy(ctl.fd, fds, n * 4);
    struct lx_iovec iov = { P(payload), strlen(payload) };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&ctl), .msg_controllen = LX_CMSG_SPACE(n * 4) };
    return SC(T, sendmsg, sock, P(&m), 0);
}

static int64_t recv_fds(int sock, int32_t *fds, int cap, char *buf, size_t bl, int *nfds, int flags, int32_t *msgflags)
{
    struct { struct lx_cmsghdr h; int32_t fd[8]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    struct lx_iovec iov = { P(buf), bl };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&ctl), .msg_controllen = LX_CMSG_LEN(cap * 4) };
    int64_t r = SC(T, recvmsg, sock, P(&m), flags);
    *nfds = 0;
    if (msgflags) *msgflags = m.msg_flags;
    if (r >= 0 && m.msg_controllen >= sizeof(struct lx_cmsghdr) && ctl.h.cmsg_level == LX_SOL_SOCKET && ctl.h.cmsg_type == LX_SCM_RIGHTS) {
        *nfds = (int)((ctl.h.cmsg_len - sizeof(struct lx_cmsghdr)) / 4);
        memcpy(fds, ctl.fd, *nfds * 4);
    }
    return r;
}

LT_TEST(scm_rights_pipe_and_file)
{
    int32_t sv[2], p[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    LT_EQ(SC(T, pipe, P(p)), 0);
    int32_t send[2] = { p[1], p[0] };
    LT_EQ(send_fds(sv[0], send, 2, "fds"), 3);
    SC(T, close, p[0]); SC(T, close, p[1]);
    int32_t got[8];
    int n;
    char b[8] = { 0 };
    LT_EQ(recv_fds(sv[1], got, 8, b, sizeof b, &n, 0, NULL), 3);
    LT_EQ(n, 2);
    LT_STREQ(b, "fds");
    /* the received descriptors are new numbers on the same descriptions: write end, then read end */
    LT_CHECK(got[0] != send[0] && got[1] != send[1]);
    LT_EQ(SC(T, write, got[0], P("via"), 3), 3);
    char r[8] = { 0 };
    LT_EQ(SC(T, read, got[1], P(r), sizeof r), 3);
    LT_STREQ(r, "via");
    /* MSG_CMSG_CLOEXEC sets close-on-exec on the received descriptors (recvmsg(2)) */
    int32_t bad[1] = { 999 };
    LT_EQ(send_fds(sv[0], bad, 1, "c"), -LX_EBADF);          /* unknown descriptor: EBADF, nothing in flight */
    int fd = (int)SC(T, openat, AT_CWD, P("/plain"), LX_O_RDONLY, 0);
    int32_t one[1] = { fd };
    LT_EQ(send_fds(sv[0], one, 1, "c"), 1);
    int32_t g2[1];
    LT_EQ(recv_fds(sv[1], g2, 1, b, 1, &n, LX_MSG_CMSG_CLOEXEC, NULL), 1);
    LT_EQ(n, 1);
    LT_EQ(SC(T, fcntl, g2[0], LX_F_GETFD), LX_FD_CLOEXEC);
    char c;
    LT_EQ(SC(T, read, g2[0], P(&c), 1), 1);
    LT_EQ(c, 'x');
    SC(T, close, g2[0]); SC(T, close, fd); SC(T, close, got[0]); SC(T, close, got[1]);
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* A control buffer too small for the descriptors: the extra ones are closed and MSG_CTRUNC is set
 * (scm_detach_fds); no descriptor leaks into the receiver's table. */
LT_TEST(scm_rights_truncated)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    int32_t fds[3];
    for (int i = 0; i < 3; i++) fds[i] = (int32_t)SC(T, openat, AT_CWD, P("/plain"), LX_O_RDONLY, 0);
    LT_EQ(send_fds(sv[0], fds, 3, "t"), 1);
    int before = lkd_fd_count(T);
    int32_t got[8];
    int n;
    char b[4];
    int32_t mf = 0;
    LT_EQ(recv_fds(sv[1], got, 1, b, sizeof b, &n, 0, &mf), 1);
    LT_EQ(n, 1);
    LT_EQ(mf & LX_MSG_CTRUNC, LX_MSG_CTRUNC);
    LT_EQ(lkd_fd_count(T), before + 1);
    for (int i = 0; i < 3; i++) SC(T, close, fds[i]);
    SC(T, close, got[0]); SC(T, close, sv[0]); SC(T, close, sv[1]);
}

LT_TEST(scm_rights_socket_and_eventfd)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    int32_t inner[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(inner)), 0);
    int efd = (int)SC(T, eventfd2, 5, LX_EFD_NONBLOCK);
    LT_CHECK(efd >= 0);
    int32_t send[2] = { inner[1], efd };
    LT_EQ(send_fds(sv[0], send, 2, "o"), 1);
    int32_t got[8];
    int n;
    char b[4];
    LT_EQ(recv_fds(sv[1], got, 8, b, sizeof b, &n, 0, NULL), 1);
    LT_EQ(n, 2);
    /* the received socket is the peer of inner[0] */
    LT_EQ(SC(T, write, inner[0], P("hi"), 2), 2);
    char r[4] = { 0 };
    LT_EQ(SC(T, read, got[0], P(r), sizeof r), 2);
    LT_STREQ(r, "hi");
    int32_t ty = 0, tl = 4;
    LT_EQ(SC(T, getsockopt, got[0], LX_SOL_SOCKET, LX_SO_TYPE, P(&ty), P(&tl)), 0);
    LT_EQ(ty, LX_SOCK_STREAM);
    /* the received eventfd shares the counter with the original */
    uint64_t v = 0;
    LT_EQ(SC(T, read, got[1], P(&v), 8), 8);
    LT_EQ(v, 5);
    v = 3;
    LT_EQ(SC(T, write, efd, P(&v), 8), 8);
    v = 0;
    LT_EQ(SC(T, read, got[1], P(&v), 8), 8);
    LT_EQ(v, 3);
    /* epoll and timerfd objects cannot cross processes: refused visibly */
    int ep = (int)SC(T, epoll_create1, 0);
    int32_t e1[1] = { ep };
    LT_EQ(send_fds(sv[0], e1, 1, "e"), -LX_EOPNOTSUPP);
    for (int i = 0; i < 2; i++) SC(T, close, got[i]);
    SC(T, close, ep); SC(T, close, efd); SC(T, close, inner[0]); SC(T, close, inner[1]);
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* SCM_RIGHTS between two real processes: the child receives a pipe end and an eventfd. */
static int32_t g_sv1, g_pipe_w;
static void child_receiver(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t got[4];
    int n;
    char b[4];
    if (recv_fds(g_sv1, got, 4, b, sizeof b, &n, 0, NULL) < 0 || n != 2) _exit(1);
    uint64_t v = 0;
    if (SC(T, read, got[1], P(&v), 8) != 8 || v != 7) _exit(2);
    v = 11;
    if (SC(T, write, got[1], P(&v), 8) != 8) _exit(3);          /* the parent reads 11 from its own eventfd */
    if (SC(T, write, got[0], P("kid"), 3) != 3) _exit(4);
    _exit(0);
}
LT_TEST(scm_rights_across_fork)
{
    int32_t sv[2], p[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    LT_EQ(SC(T, pipe, P(p)), 0);
    int efd = (int)SC(T, eventfd2, 7, 0);
    int32_t send[2] = { p[1], efd };
    LT_EQ(send_fds(sv[0], send, 2, "go"), 2);
    g_sv1 = sv[1];
    g_pipe_w = p[1];
    LT_EQ(lt_run_child(child_receiver, NULL, 10), 0);
    char r[4] = { 0 };
    LT_EQ(SC(T, read, p[0], P(r), 3), 3);
    LT_STREQ(r, "kid");
    uint64_t v = 0;
    LT_EQ(SC(T, read, efd, P(&v), 8), 8);
    LT_EQ(v, 11);
    SC(T, close, p[0]); SC(T, close, p[1]); SC(T, close, efd); SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* SCM_RIGHTS of a directory between two real processes (request wine-3: wineserver sends directory fds
 * for NtQueryDirectoryFile, the client does fchdir(fd) then opendir(".")). The receiver gets the same
 * open file description (net/core/scm.c): a directory, usable by fchdir, openat(fd, ...), getdents64,
 * fstat, with the same /proc/self/fd/N text. Exit codes name the failing step. */
static int32_t g_dir_sv;
static void child_dir_receiver(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t got[2];
    int n;
    char b[4];
    if (recv_fds(g_dir_sv, got, 2, b, sizeof b, &n, LX_MSG_CMSG_CLOEXEC, NULL) < 0 || n != 1) _exit(1);
    int d = got[0];
    struct lx_stat st;
    if (SC(T, fstat, d, P(&st)) != 0 || (st.st_mode & LX_S_IFMT) != LX_S_IFDIR) _exit(2);
    int f = (int)SC(T, openat, d, P("inner.txt"), LX_O_RDONLY, 0);
    if (f < 0) _exit(3);
    char c[8] = { 0 };
    if (SC(T, read, f, P(c), sizeof c) != 5 || memcmp(c, "hello", 5)) _exit(4);
    SC(T, close, f);
    char ents[4096];
    int64_t gn = SC(T, getdents64, d, P(ents), sizeof ents);
    if (gn <= 0) _exit(5);
    int seen = 0;
    for (int64_t off = 0; off < gn;) {
        struct lx_linux_dirent64 *e = (struct lx_linux_dirent64 *)(ents + off);
        if (!strcmp(e->d_name, "inner.txt")) seen = 1;
        off += e->d_reclen;
    }
    if (!seen) _exit(6);
    if (SC(T, fchdir, d, 0) != 0) _exit(7);
    char cwd[256] = { 0 };
    if (SC(T, getcwd, P(cwd), sizeof cwd) <= 0 || strcmp(cwd, "/sub")) _exit(8);
    if (SC(T, openat, AT_CWD, P("inner.txt"), LX_O_RDONLY, 0) < 0) _exit(9);
    char link[64], lt[256] = { 0 };
    snprintf(link, sizeof link, "/proc/self/fd/%d", d);
    if (SC(T, readlinkat, AT_CWD, P(link), P(lt), sizeof lt - 1) != 4 || strcmp(lt, "/sub")) _exit(10);
    _exit(0);
}

LT_TEST(scm_rights_directory_across_fork)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    int d = (int)SC(T, openat, AT_CWD, P("/sub"), LX_O_RDONLY | LX_O_NONBLOCK, 0);   /* as wineserver opens it */
    LT_CHECK(d >= 0);
    int32_t one[1] = { d };
    LT_EQ(send_fds(sv[0], one, 1, "d"), 1);
    g_dir_sv = sv[1];
    LT_EQ(lt_run_child(child_dir_receiver, NULL, 10), 0);
    SC(T, close, d); SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* SO_PASSCRED: the receiver gets SCM_CREDENTIALS with the sender's real pid, uid and gid (unix(7)); a
 * sender may only claim its own identity unless privileged (net/core/scm.c scm_check_creds). */
LT_TEST(scm_credentials)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    int32_t on = 1;
    LT_EQ(SC(T, setsockopt, sv[1], LX_SOL_SOCKET, LX_SO_PASSCRED, P(&on), 4), 0);
    struct { struct lx_cmsghdr h; struct lx_ucred u; } sc;
    memset(&sc, 0, sizeof sc);
    sc.h.cmsg_len = LX_CMSG_LEN(12);
    sc.h.cmsg_level = LX_SOL_SOCKET;
    sc.h.cmsg_type = LX_SCM_CREDENTIALS;
    sc.u.pid = T->proc->pid;
    sc.u.uid = T->proc->cred.uid;
    sc.u.gid = T->proc->cred.gid;
    struct lx_iovec iov = { P("cred"), 4 };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&sc), .msg_controllen = LX_CMSG_SPACE(12) };
    uint32_t saved_euid = T->proc->cred.euid;
    T->proc->cred.euid = T->proc->cred.uid ? T->proc->cred.uid : 1000;
    LT_EQ(SC(T, sendmsg, sv[0], P(&m), 0), 4);                       /* own identity: accepted */
    sc.u.pid = T->proc->pid + 1;
    LT_ERRNO(SC(T, sendmsg, sv[0], P(&m), 0), LX_EPERM);              /* someone else's pid: refused */
    T->proc->cred.euid = saved_euid;
    struct { struct lx_cmsghdr h; struct lx_ucred u; } rc;
    memset(&rc, 0, sizeof rc);
    char b[8] = { 0 };
    struct lx_iovec riov = { P(b), sizeof b };
    struct lx_msghdr rm = { .msg_iov = P(&riov), .msg_iovlen = 1, .msg_control = P(&rc), .msg_controllen = sizeof rc };
    LT_EQ(SC(T, recvmsg, sv[1], P(&rm), 0), 4);
    LT_EQ(rm.msg_controllen, LX_CMSG_SPACE(12));
    LT_EQ(rc.h.cmsg_level, LX_SOL_SOCKET);
    LT_EQ(rc.h.cmsg_type, LX_SCM_CREDENTIALS);
    LT_EQ(rc.u.pid, T->proc->pid);
    LT_EQ(rc.u.uid, T->proc->cred.euid);
    /* without SO_PASSCRED no credentials arrive */
    LT_EQ(SC(T, write, sv[1], P("x"), 1), 1);
    memset(&rc, 0, sizeof rc);
    rm.msg_controllen = sizeof rc;
    LT_EQ(SC(T, recvmsg, sv[0], P(&rm), 0), 1);
    LT_EQ(rm.msg_controllen, 0);
    /* a control buffer too small gives MSG_CTRUNC */
    LT_EQ(SC(T, write, sv[0], P("y"), 1), 1);
    rm.msg_controllen = 8;
    LT_EQ(SC(T, recvmsg, sv[1], P(&rm), 0), 1);
    LT_EQ(rm.msg_flags & LX_MSG_CTRUNC, LX_MSG_CTRUNC);
    LT_EQ(rm.msg_controllen, 0);
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* ---- options ------------------------------------------------------------------------------------- */

LT_TEST(sockopts)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    int32_t v = 1, l = 4;
    /* booleans read back as 0 or 1 */
    v = 5;
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_KEEPALIVE, P(&v), 4), 0);
    v = 0;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_KEEPALIVE, P(&v), P(&l)), 0);
    LT_EQ(v, 1);
    /* SO_RCVBUF: Linux doubles the value (sock(7)) */
    v = 65536;
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVBUF, P(&v), 4), 0);
    v = 0; l = 4;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVBUF, P(&v), P(&l)), 0);
    LT_EQ(v, 131072);
    /* SO_ERROR is 0 and is cleared by reading */
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_ERROR, P(&v), P(&l)), 0);
    LT_EQ(v, 0);
    /* SO_RCVTIMEO is kept in the object and makes a blocking read return EAGAIN after the delay */
    int64_t tv[2] = { 0, 50000 };
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVTIMEO, P(tv), 16), 0);
    int64_t tv2[2] = { 9, 9 };
    int32_t tl = 16;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVTIMEO, P(tv2), P(&tl)), 0);
    LT_EQ(tv2[0], 0);
    LT_EQ(tv2[1], 50000);
    int64_t t0 = lkd_host_mono_ns();
    char c;
    LT_ERRNO(SC(T, read, sv[0], P(&c), 1), LX_EAGAIN);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 40000000 && dt < 2000000000);
    int64_t bad[2] = { 0, 2000000 };
    LT_ERRNO(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVTIMEO, P(bad), 16), LX_EDOM);   /* sock_set_timeout */
    LT_ERRNO(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_RCVTIMEO, P(bad), 4), LX_EINVAL);
    /* SO_LINGER round trip */
    struct lx_linger lg = { 1, 3 };
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_LINGER, P(&lg), 8), 0);
    struct lx_linger lg2 = { 0, 0 };
    l = 8;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_LINGER, P(&lg2), P(&l)), 0);
    LT_EQ(lg2.l_onoff, 1);
    LT_EQ(lg2.l_linger, 3);
    /* unknown option: ENOPROTOOPT; read-only options: ENOPROTOOPT */
    LT_ERRNO(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, 4242, P(&v), P(&l)), LX_ENOPROTOOPT);
    LT_ERRNO(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_TYPE, P(&v), 4), LX_ENOPROTOOPT);
    /* SO_PASSCRED is a flag read back as set */
    v = 1;
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_PASSCRED, P(&v), 4), 0);
    v = 0; l = 4;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_PASSCRED, P(&v), P(&l)), 0);
    LT_EQ(v, 1);
    v = 0;
    LT_EQ(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_PASSCRED, P(&v), 4), 0);
    /* SO_PEERCRED: the peer of a socketpair is this process: its pid and guest ids */
    struct lx_ucred uc = { 0 };
    l = sizeof uc;
    LT_EQ(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_PEERCRED, P(&uc), P(&l)), 0);
    LT_EQ(l, 12);
    LT_EQ(uc.pid, T->proc->pid);
    LT_EQ(uc.uid, T->proc->cred.euid);
    /* wrong sizes */
    l = -1;
    LT_ERRNO(SC(T, getsockopt, sv[0], LX_SOL_SOCKET, LX_SO_TYPE, P(&v), P(&l)), LX_EINVAL);
    LT_ERRNO(SC(T, setsockopt, sv[0], LX_SOL_SOCKET, LX_SO_KEEPALIVE, P(&v), 2), LX_EINVAL);
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

/* ---- poll on sockets ----------------------------------------------------------------------------- */

LT_TEST(poll_states)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    struct lx_pollfd pf = { sv[0], LX_POLLIN | LX_POLLOUT | LX_POLLRDHUP, 0 };
    /* connected, empty: writable only (unix_poll) */
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_EQ(pf.revents, LX_POLLOUT);
    LT_EQ(SC(T, write, sv[1], P("d"), 1), 1);
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_EQ(pf.revents, LX_POLLIN | LX_POLLOUT);
    char c;
    SC(T, read, sv[0], P(&c), 1);
    /* peer shutdown(SHUT_WR): readable (EOF) with RDHUP, not HUP (both directions are not shut) */
    SC(T, shutdown, sv[1], 1);
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_EQ(pf.revents, LX_POLLIN | LX_POLLOUT | LX_POLLRDHUP);
    /* peer closed: HUP as well (sk_shutdown == SHUTDOWN_MASK) */
    SC(T, close, sv[1]);
    LT_EQ(SC(T, poll, P(&pf), 1, 0), 1);
    LT_CHECK((pf.revents & (LX_POLLIN | LX_POLLHUP | LX_POLLRDHUP)) == (LX_POLLIN | LX_POLLHUP | LX_POLLRDHUP));
    SC(T, close, sv[0]);
    /* listening socket: readable when a connection waits */
    int srv = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    SOCKADDR_UN(a, "/poll.sock");
    LT_EQ(SC(T, bind, srv, P(&a), UNLEN("/poll.sock")), 0);
    LT_EQ(SC(T, listen, srv, 4), 0);
    struct lx_pollfd lp = { srv, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&lp), 1, 0), 0);
    int cl = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, connect, cl, P(&a), UNLEN("/poll.sock")), 0);
    LT_EQ(SC(T, poll, P(&lp), 1, 100), 1);
    LT_EQ(lp.revents, LX_POLLIN);
    int acc = (int)SC(T, accept, srv, 0, 0);
    LT_EQ(SC(T, poll, P(&lp), 1, 0), 0);
    SC(T, close, acc); SC(T, close, cl); SC(T, close, srv);
    /* a fresh unconnected stream socket: OUT and HUP (tcp_poll/unix_poll for TCP_CLOSE) */
    int fresh = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_STREAM, 0);
    struct lx_pollfd fp = { fresh, LX_POLLIN | LX_POLLOUT, 0 };
    LT_EQ(SC(T, poll, P(&fp), 1, 0), 1);
    LT_EQ(fp.revents, LX_POLLOUT | LX_POLLHUP);
    SC(T, close, fresh);
}

struct hlp { lkd_task *t; int fd; int delay_ms; const char *data; };
static void *writer_thread(void *a)
{
    struct hlp *h = a;
    lkd_set_current(h->t);
    usleep((useconds_t)h->delay_ms * 1000);
    SC(h->t, write, h->fd, P(h->data), strlen(h->data));
    return NULL;
}

LT_TEST(blocking_read_wakes)
{
    int32_t sv[2];
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(sv)), 0);
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    struct hlp h = { th, sv[1], 50, "late" };
    pthread_t pt;
    pthread_create(&pt, NULL, writer_thread, &h);
    char b[8] = { 0 };
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, read, sv[0], P(b), sizeof b), 4);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 30000000);
    pthread_join(pt, NULL);
    /* blocking accept and connect-less recvmsg are interruptible: ERESTARTSYS through lkd_dispatch */
    atomic_fetch_or(&T->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(T);
    lkd_sysreq rq = { .nr = LKD_NR_read, .a = { (uint64_t)sv[0], P(b), sizeof b } };
    LT_ERRNO(lkd_dispatch(T, &rq), LX_ERESTARTSYS);
    atomic_fetch_and(&T->flags, ~(uint32_t)(LKD_TF_SIGPENDING | LKD_TF_FATAL));
    char junk[64];
    while (read(T->wake_rd, junk, sizeof junk) > 0) { }
    SC(T, close, sv[0]); SC(T, close, sv[1]);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netunix");
    put_file(ROOT, "plain", "x");
    put_dir(ROOT, "sub");
    put_file(ROOT, "sub/inner.txt", "hello");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));   /* SIGPIPE stays pending instead of interrupting waits */
    LT_RUN(socket_creation_errors);
    LT_RUN(socketpair_stream);
    LT_RUN(socketpair_dgram);
    LT_RUN(bind_listen_accept_connect);
    LT_RUN(dgram_named);
    LT_RUN(abstract_namespace);
    LT_RUN(long_path_bind);
    LT_RUN(scm_rights_pipe_and_file);
    LT_RUN(scm_rights_truncated);
    LT_RUN(scm_rights_socket_and_eventfd);
    LT_RUN(scm_rights_across_fork);
    LT_RUN(scm_rights_directory_across_fork);
    LT_RUN(scm_credentials);
    LT_RUN(sockopts);
    LT_RUN(poll_states);
    LT_RUN(blocking_read_wakes);
    rm_rf(ROOT);
    return lt_finish();
}
