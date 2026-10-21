/* socket.c: socket objects and the connection syscalls (socket, socketpair, bind, listen, accept,
 * accept4, connect, getsockname, getpeername, shutdown). Models net/socket.c, net/unix/af_unix.c,
 * net/ipv4/af_inet.c and net/ipv4/tcp.c (tcp_poll) of Linux 6.12.
 *
 * Every socket is a host socket. The host fd of a socket LKD creates is permanently O_NONBLOCK (the
 * guest O_NONBLOCK lives in the object, like LKD pipes) and SO_NOSIGPIPE (SIGPIPE is sent to the guest
 * thread by LKD). A socket received through SCM_RIGHTS keeps the host flags of its sender: LKD then
 * passes MSG_DONTWAIT on every call instead of changing the shared description.
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>

#include "net_priv.h"

#define LX_SOMAXCONN 4096          /* net.core.somaxconn default since 5.4 */
#define LX_SIOCATMARK 0x8905
#define LX_SIOCOUTQ 0x5411         /* TIOCOUTQ */
#define LX_SIOCINQ LX_FIONREAD

static _Atomic uint64_t g_sock_ino = 0x100000;

int net_install(lkd_task *t, lkd_fdobj *o, int cloexec)
{
    int fd = lkd_fd_alloc(t, o, 0, cloexec);
    lkd_fdobj_put(o);
    return fd;
}

/* ---- Object ------------------------------------------------------------------------------------ */

static int is_stream(const struct net_sock *sk) { return sk->type == LX_SOCK_STREAM || sk->type == LX_SOCK_SEQPACKET; }

int net_sock_wait(lkd_task *t, lkd_fdobj *o, short hev, int nonblock, int64_t timeo_ns)
{
    if (nonblock) return -LX_EAGAIN;
    int64_t deadline = timeo_ns > 0 ? lkd_host_mono_ns() + timeo_ns : -1;
    int r = lkd_block_wait_fd(t, o->host_fd, hev, deadline);
    if (r == 0) return -LX_EAGAIN;                    /* SO_*TIMEO expired: sock_rcvtimeo -> -EAGAIN */
    if (r == -LX_ERESTARTSYS && timeo_ns > 0) return -LX_EINTR; /* sock_intr_errno(timeo) */
    return r;
}

/* Linux revents of a socket. The host poll is translated with the measured macOS differences
 * (MESURE, docs/status/net.md): POLLHUP is reported for a half-closed or self-shut socket and a fresh
 * stream socket reports nothing, while tcp_poll/unix_poll report HUP only when both directions are shut
 * (sk_shutdown == SHUTDOWN_MASK) or the socket is closed, RDHUP when the receive side is shut, OUT when
 * the send side is shut, ERR while sk_err is set. */
static uint32_t sock_poll(lkd_fdobj *o, uint32_t want)
{
    struct net_sock *sk = o->priv;
    int hfd = o->host_fd;
    struct pollfd p = { .fd = hfd, .events = POLLIN | POLLOUT, .revents = 0 };
    if (want & LX_POLLPRI) p.events |= POLLPRI;
    if (poll(&p, 1, 0) < 0) return LX_POLLERR;
    short h = p.revents;
    if (!is_stream(sk)) {
        uint32_t r = net_host_revents_to_lx(h & ~POLLHUP);
        if (atomic_load(&sk->pending_err) || atomic_load(&sk->errq_errno)) r |= LX_POLLERR;
        if (atomic_load(&sk->shut_rd) && atomic_load(&sk->shut_wr)) r |= LX_POLLHUP;
        if (atomic_load(&sk->shut_rd)) r |= LX_POLLIN | LX_POLLRDNORM | LX_POLLRDHUP;
        return r;
    }
    if (atomic_load(&sk->listening)) return (h & POLLIN) ? (LX_POLLIN | LX_POLLRDNORM) : 0;
    uint32_t r = 0;
    int err = atomic_load(&sk->pending_err);
    if (atomic_load(&sk->connecting)) {
        if (!(h & (POLLOUT | POLLHUP | POLLERR))) return 0;   /* SYN_SENT: nothing yet */
        atomic_store(&sk->connecting, 0);
        if (!err) {
            int e = 0;
            socklen_t l = sizeof e;
            if (getsockopt(hfd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e)
                atomic_store(&sk->pending_err, err = lkd_errno_from_darwin(e));
        }
    }
    if (h & POLLIN) r |= LX_POLLIN | LX_POLLRDNORM;
    if ((h & POLLPRI) && !(h & POLLHUP)) r |= LX_POLLPRI;
    if (h & POLLOUT) r |= LX_POLLOUT | LX_POLLWRNORM;
    int rcv_shut = 0, snd_shut = 0;
    if (err) rcv_shut = snd_shut = 1;                 /* tcp_done/reset: sk_shutdown = SHUTDOWN_MASK */
    if (h & (POLLHUP | POLLERR)) {
        if (!err && (h & POLLERR)) {
            int e = 0;
            socklen_t l = sizeof e;
            if (getsockopt(hfd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e) {
                atomic_store(&sk->pending_err, err = lkd_errno_from_darwin(e));
                rcv_shut = snd_shut = 1;
            }
        }
        if (!snd_shut) {
            ssize_t s = send(hfd, "", 0, MSG_DONTWAIT);
            if (s < 0 && errno == EPIPE) snd_shut = 1;
            else if (s == 0) r |= LX_POLLOUT | LX_POLLWRNORM;   /* host reports HUP instead of OUT after a peer SHUT_WR */
        }
        if (!rcv_shut) {
            char c;
            ssize_t s = recv(hfd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (s == 0) rcv_shut = 1;
        }
    } else if (!(h & (POLLIN | POLLOUT))) {
        /* nothing reported: a stream socket that is neither connected nor connecting is TCP_CLOSE:
         * tcp_poll and unix_poll report EPOLLHUP and EPOLLOUT */
        ssize_t s = send(hfd, "", 0, MSG_DONTWAIT);
        if (s < 0 && errno == ENOTCONN) return LX_POLLOUT | LX_POLLWRNORM | LX_POLLHUP;
    }
    if (atomic_load(&sk->shut_rd)) rcv_shut = 1;
    if (atomic_load(&sk->shut_wr)) snd_shut = 1;
    if (rcv_shut) r |= LX_POLLIN | LX_POLLRDNORM | LX_POLLRDHUP;
    if (snd_shut) r |= LX_POLLOUT | LX_POLLWRNORM;
    if (rcv_shut && snd_shut) r |= LX_POLLHUP;
    if (err) r |= LX_POLLERR;
    return r;
}

static int64_t sock_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    return net_sock_recv(t, o, gva, count, 0, 0, 0);
}

static int64_t sock_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    return net_sock_send(t, o, gva, count, 0, NULL);
}

static int sock_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    /* sockfs inode: S_IFSOCK | 0777, owner = creator's fsuid, block size PAGE_SIZE */
    memset(out, 0, sizeof *out);
    out->st_dev = 0x8;
    out->st_ino = o->ino;
    out->st_nlink = 1;
    out->st_mode = LX_S_IFSOCK | 0777;
    out->st_uid = t && t->proc ? t->proc->cred.fsuid : 0;
    out->st_gid = t && t->proc ? t->proc->cred.fsgid : 0;
    out->st_blksize = 4096;
    return 0;
}

static int64_t sock_ioctl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    struct net_sock *sk = o->priv;
    int64_t ifr;
    if (net_if_ioctl(cmd, arg, &ifr)) return ifr;   /* SIOCGIF*: any socket (dev_ioctl), ifaces.c */
    switch (cmd) {
    case LX_SIOCINQ: {
        int v = 0;
        if (atomic_load(&sk->listening)) return -LX_EINVAL;
        if (ioctl(o->host_fd, FIONREAD, &v) < 0) return LKD_HOST_ERR();
        if (!is_stream(sk)) {
            /* Linux SIOCINQ on a datagram socket: size of the next datagram (udp_ioctl first_packet_length) */
            int n = 0;
            socklen_t l = sizeof n;
            if (getsockopt(o->host_fd, SOL_SOCKET, SO_NREAD, &n, &l) == 0) v = n;
        }
        int32_t v32 = v;
        return lkd_user_write(arg, &v32, sizeof v32) < 0 ? -LX_EFAULT : 0;
    }
    case LX_SIOCOUTQ: {
        int n = 0;
        socklen_t l = sizeof n;
        if (getsockopt(o->host_fd, SOL_SOCKET, SO_NWRITE, &n, &l) < 0) return LKD_HOST_ERR();
        int32_t v32 = n;
        return lkd_user_write(arg, &v32, sizeof v32) < 0 ? -LX_EFAULT : 0;
    }
    case LX_SIOCATMARK: {
        int v = 0;
        if (ioctl(o->host_fd, SIOCATMARK, &v) < 0) return LKD_HOST_ERR();
        int32_t v32 = v;
        return lkd_user_write(arg, &v32, sizeof v32) < 0 ? -LX_EFAULT : 0;
    }
    }
    lkd_log_once("net: socket ioctl 0x%x not implemented (-ENOTTY)", cmd);
    (void)t;
    return -LX_ENOTTY;
}

static int64_t sock_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)arg;
    if (cmd == LX_F_SETFL) return 0;  /* O_NONBLOCK lives in status_flags; the host fd stays as it is */
    return -LX_EINVAL;
}

static int sock_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "socket:[%llu]", (unsigned long long)o->ino);
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static void sock_release(lkd_fdobj *o)
{
    struct net_sock *sk = o->priv;
    if (!sk) return;
    net_abstract_release(sk);
    if (sk->nl_peer > 0) close(sk->nl_peer);
    free(sk);
    o->priv = NULL;
}

const lkd_fd_ops net_sock_ops = {
    .name = "socket",
    .read = sock_read,
    .write = sock_write,
    .ioctl = sock_ioctl,
    .fcntl = sock_fcntl,
    .poll = sock_poll,
    .fstat = sock_fstat,
    .describe = sock_describe,
    .release = sock_release,
};

int net_sock_wrap(int hfd, int domain, int type, int protocol, uint32_t lx_flags, int owned, lkd_fdobj **out)
{
    struct net_sock *sk = calloc(1, sizeof *sk);
    if (!sk) return -LX_ENOMEM;
    sk->domain = domain;
    sk->type = type;
    sk->protocol = protocol;
    sk->owned_nonblock = owned;
    sk->abs_lock_fd = -1;
    sk->pmtudisc = sk->pmtudisc6 = LX_IP_PMTUDISC_WANT;    /* inet_create, inet6_create (ip_no_pmtu_disc = 0) */
    int one = 1;
    setsockopt(hfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    if (owned) {
        fcntl(hfd, F_SETFL, fcntl(hfd, F_GETFL) | O_NONBLOCK);
        fcntl(hfd, F_SETFD, FD_CLOEXEC);
    }
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_SOCKET, &net_sock_ops, hfd, LX_O_RDWR | lx_flags, sk);
    if (!o) { free(sk); return -LX_ENOMEM; }
    struct stat st;
    o->ino = (fstat(hfd, &st) == 0 && st.st_ino) ? (uint64_t)st.st_ino : atomic_fetch_add(&g_sock_ino, 1);
    o->dev = 0x8;
    *out = o;
    return 0;
}

lkd_fdobj *net_sock_get(lkd_task *t, int fd, int *err)
{
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) { *err = -LX_EBADF; return NULL; }
    if (o->ops != &net_sock_ops) { lkd_fdobj_put(o); *err = -LX_ENOTSOCK; return NULL; }
    return o;
}

static int nonblocking(lkd_fdobj *o) { return (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0; }

/* ---- socket / socketpair ----------------------------------------------------------------------- */

static int check_create(int domain, int type, int protocol, int *htype)
{
    if (type & ~(LX_SOCK_TYPE_MASK | LX_SOCK_NONBLOCK | LX_SOCK_CLOEXEC)) return -LX_EINVAL;
    int base = type & LX_SOCK_TYPE_MASK;
    switch (domain) {
    case LX_AF_UNIX:
        if (protocol && protocol != LX_AF_UNIX) return -LX_EPROTONOSUPPORT;
        if (base != LX_SOCK_STREAM && base != LX_SOCK_DGRAM && base != LX_SOCK_SEQPACKET) return -LX_ESOCKTNOSUPPORT;
        if (base == LX_SOCK_SEQPACKET) {
            /* MESURE: macOS has no AF_UNIX SOCK_SEQPACKET (EPROTONOSUPPORT). */
            lkd_log_once("net: AF_UNIX SOCK_SEQPACKET is not available on the host (-ESOCKTNOSUPPORT)");
            return -LX_ESOCKTNOSUPPORT;
        }
        break;
    case LX_AF_INET:
    case LX_AF_INET6:
        if (base == LX_SOCK_RAW) return -LX_EPERM;     /* no CAP_NET_RAW (inet_create) */
        if (base != LX_SOCK_STREAM && base != LX_SOCK_DGRAM) return -LX_ESOCKTNOSUPPORT;
        break;
    case LX_AF_NETLINK:
        /* NETLINK_ROUTE served by LKD from the host's interfaces (ifaces.c, request wine-8) */
        if (base != LX_SOCK_RAW && base != LX_SOCK_DGRAM) return -LX_ESOCKTNOSUPPORT;
        break;
    case LX_AF_PACKET:
        return -LX_EPERM;                              /* packet_create needs CAP_NET_RAW */
    default:
        return -LX_EAFNOSUPPORT;
    }
    *htype = lkd_socktype_to_host(base);
    return 0;
}

static int default_protocol(int domain, int type, int protocol)
{
    if (protocol || domain == LX_AF_UNIX) return protocol;
    return type == LX_SOCK_STREAM ? LX_IPPROTO_TCP : LX_IPPROTO_UDP;
}

LKD_SYSCALL(socket)
{
    int domain = LKD_ARGI(0), type = LKD_ARGI(1), protocol = LKD_ARGI(2), htype;
    int e = check_create(domain, type, protocol, &htype);
    if (e) return e;
    if (domain == LX_AF_NETLINK) {
        lkd_fdobj *no;
        e = net_nl_create(type & LX_SOCK_TYPE_MASK, protocol, type & LX_SOCK_NONBLOCK, &no);
        if (e) return e;
        return net_install(t, no, (type & LX_SOCK_CLOEXEC) != 0);
    }
    int hfd = socket(lkd_af_to_host(domain), htype, domain == LX_AF_UNIX ? 0 : protocol);
    if (hfd < 0) return LKD_HOST_ERR();
    lkd_fdobj *o;
    int base = type & LX_SOCK_TYPE_MASK;
    e = net_sock_wrap(hfd, domain, base, default_protocol(domain, base, protocol), type & LX_SOCK_NONBLOCK, 1, &o);
    if (e) { close(hfd); return e; }
    return net_install(t, o, (type & LX_SOCK_CLOEXEC) != 0);
}

LKD_SYSCALL(socketpair)
{
    int domain = LKD_ARGI(0), type = LKD_ARGI(1), protocol = LKD_ARGI(2), htype;
    int e = check_create(domain, type, protocol, &htype);
    if (e) return e;
    if (domain != LX_AF_UNIX) return -LX_EOPNOTSUPP;   /* inet_stream_ops.socketpair = sock_no_socketpair */
    int sv[2];
    if (socketpair(AF_UNIX, htype, 0, sv) < 0) return LKD_HOST_ERR();
    int base = type & LX_SOCK_TYPE_MASK, cloexec = (type & LX_SOCK_CLOEXEC) != 0;
    lkd_fdobj *a = NULL, *b = NULL;
    if ((e = net_sock_wrap(sv[0], domain, base, protocol, type & LX_SOCK_NONBLOCK, 1, &a))) { close(sv[0]); close(sv[1]); return e; }
    if ((e = net_sock_wrap(sv[1], domain, base, protocol, type & LX_SOCK_NONBLOCK, 1, &b))) { lkd_fdobj_put(a); close(sv[1]); return e; }
    int fa = net_install(t, a, cloexec);
    if (fa < 0) { lkd_fdobj_put(b); return fa; }
    int fb = net_install(t, b, cloexec);
    if (fb < 0) { lkd_fd_close(t, fa); return fb; }
    int32_t v[2] = { fa, fb };
    if (lkd_user_write(LKD_ARGP(3), v, sizeof v) < 0) {
        lkd_fd_close(t, fa);
        lkd_fd_close(t, fb);
        return -LX_EFAULT;
    }
    return 0;
}

/* ---- bind / listen / accept / connect ---------------------------------------------------------- */

struct hop { int fd; const struct sockaddr *sa; socklen_t len; int which; };
static int do_hop(void *p)
{
    struct hop *h = p;
    return h->which == 0 ? bind(h->fd, h->sa, h->len) : connect(h->fd, h->sa, h->len);
}

static int sock_bind(lkd_task *t, lkd_fdobj *o, uint64_t addr, uint32_t alen)
{
    struct net_sock *sk = o->priv;
    if (alen > sizeof(struct lx_sockaddr_storage)) return -LX_EINVAL;
    uint8_t raw[sizeof(struct lx_sockaddr_storage) + 1] = { 0 };
    if (alen && lkd_user_read(addr, raw, alen) < 0) return -LX_EFAULT;
    if (sk->domain == LX_AF_UNIX) {
        if (alen < 2) return -LX_EINVAL;
        uint16_t fam;
        memcpy(&fam, raw, 2);
        if (fam != LX_AF_UNIX) return -LX_EINVAL;      /* unix_validate_addr */
        pthread_mutex_lock(&o->lock);
        int bound = sk->name_len != 0;
        pthread_mutex_unlock(&o->lock);
        if (bound) return -LX_EINVAL;                   /* unix_bind_*: u->addr already set */
    }
    uint8_t lx[sizeof(struct lx_sockaddr_storage) + 1];
    uint32_t lxlen = 0;
    struct net_haddr ha;
    int autob = sk->domain == LX_AF_UNIX && alen == 2;
    int r = 0;
    if (autob) {
        /* unix_autobind: an abstract name of five hex digits */
        for (int tries = 0; tries < 64; tries++) {
            lxlen = sizeof lx;
            net_autobind_name(lx, &lxlen);
            r = net_addr_from_bytes(t, sk, lx, lxlen, 1, &ha);
            if (r < 0) return r;
            r = net_abstract_bind_lock(sk, ha.abs_host, lx + 3, lxlen - 3);
            if (r == -LX_EADDRINUSE) { net_haddr_done(&ha); continue; }
            break;
        }
        if (r < 0) { net_haddr_done(&ha); return r; }
    } else {
        r = net_addr_from_bytes(t, sk, raw, alen, 1, &ha);
        if (r < 0) return r;
        if (ha.abstract) {
            r = net_abstract_bind_lock(sk, ha.abs_host, raw + 3, alen - 3);
            if (r < 0) { net_haddr_done(&ha); return r; }
            memcpy(lx, raw, alen);
            lxlen = alen;
        }
    }
    if (sk->domain == LX_AF_INET || sk->domain == LX_AF_INET6) {
        /* inet_bind_conflict / ip_unprivileged_port_start: ports below 1024 need CAP_NET_BIND_SERVICE */
        uint16_t port = ntohs(((struct sockaddr_in *)&ha.ss)->sin_port);
        if (port && port < 1024 && !(t->proc && t->proc->cred.euid == 0)) { net_haddr_done(&ha); return -LX_EACCES; }
    }
    struct hop h = { o->host_fd, (struct sockaddr *)&ha.ss, ha.len, 0 };
    r = net_with_dir(&ha, do_hop, &h);
    if (r < 0) {
        int e = errno;
        if (ha.abstract) net_abstract_release(sk);
        net_haddr_done(&ha);
        errno = e;
        return (int)LKD_HOST_ERR();
    }
    if (sk->domain == LX_AF_UNIX) {
        if (!ha.abstract) {
            /* unix_bind_bsd: the inode gets S_IFSOCK | (0777 & ~umask) */
            uint32_t mode = 0777 & ~lkd_vfs_umask(t);
            struct sockaddr_un *un = (struct sockaddr_un *)&ha.ss;
            if (ha.dirfd >= 0) fchmodat(ha.dirfd, un->sun_path, (mode_t)mode, 0);
            else chmod(un->sun_path, (mode_t)mode);
            /* getsockname returns sun_path as given plus its NUL (unix_mkname_bsd) */
            raw[alen] = 0;
            size_t pl = strnlen((char *)raw + 2, alen - 2);
            memcpy(lx, raw, 2 + pl);
            lx[2 + pl] = 0;
            lxlen = (uint32_t)(2 + pl + 1);
        }
        pthread_mutex_lock(&o->lock);
        memcpy(sk->name, lx, lxlen);
        sk->name_len = lxlen;
        pthread_mutex_unlock(&o->lock);
    }
    net_haddr_done(&ha);
    return 0;
}

LKD_SYSCALL(bind)
{
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    int r = net_sk(o)->domain == LX_AF_NETLINK ? net_nl_bind(t, net_sk(o), LKD_ARGP(1), LKD_ARGUI(2))
                                               : sock_bind(t, o, LKD_ARGP(1), LKD_ARGUI(2));
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(listen)
{
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int backlog = LKD_ARGI(1);
    int64_t r = 0;
    if (!is_stream(sk)) r = -LX_EOPNOTSUPP;
    else if (sk->domain == LX_AF_UNIX && sk->name_len == 0) r = -LX_EINVAL;   /* unix_listen: unbound */
    else {
        if ((unsigned)backlog > LX_SOMAXCONN) backlog = LX_SOMAXCONN;
        if (listen(o->host_fd, backlog) < 0) r = LKD_HOST_ERR();
        else atomic_store(&sk->listening, 1);
    }
    lkd_fdobj_put(o);
    return r;
}

static int64_t do_accept(lkd_task *t, int fd, uint64_t addr, uint64_t alen, int flags)
{
    if (flags & ~(LX_SOCK_CLOEXEC | LX_SOCK_NONBLOCK)) return -LX_EINVAL;
    int e;
    lkd_fdobj *o = net_sock_get(t, fd, &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int64_t r;
    if (!is_stream(sk)) { lkd_fdobj_put(o); return -LX_EOPNOTSUPP; }
    if (!atomic_load(&sk->listening)) { lkd_fdobj_put(o); return -LX_EINVAL; }
    struct sockaddr_storage ss;
    socklen_t sl;
    int nfd;
    for (;;) {
        sl = sizeof ss;
        nfd = accept(o->host_fd, (struct sockaddr *)&ss, &sl);
        if (nfd >= 0) break;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != ECONNABORTED) { r = LKD_HOST_ERR(); goto out; }
        int w = net_sock_wait(t, o, POLLIN, nonblocking(o), sk->rcvtimeo_ns);
        if (w < 0) { r = w; goto out; }
    }
    lkd_fdobj *n;
    e = net_sock_wrap(nfd, sk->domain, sk->type, sk->protocol, flags & LX_SOCK_NONBLOCK, 1, &n);
    if (e) { close(nfd); r = e; goto out; }
    struct net_sock *nsk = n->priv;                         /* the clone keeps the listener's IP settings */
    nsk->pmtudisc = atomic_load(&sk->pmtudisc);
    nsk->pmtudisc6 = atomic_load(&sk->pmtudisc6);
    nsk->dontfrag6 = atomic_load(&sk->dontfrag6);
    uint8_t lx[sizeof(struct lx_sockaddr_storage)];
    uint32_t lxlen = net_addr_to_linux((struct sockaddr *)&ss, sl, lx, sizeof lx);
    if (addr) {
        int pr = net_put_addr(addr, alen, lx, lxlen);
        if (pr < 0) { lkd_fdobj_put(n); r = pr; goto out; }
    }
    r = net_install(t, n, (flags & LX_SOCK_CLOEXEC) != 0);
out:
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(accept) { return do_accept(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), 0); }
LKD_SYSCALL(accept4) { return do_accept(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGI(3)); }

static int64_t sock_connect(lkd_task *t, lkd_fdobj *o, uint64_t addr, uint32_t alen)
{
    struct net_sock *sk = o->priv;
    if (alen > sizeof(struct lx_sockaddr_storage)) return -LX_EINVAL;
    uint8_t raw[sizeof(struct lx_sockaddr_storage) + 1] = { 0 };
    if (alen && lkd_user_read(addr, raw, alen) < 0) return -LX_EFAULT;
    if (alen >= 2 && sk->domain != LX_AF_UNIX) {
        uint16_t fam;
        memcpy(&fam, raw, 2);
        if (fam == LX_AF_UNSPEC) {
            /* Disconnect (inet_dgram_connect / tcp_disconnect). MESURE: macOS dissolves the association
             * of a UDP socket but answers EAFNOSUPPORT; a TCP socket refuses. */
            struct sockaddr_storage z = { 0 };
            z.ss_len = sizeof(struct sockaddr_in);
            z.ss_family = AF_UNSPEC;
            int c = connect(o->host_fd, (struct sockaddr *)&z, z.ss_len);
            if (c == 0 || sk->type == LX_SOCK_DGRAM) {
                struct sockaddr_storage p;
                socklen_t pl = sizeof p;
                if (c == 0 || (getpeername(o->host_fd, (struct sockaddr *)&p, &pl) < 0 && errno == ENOTCONN)) return 0;
            }
            lkd_log_once("net: connect(AF_UNSPEC) on a stream socket is not supported by the host");
            return LKD_HOST_ERR();
        }
    }
    struct net_haddr ha;
    int r = net_addr_from_bytes(t, sk, raw, alen, 0, &ha);
    if (r < 0) return r;
    int err = atomic_exchange(&sk->pending_err, 0);
    if (err && atomic_load(&sk->connecting)) { atomic_store(&sk->connecting, 0); net_haddr_done(&ha); return -err; }
    struct hop h = { o->host_fd, (struct sockaddr *)&ha.ss, ha.len, 1 };
    int c = net_with_dir(&ha, do_hop, &h);
    int64_t ret = 0;
    if (c == 0) {
        atomic_store(&sk->connecting, 0);
    } else if (errno == EINPROGRESS || errno == EALREADY || (errno == EAGAIN && sk->domain == LX_AF_UNIX)) {
        int was = atomic_exchange(&sk->connecting, 1);
        if (nonblocking(o)) {
            ret = was && errno == EALREADY ? -LX_EALREADY : -LX_EINPROGRESS;
            goto done;
        }
        /* __inet_stream_connect: wait for the handshake, interruptible (restartable). */
        for (;;) {
            int w = lkd_block_wait_fd(t, o->host_fd, POLLOUT, sk->sndtimeo_ns > 0 ? lkd_host_mono_ns() + sk->sndtimeo_ns : -1);
            if (w == 0) { ret = -LX_EINPROGRESS; goto done; }   /* SO_SNDTIMEO expired: -EINPROGRESS */
            if (w < 0) { ret = (w == -LX_ERESTARTSYS && sk->sndtimeo_ns > 0) ? -LX_EINTR : w; goto done; }
            if (sk->domain == LX_AF_UNIX && errno == EAGAIN) {
                c = net_with_dir(&ha, do_hop, &h);
                if (c == 0 || errno == EISCONN) break;
                if (errno == EAGAIN) continue;
                ret = LKD_HOST_ERR();
                goto done;
            }
            break;
        }
        atomic_store(&sk->connecting, 0);
        int e = 0;
        socklen_t l = sizeof e;
        if (getsockopt(o->host_fd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e) ret = -(int64_t)lkd_errno_from_darwin(e);
    } else if (errno == ENOTSOCK && sk->domain == LX_AF_UNIX) {
        ret = -LX_ECONNREFUSED;                 /* unix_find_bsd: the inode is not a socket */
    } else {
        ret = LKD_HOST_ERR();
    }
done:
    if (ret == 0 && sk->domain == LX_AF_UNIX && alen > 2 && alen <= sizeof(struct lx_sockaddr_un)) {
        pthread_mutex_lock(&o->lock);
        if (raw[2]) {
            size_t pl = strnlen((char *)raw + 2, alen - 2);
            memcpy(sk->peer, raw, 2 + pl);
            sk->peer[2 + pl] = 0;
            sk->peer_len = (uint32_t)(2 + pl + 1);
        } else {
            memcpy(sk->peer, raw, alen);
            sk->peer_len = alen;
        }
        pthread_mutex_unlock(&o->lock);
    }
    net_haddr_done(&ha);
    return ret;
}

LKD_SYSCALL(connect)
{
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    /* netlink: the kernel (port 0) is the only peer LKD serves; connect only records it */
    int64_t r = net_sk(o)->domain == LX_AF_NETLINK ? 0 : sock_connect(t, o, LKD_ARGP(1), LKD_ARGUI(2));
    lkd_fdobj_put(o);
    return r;
}

/* ---- names and shutdown ------------------------------------------------------------------------ */

static int64_t sock_name(lkd_task *t, int fd, uint64_t addr, uint64_t alen, int peer)
{
    (void)t;
    int e;
    lkd_fdobj *o = net_sock_get(t, fd, &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    uint8_t lx[sizeof(struct lx_sockaddr_storage)];
    uint32_t lxlen;
    int64_t r = 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    if (sk->domain == LX_AF_NETLINK) {
        if (!peer && !sk->nl_pid) sk->nl_pid = (uint32_t)t->proc->pid;   /* netlink_autobind */
        lxlen = net_nl_name(sk, lx, peer);
    } else if (sk->domain == LX_AF_UNIX && !peer) {
        pthread_mutex_lock(&o->lock);
        lxlen = sk->name_len;
        if (lxlen) memcpy(lx, sk->name, lxlen);
        pthread_mutex_unlock(&o->lock);
        if (!lxlen) { uint16_t f = LX_AF_UNIX; memcpy(lx, &f, 2); lxlen = 2; }
    } else {
        int c = peer ? getpeername(o->host_fd, (struct sockaddr *)&ss, &sl) : getsockname(o->host_fd, (struct sockaddr *)&ss, &sl);
        if (c < 0) { r = LKD_HOST_ERR(); goto out; }
        lxlen = net_addr_to_linux((struct sockaddr *)&ss, sl, lx, sizeof lx);
        if (sk->domain == LX_AF_UNIX && peer) {
            /* the host gives the server's host path; when it cannot be mapped back (relative leaf of a
             * long path, outside the guest view) report the name this socket connected to */
            struct sockaddr_un *un = (struct sockaddr_un *)&ss;
            int mapped = sl > offsetof(struct sockaddr_un, sun_path) && un->sun_path[0] == '/' && lxlen > 3;
            int abstract_ok = lxlen > 2 && lx[2] == 0;
            pthread_mutex_lock(&o->lock);
            if (!mapped && !abstract_ok && sk->peer_len) { memcpy(lx, sk->peer, sk->peer_len); lxlen = sk->peer_len; }
            pthread_mutex_unlock(&o->lock);
        }
    }
    r = net_put_addr(addr, alen, lx, lxlen);
out:
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(getsockname) { return sock_name(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), 0); }
LKD_SYSCALL(getpeername) { return sock_name(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), 1); }

LKD_SYSCALL(shutdown)
{
    int how = LKD_ARGI(1);
    if (how < 0 || how > 2) return -LX_EINVAL;
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int64_t r = 0;
    int hhow = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
    if (shutdown(o->host_fd, hhow) < 0) r = LKD_HOST_ERR();
    else {
        if (how != 1) atomic_store(&sk->shut_rd, 1);
        if (how != 0) atomic_store(&sk->shut_wr, 1);
    }
    lkd_fdobj_put(o);
    return r;
}

void net_register_socket(void)
{
    LKD_REGISTER(socket);
    LKD_REGISTER(socketpair);
    LKD_REGISTER(bind);
    LKD_REGISTER(listen);
    LKD_REGISTER(accept);
    LKD_REGISTER(accept4);
    LKD_REGISTER(connect);
    LKD_REGISTER(getsockname);
    LKD_REGISTER(getpeername);
    LKD_REGISTER(shutdown);
}
