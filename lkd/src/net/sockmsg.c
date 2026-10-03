/* sockmsg.c: data transfer on sockets: sendto, recvfrom, sendmsg, recvmsg, sendmmsg, recvmmsg, and
 * read/write on a socket. Models net/socket.c (____sys_sendmsg, ____sys_recvmsg, __sys_sendmmsg,
 * do_recvmmsg), net/core/scm.c (__scm_send, scm_detach_fds) and the blocking rules of
 * net/core/sock.c (sock_rcvtimeo, sock_intr_errno) of Linux 6.12.
 *
 * Guest buffers are handed to the host call directly after lkd_mm_check_range (CONVENTIONS 2.2), the
 * host fd is never blocking: an EAGAIN of the host becomes an interruptible wait.
 *
 * Ancillary data. Linux cmsghdr is 16 bytes (u64 len, level 1 = SOL_SOCKET) with 8-byte alignment;
 * Darwin's is 12 bytes (u32 len, level 0xffff) with 4-byte alignment. SCM_RIGHTS carries LKD objects
 * as host descriptors (scm.c). The host control buffer always has room for 254 descriptors: MESURE
 * (reports/darwin-process-facts-a.md F5) Darwin installs every descriptor in flight even when the buffer
 * is short, so a short buffer would leak them; LKD then closes what the guest buffer cannot hold, like
 * scm_detach_fds.
 */
#include <fcntl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include "net_priv.h"

#define LX_UIO_MAXIOV 1024
#define LX_SCM_MAX_FD 253
#define HOST_MAX_FDS 254                  /* MESURE: 255 descriptors in one message give EINVAL */
#define HOST_CTL_BYTES (CMSG_SPACE(HOST_MAX_FDS * sizeof(int)) + 256)

static int nonblocking(lkd_fdobj *o) { return (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0; }
static int is_stream(const struct net_sock *sk) { return sk->type == LX_SOCK_STREAM || sk->type == LX_SOCK_SEQPACKET; }

/* Linux send flags to host flags; unknown bits are ignored like tcp_sendmsg/unix_*_sendmsg do. */
static int send_flags(int lx, int *nosignal)
{
    int untr = 0;
    int h = lkd_msgflags_to_host(lx & (LX_MSG_OOB | LX_MSG_DONTROUTE | LX_MSG_DONTWAIT | LX_MSG_EOR), &untr);
    *nosignal = (lx & LX_MSG_NOSIGNAL) != 0;
    return h;
}

static void sigpipe(lkd_task *t)
{
    lkd_sig_send_task(t, LX_SIGPIPE, NULL, 1);
}

/* Build host iovecs for guest iovecs after validating every range. */
static int64_t build_iov(uint64_t iov_gva, uint64_t n, int for_write, struct iovec **out, uint64_t *total)
{
    if (n > LX_UIO_MAXIOV) return -LX_EMSGSIZE;
    *total = 0;
    *out = NULL;
    if (!n) return 0;
    struct lx_iovec *liov = malloc(n * sizeof *liov);
    struct iovec *hiov = malloc(n * sizeof *hiov);
    if (!liov || !hiov) { free(liov); free(hiov); return -LX_ENOMEM; }
    if (lkd_user_read(iov_gva, liov, n * sizeof *liov) < 0) { free(liov); free(hiov); return -LX_EFAULT; }
    for (uint64_t i = 0; i < n; i++) {
        if ((int64_t)liov[i].iov_len < 0) { free(liov); free(hiov); return -LX_EINVAL; }
        if (liov[i].iov_len && lkd_mm_check_range(liov[i].iov_base, liov[i].iov_len, for_write) < 0) {
            free(liov); free(hiov); return -LX_EFAULT;
        }
        hiov[i].iov_base = liov[i].iov_len ? lkd_g2h(liov[i].iov_base) : NULL;
        hiov[i].iov_len = liov[i].iov_len;
        *total += liov[i].iov_len;
    }
    free(liov);
    *out = hiov;
    return 0;
}

/* Advance an iovec array by n bytes (partial stream sends and MSG_WAITALL). */
static void iov_advance(struct iovec **iov, int *cnt, size_t n)
{
    while (*cnt && n >= (*iov)->iov_len) { n -= (*iov)->iov_len; (*iov)++; (*cnt)--; }
    if (*cnt && n) { (*iov)->iov_base = (char *)(*iov)->iov_base + n; (*iov)->iov_len -= n; }
}

struct sendctx { int fd; struct msghdr *m; int flags; ssize_t r; };
static int sendctx_run(void *p) { struct sendctx *c = p; c->r = sendmsg(c->fd, c->m, c->flags); return c->r < 0 ? -1 : 0; }

/* IP_RECVERR (request wine-4): an error the host reports for a datagram of an inet socket is also queued
 * for recvmsg(MSG_ERRQUEUE), as ip_icmp_error/ip_local_error do, and sk_err is still returned once to the
 * next call. The host gives the errno only (ECONNREFUSED from an ICMP port unreachable on a connected UDP
 * socket): the entry is reported with SO_EE_ORIGIN_LOCAL, no ICMP type, code or offender. */
static void errq_note(lkd_fdobj *o, int lx_err)
{
    struct net_sock *sk = o->priv;
    if (!atomic_load(&sk->recverr) || is_stream(sk) || (sk->domain != LX_AF_INET && sk->domain != LX_AF_INET6)) return;
    if (lx_err != LX_ECONNREFUSED && lx_err != LX_EHOSTUNREACH && lx_err != LX_ENETUNREACH && lx_err != LX_EHOSTDOWN)
        return;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    uint32_t n = 0;
    if (getpeername(o->host_fd, (struct sockaddr *)&ss, &sl) == 0)
        n = net_addr_to_linux((struct sockaddr *)&ss, sl, sk->errq_addr, sizeof sk->errq_addr);
    sk->errq_addrlen = n;
    atomic_store(&sk->errq_errno, lx_err);
}

/* recvmsg/recvfrom with MSG_ERRQUEUE: -EAGAIN when the queue is empty (never blocks, sock_error_queue
 * semantics); else no data, the address of the failed datagram, MSG_ERRQUEUE in msg_flags and one
 * IP_RECVERR/IPV6_RECVERR control message (sock_extended_err + offender sockaddr). */
static int64_t errq_take(struct net_sock *sk, struct lx_sock_extended_err *ee, uint8_t *addr, uint32_t *alen)
{
    int e = atomic_exchange(&sk->errq_errno, 0);
    if (!e) return -LX_EAGAIN;
    memset(ee, 0, sizeof *ee);
    ee->ee_errno = (uint32_t)e;
    ee->ee_origin = LX_SO_EE_ORIGIN_LOCAL;
    *alen = sk->errq_addrlen;
    memcpy(addr, sk->errq_addr, sizeof sk->errq_addr);
    return 0;
}

/* Core of every send: iov already built. to may be NULL. Control (host) may be NULL. */
static int64_t send_core(lkd_task *t, lkd_fdobj *o, struct iovec *iov, int iovcnt, uint64_t total, int lx_flags,
                         struct net_haddr *to, void *ctl, socklen_t ctllen)
{
    struct net_sock *sk = o->priv;
    if (sk->domain == LX_AF_NETLINK) {
        /* requests to the kernel side served by LKD (ifaces.c); the destination is always the kernel */
        if (!sk->nl_pid && lkd_current()) sk->nl_pid = (uint32_t)lkd_current()->proc->pid;   /* netlink_autobind */
        if (total > (1u << 20)) return -LX_EMSGSIZE;
        uint8_t *b = malloc(total ? total : 1);
        if (!b) return -LX_ENOMEM;
        uint64_t p = 0;
        for (int i = 0; i < iovcnt; i++) { if (iov[i].iov_len) memcpy(b + p, iov[i].iov_base, iov[i].iov_len); p += iov[i].iov_len; }
        int64_t r = net_nl_send(o, b, total);
        free(b);
        return r;
    }
    int nosig;
    int hf = send_flags(lx_flags, &nosig);
    int nb = nonblocking(o) || (lx_flags & LX_MSG_DONTWAIT);
    if (!sk->owned_nonblock) hf |= MSG_DONTWAIT;
    int err = atomic_exchange(&sk->pending_err, 0);
    if (err) return -err;
    uint64_t sent = 0;
    struct msghdr m = { 0 };
    if (to) { m.msg_name = &to->ss; m.msg_namelen = to->len; }
    for (;;) {
        m.msg_iov = iov;
        m.msg_iovlen = iovcnt;
        m.msg_control = sent ? NULL : ctl;          /* ancillary data goes with the first byte */
        m.msg_controllen = sent ? 0 : ctllen;
        struct sendctx c = { o->host_fd, &m, hf, 0 };
        if (to) net_with_dir(to, sendctx_run, &c);
        else sendctx_run(&c);
        if (c.r >= 0) {
            sent += (uint64_t)c.r;
            if (!is_stream(sk) || sent >= total || nb) return (int64_t)sent;
            iov_advance(&iov, &iovcnt, (size_t)c.r);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == ENOBUFS) {
            if (nb) return sent ? (int64_t)sent : -LX_EAGAIN;
            int w = net_sock_wait(t, o, POLLOUT, 0, sk->sndtimeo_ns);
            if (w < 0) return sent ? (int64_t)sent : w;
            continue;
        }
        if (errno == EPIPE) {
            if (!nosig) sigpipe(t);
            return sent ? (int64_t)sent : -LX_EPIPE;
        }
        if (errno == EMSGSIZE && sk->domain == LX_AF_UNIX && !is_stream(sk)) return -LX_EMSGSIZE;
        int64_t he = LKD_HOST_ERR();
        errq_note(o, (int)-he);
        return sent ? (int64_t)sent : he;
    }
}

int64_t net_sock_send(lkd_task *t, lkd_fdobj *o, uint64_t buf, uint64_t len, int lx_flags, struct net_haddr *to)
{
    if (len > 0x7ffff000ull) len = 0x7ffff000ull;   /* MAX_RW_COUNT */
    if (len && lkd_mm_check_range(buf, len, 0) < 0) return -LX_EFAULT;
    struct iovec iov = { len ? lkd_g2h(buf) : NULL, len };
    return send_core(t, o, &iov, 1, len, lx_flags, to, NULL, 0);
}

/* ---- receive ----------------------------------------------------------------------------------- */

struct rcvout {
    uint8_t name[sizeof(struct lx_sockaddr_storage)];
    uint32_t namelen;
    int hflags_out;
    void *ctl;              /* host control buffer (HOST_CTL_BYTES) or NULL */
    socklen_t ctllen;
};

static int64_t recv_core(lkd_task *t, lkd_fdobj *o, struct iovec *iov, int iovcnt, uint64_t total, int lx_flags,
                         struct rcvout *out)
{
    struct net_sock *sk = o->priv;
    int untr = 0;
    int hf = lkd_msgflags_to_host(lx_flags & (LX_MSG_OOB | LX_MSG_PEEK | LX_MSG_DONTWAIT | LX_MSG_WAITALL), &untr);
    hf &= ~MSG_WAITALL;                              /* waited for by LKD */
    int nb = nonblocking(o) || (lx_flags & LX_MSG_DONTWAIT);
    hf |= MSG_DONTWAIT;
    int err = atomic_exchange(&sk->pending_err, 0);
    if (err) return -err;
    int waitall = (lx_flags & LX_MSG_WAITALL) && is_stream(sk) && !(lx_flags & LX_MSG_PEEK) && !nb;
    /* MSG_TRUNC on a datagram socket returns the real datagram length (udp_recvmsg, unix_dgram_recvmsg) */
    int want_real = (lx_flags & LX_MSG_TRUNC) && !is_stream(sk);
    uint64_t got = 0;
    for (;;) {
        int real = 0;
        if (want_real) {
            socklen_t l = sizeof real;
            if (getsockopt(o->host_fd, SOL_SOCKET, SO_NREAD, &real, &l) < 0) real = 0;
        }
        struct sockaddr_storage ss;
        struct msghdr m = { 0 };
        m.msg_name = &ss;
        m.msg_namelen = sizeof ss;
        m.msg_iov = iov;
        m.msg_iovlen = iovcnt;
        m.msg_control = got ? NULL : (out ? out->ctl : NULL);
        m.msg_controllen = got ? 0 : (out ? out->ctllen : 0);
        memset(&ss, 0, sizeof ss);
        ssize_t r = recvmsg(o->host_fd, &m, hf);
        if (r >= 0) {
            if (out && !got) {
                out->namelen = m.msg_namelen ? net_addr_to_linux((struct sockaddr *)&ss, m.msg_namelen, out->name, sizeof out->name) : 0;
                if (out->namelen == 2 && sk->domain == LX_AF_UNIX) out->namelen = 0;   /* unbound sender: msg_namelen 0 */
                if (sk->domain == LX_AF_NETLINK) out->namelen = net_nl_name(sk, out->name, 1);   /* from the kernel */
                out->hflags_out = m.msg_flags;
                out->ctllen = m.msg_control ? m.msg_controllen : 0;
            }
            if (want_real && real > r && (m.msg_flags & MSG_TRUNC)) return real;
            got += (uint64_t)r;
            if (!waitall || r == 0 || got >= total) return (int64_t)got;
            iov_advance(&iov, &iovcnt, (size_t)r);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            if (nb) return got ? (int64_t)got : -LX_EAGAIN;
            int w = net_sock_wait(t, o, POLLIN, 0, sk->rcvtimeo_ns);
            if (w < 0) return got ? (int64_t)got : w;
            continue;
        }
        if (errno == ENOTCONN && is_stream(sk) && atomic_load(&sk->shut_rd)) return (int64_t)got;
        int64_t he = LKD_HOST_ERR();
        errq_note(o, (int)-he);
        return got ? (int64_t)got : he;
    }
}

int64_t net_sock_recv(lkd_task *t, lkd_fdobj *o, uint64_t buf, uint64_t len, int lx_flags, uint64_t addr_gva, uint64_t addrlen_gva)
{
    if (len > 0x7ffff000ull) len = 0x7ffff000ull;
    if (lx_flags & LX_MSG_ERRQUEUE) {
        struct lx_sock_extended_err ee;
        uint8_t a[sizeof(struct lx_sockaddr_in6)];
        uint32_t al = 0;
        int64_t r = errq_take(o->priv, &ee, a, &al);
        if (r < 0) return r;
        if (addr_gva) { int e = net_put_addr(addr_gva, addrlen_gva, a, al); if (e < 0) return e; }
        return 0;
    }
    if (len && lkd_mm_check_range(buf, len, 1) < 0) return -LX_EFAULT;
    struct iovec iov = { len ? lkd_g2h(buf) : NULL, len };
    struct rcvout out = { .ctl = NULL };
    int64_t r = recv_core(t, o, &iov, 1, len, lx_flags, &out);
    if (r >= 0 && addr_gva) {
        int e = net_put_addr(addr_gva, addrlen_gva, out.name, out.namelen);
        if (e < 0) return e;
    }
    return r;
}

/* ---- sendto / recvfrom ------------------------------------------------------------------------- */

LKD_SYSCALL(sendto)
{
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int64_t r;
    uint64_t addr = LKD_ARGP(4);
    uint32_t alen = LKD_ARGUI(5);
    if (addr && alen && sk->domain != LX_AF_NETLINK) {
        struct net_haddr ha;
        r = net_addr_from_guest(t, sk, addr, alen, 0, &ha);
        if (r >= 0) {
            /* a destination on a connected stream socket is ignored by tcp (EISCONN by unix_stream) */
            if (is_stream(sk)) r = net_sock_send(t, o, LKD_ARGP(1), LKD_ARGU(2), LKD_ARGI(3), NULL);
            else r = net_sock_send(t, o, LKD_ARGP(1), LKD_ARGU(2), LKD_ARGI(3), &ha);
        }
        net_haddr_done(&ha);
    } else {
        r = net_sock_send(t, o, LKD_ARGP(1), LKD_ARGU(2), LKD_ARGI(3), NULL);
    }
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(recvfrom)
{
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    int64_t r = net_sock_recv(t, o, LKD_ARGP(1), LKD_ARGU(2), LKD_ARGI(3), LKD_ARGP(4), LKD_ARGP(5));
    lkd_fdobj_put(o);
    return r;
}

/* ---- sendmsg / recvmsg ------------------------------------------------------------------------- */

/* Parse the guest control buffer into a host control buffer (SCM_RIGHTS only). */
static int64_t build_send_ctl(lkd_task *t, struct net_sock *sk, uint64_t ctl_gva, uint64_t ctl_len,
                              uint8_t **hctl, socklen_t *hlen, int *hfds, int *nh)
{
    *hctl = NULL;
    *hlen = 0;
    *nh = 0;
    if (ctl_len > 0x7fffffff) return -LX_ENOBUFS;
    if (ctl_len < sizeof(struct lx_cmsghdr)) return 0;
    if (ctl_len > 65536) return -LX_ENOBUFS;        /* sysctl_optmem_max */
    uint8_t *buf = malloc(ctl_len);
    if (!buf) return -LX_ENOMEM;
    if (lkd_user_read(ctl_gva, buf, ctl_len) < 0) { free(buf); return -LX_EFAULT; }
    int64_t ret = 0;
    int nguest = 0;
    for (uint64_t off = 0; off + sizeof(struct lx_cmsghdr) <= ctl_len;) {
        struct lx_cmsghdr ch;
        memcpy(&ch, buf + off, sizeof ch);
        if (ch.cmsg_len < sizeof ch || ch.cmsg_len > ctl_len - off) { ret = -LX_EINVAL; break; }
        if (ch.cmsg_level == LX_SOL_SOCKET) {
            if (sk->domain != LX_AF_UNIX) { ret = -LX_EINVAL; break; }
            if (ch.cmsg_type == LX_SCM_RIGHTS) {
                int n = (int)((ch.cmsg_len - sizeof ch) / sizeof(int32_t));
                if (nguest + n > LX_SCM_MAX_FD) { ret = -LX_EINVAL; break; }
                for (int i = 0; i < n; i++) {
                    int32_t gfd;
                    memcpy(&gfd, buf + off + sizeof ch + (size_t)i * 4, 4);
                    lkd_fdobj *x = lkd_fd_get(t, gfd, NULL);
                    if (!x) { ret = -LX_EBADF; break; }
                    int k = net_scm_export(x, hfds + *nh, HOST_MAX_FDS - *nh);
                    lkd_fdobj_put(x);
                    if (k < 0) { ret = k; break; }
                    *nh += k;
                    nguest++;
                }
                if (ret < 0) break;
            } else if (ch.cmsg_type == LX_SCM_CREDENTIALS) {
                /* scm_check_creds: the sender may only claim its own identity (or any, with CAP_SETUID).
                 * The receiver's credentials come from the host's view of the peer, which is the same
                 * process, so the message itself carries nothing. */
                struct lx_ucred u;
                if (ch.cmsg_len < LX_CMSG_LEN(sizeof u)) { ret = -LX_EINVAL; break; }
                memcpy(&u, buf + off + sizeof ch, sizeof u);
                const struct lkd_creds *c = &t->proc->cred;
                int priv = c->euid == 0;
                int uid_ok = u.uid == c->uid || u.uid == c->euid || u.uid == c->suid;
                int gid_ok = u.gid == c->gid || u.gid == c->egid || u.gid == c->sgid;
                if (!priv && (u.pid != t->proc->pid || !uid_ok || !gid_ok)) { ret = -LX_EPERM; break; }
            } else {
                ret = -LX_EINVAL;
                break;
            }
        } else if (sk->domain != LX_AF_UNIX) {
            lkd_log_once("net: ancillary data level %d type %d on an inet socket is not implemented (-EOPNOTSUPP)",
                         ch.cmsg_level, ch.cmsg_type);
            ret = -LX_EOPNOTSUPP;
            break;
        }
        /* other levels on AF_UNIX are skipped (__scm_send) */
        off += LX_CMSG_ALIGN(ch.cmsg_len);
    }
    free(buf);
    if (ret < 0) {
        for (int i = 0; i < *nh; i++) close(hfds[i]);
        *nh = 0;
        return ret;
    }
    if (*nh) {
        socklen_t sz = (socklen_t)CMSG_SPACE((size_t)*nh * sizeof(int));
        uint8_t *h = calloc(1, sz);
        if (!h) { for (int i = 0; i < *nh; i++) close(hfds[i]); *nh = 0; return -LX_ENOMEM; }
        struct cmsghdr *c = (struct cmsghdr *)h;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN((size_t)*nh * sizeof(int));
        memcpy(CMSG_DATA(c), hfds, (size_t)*nh * sizeof(int));
        *hctl = h;
        *hlen = sz;
    }
    return 0;
}

static int64_t do_sendmsg(lkd_task *t, int fd, uint64_t msg_gva, int flags)
{
    struct lx_msghdr mh;
    if (lkd_user_read(msg_gva, &mh, sizeof mh) < 0) return -LX_EFAULT;
    int e;
    lkd_fdobj *o = net_sock_get(t, fd, &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    struct iovec *iov = NULL;
    uint64_t total = 0;
    int64_t r = build_iov(mh.msg_iov, mh.msg_iovlen, 0, &iov, &total);
    if (r < 0) { lkd_fdobj_put(o); return r; }
    struct net_haddr ha;
    struct net_haddr *to = NULL;
    memset(&ha, 0, sizeof ha);
    ha.dirfd = -1;
    if (mh.msg_name && mh.msg_namelen && !is_stream(sk) && sk->domain != LX_AF_NETLINK) {
        if (mh.msg_namelen < 0) { r = -LX_EINVAL; goto out; }
        r = net_addr_from_guest(t, sk, mh.msg_name, (uint32_t)mh.msg_namelen, 0, &ha);
        if (r < 0) goto out;
        to = &ha;
    }
    uint8_t *hctl = NULL;
    socklen_t hlen = 0;
    int hfds[HOST_MAX_FDS];
    int nh = 0;
    r = build_send_ctl(t, sk, mh.msg_control, mh.msg_controllen, &hctl, &hlen, hfds, &nh);
    if (r < 0) goto out;
    r = send_core(t, o, iov, (int)mh.msg_iovlen, total, flags, to, hctl, hlen);
    for (int i = 0; i < nh; i++) close(hfds[i]);   /* the host duplicated what was in flight */
    free(hctl);
out:
    net_haddr_done(&ha);
    free(iov);
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(sendmsg) { return do_sendmsg(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2)); }

/* Turn the host control data into guest control data (scm_detach_fds). Returns the guest bytes used
 * and sets *ctrunc. Every received host descriptor is either installed or closed. */
static int64_t deliver_rights(lkd_task *t, uint8_t *hctl, socklen_t hlen, uint64_t ctl_gva, uint64_t ctl_cap,
                           int cloexec, int *ctrunc)
{
    int hfds[HOST_MAX_FDS * 2];
    int n = 0;
    for (struct cmsghdr *c = hlen ? (struct cmsghdr *)hctl : NULL; c;) {
        if ((uint8_t *)c + sizeof *c > hctl + hlen || c->cmsg_len < sizeof *c) break;
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k && n < (int)(sizeof hfds / sizeof hfds[0]); i++) memcpy(&hfds[n++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
        }
        uint8_t *next = (uint8_t *)c + __DARWIN_ALIGN32(c->cmsg_len);
        if (next >= hctl + hlen) break;
        c = (struct cmsghdr *)next;
    }
    if (!n) return 0;
    lkd_fdobj *objs[HOST_MAX_FDS * 2];
    int nobj = net_scm_import(hfds, n, objs, (int)(sizeof objs / sizeof objs[0]));
    int fdmax = ctl_cap >= sizeof(struct lx_cmsghdr) ? (int)((ctl_cap - sizeof(struct lx_cmsghdr)) / sizeof(int32_t)) : 0;
    int32_t gfds[HOST_MAX_FDS * 2];
    int inst = 0;
    for (int i = 0; i < nobj; i++) {
        if (inst < fdmax) {
            int g = lkd_fd_alloc(t, objs[i], 0, cloexec);
            lkd_fdobj_put(objs[i]);
            if (g < 0) { *ctrunc = 1; for (int j = i + 1; j < nobj; j++) lkd_fdobj_put(objs[j]); break; }
            gfds[inst++] = g;
        } else {
            *ctrunc = 1;
            lkd_fdobj_put(objs[i]);
        }
    }
    if (!inst) { if (nobj) *ctrunc = 1; return 0; }
    struct lx_cmsghdr ch = { LX_CMSG_LEN((size_t)inst * 4), LX_SOL_SOCKET, LX_SCM_RIGHTS };
    uint64_t used = LX_CMSG_SPACE((size_t)inst * 4);
    if (used > ctl_cap) used = ctl_cap;
    if (lkd_user_write(ctl_gva, &ch, sizeof ch) < 0 || lkd_user_write(ctl_gva + sizeof ch, gfds, (size_t)inst * 4) < 0) {
        for (int i = 0; i < inst; i++) lkd_fd_close(t, gfds[i]);
        return -LX_EFAULT;
    }
    return (int64_t)used;
}

/* scm_recv: credentials first (when SO_PASSCRED is on and the peer is known), then the descriptors. */
static int64_t deliver_ctl(lkd_task *t, lkd_fdobj *o, uint8_t *hctl, socklen_t hlen, uint64_t ctl_gva,
                           uint64_t ctl_cap, int cloexec, int *ctrunc)
{
    struct net_sock *sk = o->priv;
    uint64_t used0 = 0;
    if (atomic_load(&sk->passcred) && sk->domain == LX_AF_UNIX) {
        struct lx_ucred u;
        if (net_peer_cred(o, &u)) {
            if (ctl_cap >= LX_CMSG_LEN(sizeof u)) {
                struct lx_cmsghdr ch = { LX_CMSG_LEN(sizeof u), LX_SOL_SOCKET, LX_SCM_CREDENTIALS };
                if (lkd_user_write(ctl_gva, &ch, sizeof ch) < 0 || lkd_user_write(ctl_gva + sizeof ch, &u, sizeof u) < 0) return -LX_EFAULT;
                used0 = LX_CMSG_SPACE(sizeof u);
                if (used0 > ctl_cap) used0 = ctl_cap;
            } else {
                *ctrunc = 1;
            }
        }
    }
    int64_t r = 0;
    if (hlen) {
        r = deliver_rights(t, hctl, hlen, ctl_gva + used0, ctl_cap - used0, cloexec, ctrunc);
        if (r < 0) return r;
    }
    return (int64_t)used0 + r;
}

static int64_t recv_errqueue(lkd_fdobj *o, uint64_t msg_gva, const struct lx_msghdr *mh)
{
    struct net_sock *sk = o->priv;
    struct lx_sock_extended_err ee;
    uint8_t a[sizeof(struct lx_sockaddr_in6)];
    uint32_t al = 0;
    int64_t r = errq_take(sk, &ee, a, &al);
    if (r < 0) return r;
    int v6 = sk->domain == LX_AF_INET6;
    /* ip_recv_error: struct { sock_extended_err ee; sockaddr_in offender; } (sockaddr_in6 for IPv6),
     * the offender zeroed (family AF_UNSPEC) when unknown */
    uint8_t data[sizeof ee + sizeof(struct lx_sockaddr_in6)];
    memset(data, 0, sizeof data);
    memcpy(data, &ee, sizeof ee);
    uint32_t dlen = (uint32_t)sizeof ee + (v6 ? (uint32_t)sizeof(struct lx_sockaddr_in6) : (uint32_t)sizeof(struct lx_sockaddr_in));
    int lxf = LX_MSG_ERRQUEUE;
    uint64_t used = 0;
    if (mh->msg_control && mh->msg_controllen >= LX_CMSG_LEN(dlen)) {
        struct lx_cmsghdr ch = { LX_CMSG_LEN(dlen), v6 ? LX_IPPROTO_IPV6 : LX_IPPROTO_IP, v6 ? LX_IPV6_RECVERR : LX_IP_RECVERR };
        if (lkd_user_write(mh->msg_control, &ch, sizeof ch) < 0 ||
            lkd_user_write(mh->msg_control + sizeof ch, data, dlen) < 0) return -LX_EFAULT;
        used = LX_CMSG_SPACE(dlen) <= mh->msg_controllen ? LX_CMSG_SPACE(dlen) : LX_CMSG_LEN(dlen);
    } else {
        lxf |= LX_MSG_CTRUNC;   /* put_cmsg without room */
    }
    int32_t nl = 0;
    if (mh->msg_name && al) {
        uint32_t n = (uint32_t)mh->msg_namelen < al ? (uint32_t)mh->msg_namelen : al;
        if (n && lkd_user_write(mh->msg_name, a, n) < 0) return -LX_EFAULT;
        nl = (int32_t)al;
    }
    if (lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_namelen), &nl, sizeof nl) < 0 ||
        lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_controllen), &used, sizeof used) < 0 ||
        lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_flags), &lxf, sizeof lxf) < 0)
        return -LX_EFAULT;
    return 0; /* no payload is kept (the host does not return the failed datagram) */
}

static int64_t do_recvmsg(lkd_task *t, int fd, uint64_t msg_gva, int flags, int nonblock_extra)
{
    struct lx_msghdr mh;
    if (lkd_user_read(msg_gva, &mh, sizeof mh) < 0) return -LX_EFAULT;
    int e;
    lkd_fdobj *o = net_sock_get(t, fd, &e);
    if (!o) return e;
    if (flags & LX_MSG_ERRQUEUE) {
        int64_t r = recv_errqueue(o, msg_gva, &mh);
        lkd_fdobj_put(o);
        return r;
    }
    struct iovec *iov = NULL;
    uint64_t total = 0;
    int64_t r = build_iov(mh.msg_iov, mh.msg_iovlen, 1, &iov, &total);
    if (r < 0) { lkd_fdobj_put(o); return r; }
    if (mh.msg_namelen < 0) { free(iov); lkd_fdobj_put(o); return -LX_EINVAL; }
    struct rcvout out;
    memset(&out, 0, sizeof out);
    out.ctl = calloc(1, HOST_CTL_BYTES);
    out.ctllen = HOST_CTL_BYTES;
    if (!out.ctl) { free(iov); lkd_fdobj_put(o); return -LX_ENOMEM; }
    r = recv_core(t, o, iov, (int)mh.msg_iovlen, total, flags | nonblock_extra, &out);
    if (r >= 0) {
        int ctrunc = 0;
        int64_t used = 0;
        used = deliver_ctl(t, o, out.ctl, out.ctllen, mh.msg_control, mh.msg_control ? mh.msg_controllen : 0,
                           (flags & LX_MSG_CMSG_CLOEXEC) != 0, &ctrunc);
        if (used < 0) { r = used; goto out; }
        int lxf = lkd_msgflags_from_host(out.hflags_out & (MSG_EOR | MSG_TRUNC | MSG_OOB)) | (ctrunc ? LX_MSG_CTRUNC : 0);
        if (flags & LX_MSG_CMSG_CLOEXEC) lxf |= LX_MSG_CMSG_CLOEXEC;   /* ____sys_recvmsg keeps it */
        int32_t nl = 0;
        if (mh.msg_name && out.namelen) {
            uint32_t n = (uint32_t)mh.msg_namelen < out.namelen ? (uint32_t)mh.msg_namelen : out.namelen;
            if (n && lkd_user_write(mh.msg_name, out.name, n) < 0) { r = -LX_EFAULT; goto out; }
            nl = (int32_t)out.namelen;
        }
        uint64_t cl = (uint64_t)used;
        if (lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_namelen), &nl, sizeof nl) < 0 ||
            lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_controllen), &cl, sizeof cl) < 0 ||
            lkd_user_write(msg_gva + offsetof(struct lx_msghdr, msg_flags), &lxf, sizeof lxf) < 0)
            r = -LX_EFAULT;
    }
out:
    free(out.ctl);
    free(iov);
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(recvmsg) { return do_recvmsg(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), 0); }

/* ---- sendmmsg / recvmmsg ----------------------------------------------------------------------- */

LKD_SYSCALL(sendmmsg)
{
    int fd = LKD_ARGI(0);
    uint64_t vec = LKD_ARGP(1);
    unsigned vlen = LKD_ARGUI(2);
    int flags = LKD_ARGI(3);
    if (vlen > LX_UIO_MAXIOV) vlen = LX_UIO_MAXIOV;
    unsigned i;
    int64_t err = 0;
    for (i = 0; i < vlen; i++) {
        uint64_t m = vec + (uint64_t)i * sizeof(struct lx_mmsghdr);
        int64_t r = do_sendmsg(t, fd, m, flags);
        if (r < 0) { err = r; break; }
        uint32_t len = (uint32_t)r;
        if (lkd_user_write(m + offsetof(struct lx_mmsghdr, msg_len), &len, sizeof len) < 0) { err = -LX_EFAULT; break; }
    }
    return i ? (int64_t)i : err;
}

LKD_SYSCALL(recvmmsg)
{
    int fd = LKD_ARGI(0);
    uint64_t vec = LKD_ARGP(1);
    unsigned vlen = LKD_ARGUI(2);
    int flags = LKD_ARGI(3);
    uint64_t tsp = LKD_ARGP(4);
    int64_t deadline = -1;
    if (tsp) {
        struct lx_timespec ts;
        if (lkd_user_read(tsp, &ts, sizeof ts) < 0) return -LX_EFAULT;
        if (!net_ts_valid(&ts)) return -LX_EINVAL;
        deadline = net_deadline_from_ts(&ts);
    }
    if (vlen > LX_UIO_MAXIOV) vlen = LX_UIO_MAXIOV;
    unsigned i;
    int64_t err = 0;
    for (i = 0; i < vlen; i++) {
        uint64_t m = vec + (uint64_t)i * sizeof(struct lx_mmsghdr);
        int extra = (i > 0 && (flags & LX_MSG_WAITFORONE)) ? LX_MSG_DONTWAIT : 0;
        int64_t r = do_recvmsg(t, fd, m, flags & ~LX_MSG_WAITFORONE, extra);
        if (r < 0) { err = r; break; }
        uint32_t len = (uint32_t)r;
        if (lkd_user_write(m + offsetof(struct lx_mmsghdr, msg_len), &len, sizeof len) < 0) { err = -LX_EFAULT; break; }
        /* do_recvmmsg: the timeout is only checked after a datagram arrived (man recvmmsg BUGS) */
        if (deadline >= 0 && lkd_host_mono_ns() >= deadline) { i++; break; }
    }
    if (tsp) {
        int64_t left = deadline - lkd_host_mono_ns();
        if (left < 0) left = 0;
        struct lx_timespec ts = { left / 1000000000, left % 1000000000 };
        lkd_user_write(tsp, &ts, sizeof ts);
    }
    if (i) return (int64_t)i;
    if (err == -LX_EAGAIN && (flags & LX_MSG_WAITFORONE)) return err;
    return err;
}

void net_register_msg(void)
{
    LKD_REGISTER(sendto);
    LKD_REGISTER(recvfrom);
    LKD_REGISTER(sendmsg);
    LKD_REGISTER(recvmsg);
    LKD_REGISTER(sendmmsg);
    LKD_REGISTER(recvmmsg);
}
