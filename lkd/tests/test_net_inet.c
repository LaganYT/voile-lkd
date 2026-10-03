/* test_net_inet.c: AF_INET and AF_INET6 sockets on the loopback. Linux references: tcp(7), udp(7),
 * ip(7), ipv6(7), socket(7), connect(2), net/ipv4/tcp.c, net/ipv4/af_inet.c, net/core/sock.c of v6.12.
 * The DNS case follows what glibc's resolver does: UDP socket, connect, send, poll, recv, SO_ERROR. */
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

static struct lx_sockaddr_in loop4(uint16_t port_host)
{
    struct lx_sockaddr_in a = { .sin_family = LX_AF_INET, .sin_port = htons(port_host), .sin_addr = htonl(INADDR_LOOPBACK) };
    return a;
}

static uint16_t port_of(int fd)
{
    struct lx_sockaddr_in a;
    int32_t l = sizeof a;
    if (SC(T, getsockname, fd, P(&a), P(&l)) != 0) return 0;
    return ntohs(a.sin_port);
}

static int tcp_listener(uint16_t *port)
{
    int s = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_in a = loop4(0);
    if (SC(T, bind, s, P(&a), sizeof a) != 0 || SC(T, listen, s, 8) != 0) return -1;
    *port = port_of(s);
    return s;
}

LT_TEST(tcp_roundtrip)
{
    uint16_t port;
    int srv = tcp_listener(&port);
    LT_CHECK(srv >= 0 && port != 0);
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_in a = loop4(port);
    LT_EQ(SC(T, connect, c, P(&a), sizeof a), 0);
    struct lx_sockaddr_in pa;
    int32_t pl = sizeof pa;
    int acc = (int)SC(T, accept, srv, P(&pa), P(&pl));
    LT_CHECK(acc >= 0);
    LT_EQ(pl, 16);
    LT_EQ(pa.sin_family, LX_AF_INET);                              /* Linux layout: u16 family, no sa_len */
    LT_EQ(pa.sin_addr, htonl(INADDR_LOOPBACK));
    LT_EQ(ntohs(pa.sin_port), port_of(c));
    LT_EQ(SC(T, write, c, P("GET /"), 5), 5);
    char b[16] = { 0 };
    LT_EQ(SC(T, recvfrom, acc, P(b), 3, LX_MSG_PEEK, 0, 0), 3);     /* MSG_PEEK leaves the data */
    LT_EQ(SC(T, recvfrom, acc, P(b), sizeof b, 0, 0, 0), 5);
    LT_STREQ(b, "GET /");
    /* getpeername / getsockname are the mirror image */
    struct lx_sockaddr_in me, peer;
    int32_t l1 = sizeof me, l2 = sizeof peer;
    LT_EQ(SC(T, getsockname, c, P(&me), P(&l1)), 0);
    LT_EQ(SC(T, getpeername, c, P(&peer), P(&l2)), 0);
    LT_EQ(ntohs(peer.sin_port), port);
    /* MSG_WAITALL waits for the full amount (recv(2)) */
    SC(T, write, c, P("12"), 2);
    SC(T, write, c, P("345"), 3);
    char w[8] = { 0 };
    LT_EQ(SC(T, recvfrom, acc, P(w), 5, LX_MSG_WAITALL, 0, 0), 5);
    LT_STREQ(w, "12345");
    /* shutdown(WR) gives EOF on the other side; the other direction still works */
    LT_EQ(SC(T, shutdown, c, 1), 0);
    LT_EQ(SC(T, read, acc, P(b), sizeof b), 0);
    LT_EQ(SC(T, write, acc, P("bye"), 3), 3);
    LT_EQ(SC(T, read, c, P(b), sizeof b), 3);
    /* SO_TYPE / SO_PROTOCOL / SO_DOMAIN */
    int32_t v = 0, vl = 4;
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_PROTOCOL, P(&v), P(&vl)), 0);
    LT_EQ(v, LX_IPPROTO_TCP);
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_DOMAIN, P(&v), P(&vl)), 0);
    LT_EQ(v, LX_AF_INET);
    SC(T, close, acc); SC(T, close, c); SC(T, close, srv);
}

LT_TEST(connect_refused_nonblocking)
{
    /* a port nobody listens on: bind a listener, read its port, close it */
    uint16_t port;
    int srv = tcp_listener(&port);
    SC(T, close, srv);
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM | LX_SOCK_NONBLOCK, 0);
    struct lx_sockaddr_in a = loop4(port);
    int64_t r = SC(T, connect, c, P(&a), sizeof a);
    /* connect(2): EINPROGRESS on a nonblocking socket (loopback refusal can already be pending) */
    LT_CHECK(r == -LX_EINPROGRESS || r == -LX_ECONNREFUSED);
    struct lx_pollfd pf = { c, LX_POLLOUT, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 2000), 1);
    LT_CHECK(pf.revents & LX_POLLERR);
    LT_CHECK(pf.revents & LX_POLLHUP);
    /* tcp(7): the error is read once with SO_ERROR */
    int32_t e = 0, el = 4;
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_ERROR, P(&e), P(&el)), 0);
    LT_EQ(e, LX_ECONNREFUSED);
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_ERROR, P(&e), P(&el)), 0);
    LT_EQ(e, 0);
    SC(T, close, c);
    /* the blocking form returns the error directly */
    int c2 = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    LT_ERRNO(SC(T, connect, c2, P(&a), sizeof a), LX_ECONNREFUSED);
    SC(T, close, c2);
}

LT_TEST(nonblocking_connect_success)
{
    uint16_t port;
    int srv = tcp_listener(&port);
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM | LX_SOCK_NONBLOCK, 0);
    struct lx_sockaddr_in a = loop4(port);
    int64_t r = SC(T, connect, c, P(&a), sizeof a);
    LT_CHECK(r == 0 || r == -LX_EINPROGRESS);
    struct lx_pollfd pf = { c, LX_POLLOUT, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 2000), 1);
    LT_EQ(pf.revents, LX_POLLOUT);
    int32_t e = 5, el = 4;
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_ERROR, P(&e), P(&el)), 0);
    LT_EQ(e, 0);
    /* a second connect on a connected socket: EISCONN */
    LT_ERRNO(SC(T, connect, c, P(&a), sizeof a), LX_EISCONN);
    int acc = (int)SC(T, accept4, srv, 0, 0, LX_SOCK_NONBLOCK);
    LT_CHECK(acc >= 0);
    LT_EQ(SC(T, fcntl, acc, LX_F_GETFL) & LX_O_NONBLOCK, LX_O_NONBLOCK);
    char b[4];
    LT_ERRNO(SC(T, read, acc, P(b), 4), LX_EAGAIN);
    SC(T, close, acc); SC(T, close, c); SC(T, close, srv);
}

LT_TEST(bind_rules)
{
    uint16_t port;
    int srv = tcp_listener(&port);
    int s2 = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_in a = loop4(port);
    LT_ERRNO(SC(T, bind, s2, P(&a), sizeof a), LX_EADDRINUSE);
    /* SO_REUSEADDR does not allow two listeners (tcp(7)) */
    int32_t on = 1;
    LT_EQ(SC(T, setsockopt, s2, LX_SOL_SOCKET, LX_SO_REUSEADDR, P(&on), 4), 0);
    LT_ERRNO(SC(T, bind, s2, P(&a), sizeof a), LX_EADDRINUSE);
    /* wrong family and short address */
    struct lx_sockaddr_in6 a6 = { .sin6_family = LX_AF_INET6 };
    LT_ERRNO(SC(T, bind, s2, P(&a6), sizeof a6), LX_EAFNOSUPPORT);
    LT_ERRNO(SC(T, bind, s2, P(&a), 8), LX_EINVAL);
    /* ports below 1024 need CAP_NET_BIND_SERVICE */
    T->proc->cred.euid = 1000;
    struct lx_sockaddr_in low = loop4(80);
    LT_ERRNO(SC(T, bind, s2, P(&low), sizeof low), LX_EACCES);
    /* getpeername on an unconnected socket */
    struct lx_sockaddr_in pa;
    int32_t pl = sizeof pa;
    LT_ERRNO(SC(T, getpeername, s2, P(&pa), P(&pl)), LX_ENOTCONN);
    /* listen on a datagram socket */
    int u = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    LT_ERRNO(SC(T, listen, u, 1), LX_EOPNOTSUPP);
    LT_ERRNO(SC(T, accept, u, 0, 0), LX_EOPNOTSUPP);
    SC(T, close, u); SC(T, close, s2); SC(T, close, srv);
}

LT_TEST(udp_datagrams)
{
    int a = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    int b = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    struct lx_sockaddr_in aa = loop4(0);
    LT_EQ(SC(T, bind, a, P(&aa), sizeof aa), 0);
    uint16_t pa = port_of(a);
    struct lx_sockaddr_in dst = loop4(pa);
    LT_EQ(SC(T, sendto, b, P("hello"), 5, 0, P(&dst), sizeof dst), 5);
    char buf[16] = { 0 };
    struct lx_sockaddr_in from;
    int32_t fl = sizeof from;
    LT_EQ(SC(T, recvfrom, a, P(buf), sizeof buf, 0, P(&from), P(&fl)), 5);
    LT_STREQ(buf, "hello");
    LT_EQ(fl, 16);
    LT_EQ(from.sin_family, LX_AF_INET);
    LT_EQ(ntohs(from.sin_port), port_of(b));                       /* b was bound implicitly by sendto */
    /* a datagram larger than the buffer is truncated; MSG_TRUNC returns the real size */
    LT_EQ(SC(T, sendto, b, P("0123456789"), 10, 0, P(&dst), sizeof dst), 10);
    LT_EQ(SC(T, recvfrom, a, P(buf), 4, LX_MSG_TRUNC, 0, 0), 10);
    /* connect() on UDP sets the default peer; send without address works; getpeername works */
    LT_EQ(SC(T, connect, b, P(&dst), sizeof dst), 0);
    LT_EQ(SC(T, write, b, P("conn"), 4), 4);
    LT_EQ(SC(T, read, a, P(buf), sizeof buf), 4);
    struct lx_sockaddr_in peer;
    int32_t pl = sizeof peer;
    LT_EQ(SC(T, getpeername, b, P(&peer), P(&pl)), 0);
    LT_EQ(ntohs(peer.sin_port), pa);
    /* connect(AF_UNSPEC) dissolves the association (connect(2)) */
    struct lx_sockaddr_in un = { .sin_family = LX_AF_UNSPEC };
    LT_EQ(SC(T, connect, b, P(&un), sizeof un), 0);
    pl = sizeof peer;
    LT_ERRNO(SC(T, getpeername, b, P(&peer), P(&pl)), LX_ENOTCONN);
    /* nonblocking recv with nothing queued */
    SC(T, fcntl, a, LX_F_SETFL, LX_O_NONBLOCK);
    LT_ERRNO(SC(T, recvfrom, a, P(buf), sizeof buf, 0, 0, 0), LX_EAGAIN);
    /* sendmmsg / recvmmsg */
    struct lx_iovec iov[2] = { { P("m1"), 2 }, { P("m2xx"), 4 } };
    struct lx_mmsghdr mm[2];
    memset(mm, 0, sizeof mm);
    for (int i = 0; i < 2; i++) {
        mm[i].msg_hdr.msg_name = P(&dst);
        mm[i].msg_hdr.msg_namelen = sizeof dst;
        mm[i].msg_hdr.msg_iov = P(&iov[i]);
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    LT_EQ(SC(T, sendmmsg, b, P(mm), 2, 0), 2);
    LT_EQ(mm[0].msg_len, 2);
    LT_EQ(mm[1].msg_len, 4);
    struct lx_mmsghdr rm[2];
    char r0[8] = { 0 }, r1[8] = { 0 };
    struct lx_iovec riov[2] = { { P(r0), sizeof r0 }, { P(r1), sizeof r1 } };
    memset(rm, 0, sizeof rm);
    for (int i = 0; i < 2; i++) { rm[i].msg_hdr.msg_iov = P(&riov[i]); rm[i].msg_hdr.msg_iovlen = 1; }
    usleep(20000);
    LT_EQ(SC(T, recvmmsg, a, P(rm), 2, LX_MSG_DONTWAIT, 0), 2);
    LT_EQ(rm[0].msg_len, 2);
    LT_EQ(rm[1].msg_len, 4);
    LT_MEMEQ(r0, "m1", 2);
    LT_MEMEQ(r1, "m2xx", 4);
    SC(T, close, a); SC(T, close, b);
}

/* sendmsg/recvmsg with a name and a gather list. */
LT_TEST(sendmsg_recvmsg_iov)
{
    int a = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    int b = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    struct lx_sockaddr_in aa = loop4(0);
    LT_EQ(SC(T, bind, a, P(&aa), sizeof aa), 0);
    struct lx_sockaddr_in dst = loop4(port_of(a));
    struct lx_iovec iov[3] = { { P("ab"), 2 }, { P("cde"), 3 }, { P("f"), 1 } };
    struct lx_msghdr m = { .msg_name = P(&dst), .msg_namelen = sizeof dst, .msg_iov = P(iov), .msg_iovlen = 3 };
    LT_EQ(SC(T, sendmsg, b, P(&m), 0), 6);
    char x[4] = { 0 }, y[8] = { 0 };
    struct lx_iovec riov[2] = { { P(x), 4 }, { P(y), 8 } };
    struct lx_sockaddr_in from;
    struct lx_msghdr rm = { .msg_name = P(&from), .msg_namelen = sizeof from, .msg_iov = P(riov), .msg_iovlen = 2 };
    LT_EQ(SC(T, recvmsg, a, P(&rm), 0), 6);
    LT_MEMEQ(x, "abcd", 4);
    LT_MEMEQ(y, "ef", 2);
    LT_EQ(rm.msg_namelen, 16);
    LT_EQ(rm.msg_flags, 0);
    /* truncation sets MSG_TRUNC in msg_flags */
    LT_EQ(SC(T, sendmsg, b, P(&m), 0), 6);
    struct lx_iovec small = { P(x), 2 };
    struct lx_msghdr tm = { .msg_iov = P(&small), .msg_iovlen = 1 };
    LT_EQ(SC(T, recvmsg, a, P(&tm), 0), 2);
    LT_EQ(tm.msg_flags & LX_MSG_TRUNC, LX_MSG_TRUNC);
    SC(T, close, a); SC(T, close, b);
}

struct dns { int fd; };
static void *fake_nameserver(void *p)
{
    struct dns *d = p;
    lkd_task *ts = fake_make_thread(T);
    lkd_set_current(ts);
    char q[512];
    struct lx_sockaddr_in from;
    int32_t fl = sizeof from;
    int64_t n = SC(ts, recvfrom, d->fd, P(q), sizeof q, 0, P(&from), P(&fl));
    if (n > 0) {
        q[2] |= 0x80;                                              /* QR: this is a response */
        SC(ts, sendto, d->fd, P(q), (uint64_t)n, 0, P(&from), (uint64_t)fl);
    }
    return NULL;
}

/* What glibc's stub resolver does against /etc/resolv.conf's nameserver (resolv/res_send.c): a
 * non-blocking UDP socket, connect, send, poll for the answer, recv, SO_ERROR. */
LT_TEST(dns_style_exchange)
{
    int ns = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    struct lx_sockaddr_in na = loop4(0);
    LT_EQ(SC(T, bind, ns, P(&na), sizeof na), 0);
    struct dns d = { ns };
    pthread_t pt;
    pthread_create(&pt, NULL, fake_nameserver, &d);
    lkd_set_current(T);
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM | LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC, 0);
    struct lx_sockaddr_in dst = loop4(port_of(ns));
    LT_EQ(SC(T, connect, c, P(&dst), sizeof dst), 0);
    uint8_t query[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 3, 'w', 'w', 'w', 0, 0, 1, 0, 1 };
    LT_EQ(SC(T, sendto, c, P(query), sizeof query, LX_MSG_NOSIGNAL, 0, 0), sizeof query);
    struct lx_pollfd pf = { c, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&pf), 1, 3000), 1);
    LT_EQ(pf.revents, LX_POLLIN);
    uint8_t ans[512];
    LT_EQ(SC(T, recvfrom, c, P(ans), sizeof ans, 0, 0, 0), sizeof query);
    LT_EQ(ans[0], 0x12);
    LT_EQ(ans[2] & 0x80, 0x80);
    int32_t e = 9, el = 4;
    LT_EQ(SC(T, getsockopt, c, LX_SOL_SOCKET, LX_SO_ERROR, P(&e), P(&el)), 0);
    LT_EQ(e, 0);
    pthread_join(pt, NULL);
    /* the nameserver is gone: a query to a closed port gets ECONNREFUSED through SO_ERROR/recv (ICMP) */
    SC(T, close, ns);
    LT_EQ(SC(T, sendto, c, P(query), sizeof query, 0, 0, 0), sizeof query);
    pf.revents = 0;
    int pr = (int)SC(T, poll, P(&pf), 1, 500);
    LT_CHECK(pr == 0 || pr == 1);                                  /* the host may not report ICMP errors on UDP */
    SC(T, close, c);
}

/* IP_RECVERR and IPV6_RECVERR (request wine-4): glibc's resolver (send_dg) sets IP_RECVERR on its UDP
 * socket and gives up on ENOPROTOOPT. The flag reads back, MSG_ERRQUEUE answers EAGAIN on an empty queue,
 * and an error the host reports for the connected peer (ECONNREFUSED from the closed port) is queued:
 * one control message IPPROTO_IP/IP_RECVERR with ee_errno ECONNREFUSED, SO_EE_ORIGIN_LOCAL (the host gives no
 * ICMP details), msg_flags MSG_ERRQUEUE (ip(7) IP_RECVERR, net/ipv4/ip_sockglue.c ip_recv_error). */
LT_TEST(ip_recverr)
{
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM | LX_SOCK_NONBLOCK, 0);
    int32_t one = 1, v = 7, vl = 4;
    LT_EQ(SC(T, getsockopt, c, LX_IPPROTO_IP, LX_IP_RECVERR, P(&v), P(&vl)), 0);
    LT_EQ(v, 0);
    LT_EQ(SC(T, setsockopt, c, LX_IPPROTO_IP, LX_IP_RECVERR, P(&one), 4), 0);
    LT_EQ(SC(T, getsockopt, c, LX_IPPROTO_IP, LX_IP_RECVERR, P(&v), P(&vl)), 0);
    LT_EQ(v, 1);
    struct { struct lx_cmsghdr h; uint8_t d[64]; } ctl;
    char buf[64];
    struct lx_iovec iov = { P(buf), sizeof buf };
    struct lx_sockaddr_in from;
    struct lx_msghdr m = { .msg_name = P(&from), .msg_namelen = sizeof from, .msg_iov = P(&iov), .msg_iovlen = 1,
                           .msg_control = P(&ctl), .msg_controllen = sizeof ctl };
    LT_ERRNO(SC(T, recvmsg, c, P(&m), LX_MSG_ERRQUEUE), LX_EAGAIN);              /* empty queue */
    /* a closed port: bind a socket, note its port, close it */
    int z = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    struct lx_sockaddr_in za = loop4(0);
    LT_EQ(SC(T, bind, z, P(&za), sizeof za), 0);
    struct lx_sockaddr_in dst = loop4(port_of(z));
    SC(T, close, z);
    LT_EQ(SC(T, connect, c, P(&dst), sizeof dst), 0);
    int refused = 0;
    for (int i = 0; i < 20 && !refused; i++) {
        (void)SC(T, sendto, c, P("q"), 1, 0, 0, 0);
        usleep(20000);
        int64_t r = SC(T, recvfrom, c, P(buf), sizeof buf, 0, 0, 0);
        refused = r == -LX_ECONNREFUSED;
    }
    LT_CHECK(refused);                                                           /* MESURE: the macOS loopback reports it */
    memset(&ctl, 0, sizeof ctl);
    m.msg_namelen = sizeof from;
    m.msg_controllen = sizeof ctl;
    LT_EQ(SC(T, recvmsg, c, P(&m), LX_MSG_ERRQUEUE), 0);
    LT_EQ(m.msg_flags & LX_MSG_ERRQUEUE, LX_MSG_ERRQUEUE);
    LT_EQ(ctl.h.cmsg_level, LX_IPPROTO_IP);
    LT_EQ(ctl.h.cmsg_type, LX_IP_RECVERR);
    struct lx_sock_extended_err ee;
    memcpy(&ee, ctl.d, sizeof ee);
    LT_EQ(ee.ee_errno, LX_ECONNREFUSED);
    LT_EQ(ee.ee_origin, LX_SO_EE_ORIGIN_LOCAL);
    LT_EQ(m.msg_namelen, sizeof from);
    LT_EQ(from.sin_port, dst.sin_port);
    LT_ERRNO(SC(T, recvmsg, c, P(&m), LX_MSG_ERRQUEUE), LX_EAGAIN);              /* consumed */
    SC(T, close, c);
    /* IPv6 has its own flag; a unix socket has no IP level */
    int c6 = (int)SC(T, socket, LX_AF_INET6, LX_SOCK_DGRAM, 0);
    LT_EQ(SC(T, setsockopt, c6, LX_IPPROTO_IPV6, LX_IPV6_RECVERR, P(&one), 4), 0);
    LT_EQ(SC(T, getsockopt, c6, LX_IPPROTO_IPV6, LX_IPV6_RECVERR, P(&v), P(&vl)), 0);
    LT_EQ(v, 1);
    SC(T, close, c6);
    int u = (int)SC(T, socket, LX_AF_UNIX, LX_SOCK_DGRAM, 0);
    LT_ERRNO(SC(T, setsockopt, u, LX_IPPROTO_IP, LX_IP_RECVERR, P(&one), 4), LX_ENOPROTOOPT);
    SC(T, close, u);
}

LT_TEST(tcp_options)
{
    int s = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    int32_t v = 1, l = 4;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_TCP, LX_TCP_NODELAY, P(&v), 4), 0);
    v = 0;
    LT_EQ(SC(T, getsockopt, s, LX_IPPROTO_TCP, LX_TCP_NODELAY, P(&v), P(&l)), 0);
    LT_EQ(v, 1);
    v = 30;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_TCP, LX_TCP_KEEPIDLE, P(&v), 4), 0);
    v = 0;
    LT_EQ(SC(T, getsockopt, s, LX_IPPROTO_TCP, LX_TCP_KEEPIDLE, P(&v), P(&l)), 0);
    LT_EQ(v, 30);
    v = 5;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_TCP, LX_TCP_KEEPINTVL, P(&v), 4), 0);
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_TCP, LX_TCP_KEEPCNT, P(&v), 4), 0);
    v = 1;
    LT_EQ(SC(T, setsockopt, s, LX_SOL_SOCKET, LX_SO_REUSEPORT, P(&v), 4), 0);
    LT_EQ(SC(T, setsockopt, s, LX_SOL_SOCKET, LX_SO_BROADCAST, P(&v), 4), 0);
    v = 0;
    LT_EQ(SC(T, getsockopt, s, LX_SOL_SOCKET, LX_SO_REUSEPORT, P(&v), P(&l)), 0);
    LT_EQ(v, 1);
    v = 0x10;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_IP, LX_IP_TOS, P(&v), 4), 0);
    v = 0;
    LT_EQ(SC(T, getsockopt, s, LX_IPPROTO_IP, LX_IP_TOS, P(&v), P(&l)), 0);
    LT_EQ(v, 0x10);
    v = 64;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_IP, LX_IP_TTL, P(&v), 4), 0);
    /* SNDBUF doubling and the cap at wmem_max*2 */
    v = 1 << 30;
    LT_EQ(SC(T, setsockopt, s, LX_SOL_SOCKET, LX_SO_SNDBUF, P(&v), 4), 0);
    LT_EQ(SC(T, getsockopt, s, LX_SOL_SOCKET, LX_SO_SNDBUF, P(&v), P(&l)), 0);
    LT_EQ(v, 425984);
    /* nonexistent options */
    LT_ERRNO(SC(T, setsockopt, s, LX_IPPROTO_TCP, 9999, P(&v), 4), LX_ENOPROTOOPT);
    LT_ERRNO(SC(T, setsockopt, s, 99, 1, P(&v), 4), LX_ENOPROTOOPT);
    SC(T, close, s);
}

LT_TEST(ipv6_loopback)
{
    int s = (int)SC(T, socket, LX_AF_INET6, LX_SOCK_STREAM, 0);
    LT_CHECK(s >= 0);
    struct lx_sockaddr_in6 a = { .sin6_family = LX_AF_INET6 };
    a.sin6_addr[15] = 1;                                          /* ::1 */
    int32_t on = 1;
    LT_EQ(SC(T, setsockopt, s, LX_IPPROTO_IPV6, LX_IPV6_V6ONLY, P(&on), 4), 0);
    LT_EQ(SC(T, bind, s, P(&a), sizeof a), 0);
    LT_EQ(SC(T, listen, s, 4), 0);
    struct lx_sockaddr_in6 got;
    int32_t gl = sizeof got;
    LT_EQ(SC(T, getsockname, s, P(&got), P(&gl)), 0);
    LT_EQ(gl, 28);
    LT_EQ(got.sin6_family, LX_AF_INET6);
    LT_EQ(got.sin6_addr[15], 1);
    LT_CHECK(got.sin6_port != 0);
    int c = (int)SC(T, socket, LX_AF_INET6, LX_SOCK_STREAM, 0);
    LT_EQ(SC(T, connect, c, P(&got), sizeof got), 0);
    int acc = (int)SC(T, accept, s, 0, 0);
    LT_CHECK(acc >= 0);
    LT_EQ(SC(T, write, c, P("v6"), 2), 2);
    char b[4] = { 0 };
    LT_EQ(SC(T, read, acc, P(b), 4), 2);
    LT_STREQ(b, "v6");
    int32_t v = 0, vl = 4;
    LT_EQ(SC(T, getsockopt, s, LX_IPPROTO_IPV6, LX_IPV6_V6ONLY, P(&v), P(&vl)), 0);
    LT_EQ(v, 1);
    SC(T, close, acc); SC(T, close, c); SC(T, close, s);
}

/* poll/select on TCP states: SYN pending is not writable, listener readable on connection, peer FIN gives
 * IN|RDHUP, peer reset gives ERR|HUP (tcp_poll). */
LT_TEST(tcp_poll_states)
{
    uint16_t port;
    int srv = tcp_listener(&port);
    struct lx_pollfd lp = { srv, LX_POLLIN, 0 };
    LT_EQ(SC(T, poll, P(&lp), 1, 0), 0);
    int c = (int)SC(T, socket, LX_AF_INET, LX_SOCK_STREAM, 0);
    struct lx_sockaddr_in a = loop4(port);
    LT_EQ(SC(T, connect, c, P(&a), sizeof a), 0);
    LT_EQ(SC(T, poll, P(&lp), 1, 1000), 1);
    LT_EQ(lp.revents, LX_POLLIN);
    int acc = (int)SC(T, accept, srv, 0, 0);
    struct lx_pollfd cp = { c, LX_POLLIN | LX_POLLOUT | LX_POLLRDHUP, 0 };
    LT_EQ(SC(T, poll, P(&cp), 1, 0), 1);
    LT_EQ(cp.revents, LX_POLLOUT);
    SC(T, shutdown, acc, 1);
    usleep(20000);
    LT_EQ(SC(T, poll, P(&cp), 1, 1000), 1);
    LT_EQ(cp.revents, LX_POLLIN | LX_POLLOUT | LX_POLLRDHUP);
    char b[4];
    LT_EQ(SC(T, read, c, P(b), 4), 0);
    /* select(2) agrees: readable (EOF) and writable */
    uint64_t rd = 1ull << c, wr = 1ull << c;
    struct { int64_t s, us; } tv = { 1, 0 };
    LT_EQ(SC(T, select, c + 1, P(&rd), P(&wr), 0, P(&tv)), 2);
    LT_EQ(rd, 1ull << c);
    LT_EQ(wr, 1ull << c);
    SC(T, close, acc); SC(T, close, c); SC(T, close, srv);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netinet");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    LT_RUN(tcp_roundtrip);
    LT_RUN(connect_refused_nonblocking);
    LT_RUN(nonblocking_connect_success);
    LT_RUN(bind_rules);
    LT_RUN(udp_datagrams);
    LT_RUN(sendmsg_recvmsg_iov);
    LT_RUN(dns_style_exchange);
    LT_RUN(ip_recverr);
    LT_RUN(tcp_options);
    LT_RUN(ipv6_loopback);
    LT_RUN(tcp_poll_states);
    rm_rf(ROOT);
    return lt_finish();
}
