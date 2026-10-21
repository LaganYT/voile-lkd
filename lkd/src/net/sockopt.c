/* sockopt.c: setsockopt/getsockopt. Models net/core/sock.c (sk_setsockopt, sk_getsockopt),
 * net/ipv4/tcp.c (do_tcp_setsockopt/getsockopt), net/ipv4/ip_sockglue.c and net/ipv6/ipv6_sockglue.c of
 * Linux 6.12. Option numbers are translated by core (lkd_sockopt_to_host); the options whose meaning
 * differs on Darwin are handled here:
 *   SO_TYPE, SO_PROTOCOL, SO_DOMAIN, SO_ACCEPTCONN  from the object
 *   SO_ERROR        an error already seen by poll is kept in the object and returned first
 *   SO_RCVTIMEO/SNDTIMEO  kept in the object: LKD waits itself (the host fd never blocks)
 *   SO_RCVBUF/SNDBUF      Linux doubles the value (capped by rmem_max/wmem_max = 212992)
 *   SO_LINGER       Darwin SO_LINGER counts clock ticks: SO_LINGER_SEC is the Linux meaning
 *   SO_PEERCRED     LOCAL_PEERCRED + LOCAL_PEERPID, pid and ids mapped to the guest's
 *   SO_PASSCRED     a flag: recvmsg adds SCM_CREDENTIALS built from LOCAL_PEERPID/LOCAL_PEERCRED
 *   boolean options are read back as 0/1 (Darwin returns the option bit)
 *   IP_MTU_DISCOVER, IPV6_MTU_DISCOVER, IPV6_DONTFRAG  kept in the object (apply_dontfrag)
 * Anything else answers -ENOPROTOOPT and is logged once.
 */
#include <sys/socket.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "net_priv.h"

#define DARWIN_IPV6_DONTFRAG 62      /* netinet6/in6.h, named only under __APPLE_USE_RFC_3542 */
#define LX_SO_RCVTIMEO_NEW 66
#define LX_SO_SNDTIMEO_NEW 67
#define LX_SYSCTL_RMEM_MAX 212992
#define LX_SYSCTL_WMEM_MAX 212992
#define LX_SOCK_MIN_RCVBUF 2304      /* 2048 + sizeof(struct sk_buff) aligned, x86-64 6.12 (approximate) */
#define LX_SOCK_MIN_SNDBUF 4608

static int is_bool_opt(int level, int opt)
{
    if (level == LX_SOL_SOCKET)
        return opt == LX_SO_REUSEADDR || opt == LX_SO_REUSEPORT || opt == LX_SO_KEEPALIVE || opt == LX_SO_BROADCAST ||
               opt == LX_SO_OOBINLINE || opt == LX_SO_DONTROUTE || opt == LX_SO_DEBUG || opt == LX_SO_ACCEPTCONN;
    if (level == LX_IPPROTO_TCP) return opt == LX_TCP_NODELAY;
    if (level == LX_IPPROTO_IPV6) return opt == LX_IPV6_V6ONLY || opt == LX_IPV6_MULTICAST_LOOP;
    if (level == LX_IPPROTO_IP) return opt == LX_IP_MULTICAST_LOOP;
    return 0;
}

int net_sockopt_get_error(lkd_fdobj *o)
{
    struct net_sock *sk = o->priv;
    int e = atomic_exchange(&sk->pending_err, 0);
    if (e) return e;
    int h = 0;
    socklen_t l = sizeof h;
    if (getsockopt(o->host_fd, SOL_SOCKET, SO_ERROR, &h, &l) < 0) return 0;
    return h ? lkd_errno_from_darwin(h) : 0;
}

/* The guest pid and ids of the peer of an AF_UNIX socket (cred_to_ucred). A peer outside the family has
 * no pid in the guest's namespace (pid_vnr gives 0); its uid is the guest identity of the runtime's host
 * account, or the overflow id. */
int net_peer_cred(lkd_fdobj *o, struct lx_ucred *u)
{
    u->pid = 0;
    u->uid = u->gid = (uint32_t)-1;
    struct xucred xc;
    socklen_t xl = sizeof xc;
    if (getsockopt(o->host_fd, SOL_LOCAL, LOCAL_PEERCRED, &xc, &xl) < 0) return 0;
    pid_t hp = 0;
    socklen_t pl = sizeof hp;
    if (getsockopt(o->host_fd, SOL_LOCAL, LOCAL_PEERPID, &hp, &pl) < 0) hp = 0;
    const lkd_config *c = lkd_config_get();
    uint32_t guid = c && c->uid ? c->uid : 1000, ggid = c && c->gid ? c->gid : 1000;
    u->uid = xc.cr_uid == getuid() ? guid : (xc.cr_uid == 0 ? 0 : 65534);
    u->gid = xc.cr_ngroups > 0 && (gid_t)xc.cr_groups[0] == getgid() ? ggid : 65534;
    int known = 0;
    if (hp > 0) {
        static lkd_proc_info list[256];
        static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&mu);
        int n = lkd_proc_list(list, 256);
        for (int i = 0; i < n; i++) {
            if (list[i].host_pid == hp && list[i].state != 'Z') {
                u->pid = list[i].pid;
                u->uid = list[i].euid;
                u->gid = list[i].egid;
                known = 1;
                break;
            }
        }
        pthread_mutex_unlock(&mu);
    }
    return known;
}

int net_peer_is_family(lkd_fdobj *o)
{
    pid_t hp = 0;
    socklen_t pl = sizeof hp;
    if (getsockopt(o->host_fd, SOL_LOCAL, LOCAL_PEERPID, &hp, &pl) < 0 || hp <= 0 || hp == getpid()) return 0;
    static lkd_proc_info list[256];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int found = 0;
    pthread_mutex_lock(&mu);
    int n = lkd_proc_list(list, 256);
    for (int i = 0; i < n && !found; i++)
        if (list[i].host_pid == hp && list[i].state != 'Z') found = 1;
    pthread_mutex_unlock(&mu);
    return found;
}

/* Path MTU options (ip(7), ipv6(7)): every socket keeps what the guest set and reads it back. A datagram
 * socket also gets the host's don't-fragment flag when Linux would send DF datagrams and never fragment
 * them (IP_PMTUDISC_DO and PROBE, IPV6_DONTFRAG): Darwin's IP_DONTFRAG and IPV6_DONTFRAG do that, and
 * answer EMSGSIZE beyond the MTU. The other values keep the host's default. Darwin's TCP does path MTU
 * discovery by itself, so a stream socket only keeps the value. */
static int64_t apply_dontfrag(lkd_fdobj *o, struct net_sock *sk)
{
    if (sk->type != LX_SOCK_DGRAM && sk->type != LX_SOCK_RAW) return 0;
    int r;
    if (sk->domain == LX_AF_INET) {
        int m = atomic_load(&sk->pmtudisc);
        int hv = m == LX_IP_PMTUDISC_DO || m == LX_IP_PMTUDISC_PROBE;
        r = setsockopt(o->host_fd, IPPROTO_IP, IP_DONTFRAG, &hv, sizeof hv);
    } else {
        int m = atomic_load(&sk->pmtudisc6);
        int hv = atomic_load(&sk->dontfrag6) || m == LX_IP_PMTUDISC_DO || m == LX_IP_PMTUDISC_PROBE;
        r = setsockopt(o->host_fd, IPPROTO_IPV6, DARWIN_IPV6_DONTFRAG, &hv, sizeof hv);
    }
    return r < 0 ? LKD_HOST_ERR() : 0;
}

/* IP_MTU_DISCOVER is an IP option of AF_INET and AF_INET6 sockets (ipv6_setsockopt hands SOL_IP to
 * ip_setsockopt); the IPv6 ones belong to AF_INET6 sockets only. */
static int is_pmtu_opt(int level, int opt)
{
    return (level == LX_IPPROTO_IP && opt == LX_IP_MTU_DISCOVER) ||
           (level == LX_IPPROTO_IPV6 && (opt == LX_IPV6_MTU_DISCOVER || opt == LX_IPV6_DONTFRAG));
}

static int pmtu_opt_applies(const struct net_sock *sk, int level)
{
    return sk->domain == LX_AF_INET6 || (sk->domain == LX_AF_INET && level == LX_IPPROTO_IP);
}

static int64_t put_opt(uint64_t optval, uint64_t optlen_gva, const void *v, uint32_t vlen, int32_t ulen)
{
    uint32_t n = (uint32_t)ulen < vlen ? (uint32_t)ulen : vlen;
    if (n && lkd_user_write(optval, v, n) < 0) return -LX_EFAULT;
    int32_t nl = (int32_t)n;
    if (lkd_user_write(optlen_gva, &nl, sizeof nl) < 0) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(getsockopt)
{
    int level = LKD_ARGI(1), opt = LKD_ARGI(2);
    uint64_t optval = LKD_ARGP(3), optlen = LKD_ARGP(4);
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int64_t r = 0;
    int32_t ulen;
    if (lkd_user_read(optlen, &ulen, sizeof ulen) < 0) { r = -LX_EFAULT; goto out; }
    if (ulen < 0) { r = -LX_EINVAL; goto out; }
    int32_t v = 0;
    if (level == 270 /* SOL_NETLINK */) {
        if (sk->domain != LX_AF_NETLINK) { r = -LX_ENOPROTOOPT; goto out; }
        if (opt < 1 || opt > 12) { r = -LX_ENOPROTOOPT; goto out; }
        v = 0;   /* no option has an effect: LKD answers synchronously and sends no notification */
        goto put_int;
    }
    if (level == LX_SOL_SOCKET) {
        switch (opt) {
        case LX_SO_TYPE: v = sk->type; goto put_int;
        case LX_SO_PROTOCOL: v = sk->protocol; goto put_int;
        case LX_SO_DOMAIN: v = sk->domain; goto put_int;
        case LX_SO_ACCEPTCONN: v = atomic_load(&sk->listening) ? 1 : 0; goto put_int;
        case LX_SO_ERROR: v = net_sockopt_get_error(o); goto put_int;
        case LX_SO_PASSCRED: v = atomic_load(&sk->passcred); goto put_int;
        case LX_SO_RCVTIMEO: case LX_SO_SNDTIMEO: case LX_SO_RCVTIMEO_NEW: case LX_SO_SNDTIMEO_NEW: {
            int64_t ns = (opt == LX_SO_RCVTIMEO || opt == LX_SO_RCVTIMEO_NEW) ? sk->rcvtimeo_ns : sk->sndtimeo_ns;
            int64_t tv[2] = { ns / 1000000000, (ns % 1000000000) / 1000 };
            r = put_opt(optval, optlen, tv, sizeof tv, ulen);
            goto out;
        }
        case LX_SO_LINGER: {
            struct linger l;
            socklen_t sl = sizeof l;
            if (getsockopt(o->host_fd, SOL_SOCKET, SO_LINGER_SEC, &l, &sl) < 0) { r = LKD_HOST_ERR(); goto out; }
            struct lx_linger ll = { l.l_onoff ? 1 : 0, l.l_linger };
            r = put_opt(optval, optlen, &ll, sizeof ll, ulen);
            goto out;
        }
        case LX_SO_PEERCRED: {
            struct lx_ucred u = { 0, (uint32_t)-1, (uint32_t)-1 };
            if (sk->domain == LX_AF_UNIX) net_peer_cred(o, &u);
            r = put_opt(optval, optlen, &u, sizeof u, ulen);
            goto out;
        }
        case LX_SO_SNDLOWAT: v = 1; goto put_int;      /* fixed at 1 on Linux */
        }
    }
    if ((level == LX_IPPROTO_IP && opt == LX_IP_RECVERR) || (level == LX_IPPROTO_IPV6 && opt == LX_IPV6_RECVERR)) {
        if (sk->domain != LX_AF_INET && sk->domain != LX_AF_INET6) { r = -LX_ENOPROTOOPT; goto out; }
        v = (atomic_load(&sk->recverr) & (level == LX_IPPROTO_IP ? 1 : 2)) ? 1 : 0;
        goto put_int;
    }
    if (is_pmtu_opt(level, opt)) {
        if (!pmtu_opt_applies(sk, level)) { r = -LX_ENOPROTOOPT; goto out; }
        v = level == LX_IPPROTO_IP ? atomic_load(&sk->pmtudisc)
            : opt == LX_IPV6_MTU_DISCOVER ? atomic_load(&sk->pmtudisc6) : atomic_load(&sk->dontfrag6);
        goto put_int;
    }
    int hl, ho;
    if (lkd_sockopt_to_host(level, opt, &hl, &ho) < 0) {
        lkd_log_once("net: getsockopt level %d option %d not implemented (-ENOPROTOOPT)", level, opt);
        r = -LX_ENOPROTOOPT;
        goto out;
    }
    if (level == LX_IPPROTO_IP && (opt == LX_IP_MULTICAST_TTL || opt == LX_IP_MULTICAST_LOOP)) {
        u_char c = 0;
        socklen_t sl = sizeof c;
        if (getsockopt(o->host_fd, hl, ho, &c, &sl) < 0) { r = LKD_HOST_ERR(); goto out; }
        v = c;
    } else {
        int hv = 0;
        socklen_t sl = sizeof hv;
        if (getsockopt(o->host_fd, hl, ho, &hv, &sl) < 0) { r = LKD_HOST_ERR(); goto out; }
        v = hv;
    }
    if (is_bool_opt(level, opt)) v = v ? 1 : 0;
put_int:
    if (level == LX_IPPROTO_IP && ulen > 0 && ulen < 4 && v >= 0 && v <= 255) {
        /* do_ip_getsockopt: a short buffer gets one unsigned char */
        uint8_t c = (uint8_t)v;
        r = put_opt(optval, optlen, &c, 1, 1);
    } else {
        r = put_opt(optval, optlen, &v, sizeof v, ulen);
    }
out:
    lkd_fdobj_put(o);
    return r;
}

static int64_t set_timeo(int64_t *dst, uint64_t optval, uint32_t optlen)
{
    int64_t tv[2];
    if (optlen < sizeof tv) return -LX_EINVAL;
    if (lkd_user_read(optval, tv, sizeof tv) < 0) return -LX_EFAULT;
    if (tv[1] < 0 || tv[1] >= 1000000) return -LX_EDOM;    /* sock_set_timeout */
    if (tv[0] < 0) { *dst = 0; return 0; }
    if (tv[0] == 0 && tv[1] == 0) { *dst = 0; return 0; }
    if (tv[0] > INT64_MAX / 1000000000 - 1) { *dst = 0; return 0; }
    *dst = tv[0] * 1000000000 + tv[1] * 1000;
    return 0;
}

LKD_SYSCALL(setsockopt)
{
    int level = LKD_ARGI(1), opt = LKD_ARGI(2);
    uint64_t optval = LKD_ARGP(3);
    int32_t optlen = LKD_ARGI(4);
    int e;
    lkd_fdobj *o = net_sock_get(t, LKD_ARGI(0), &e);
    if (!o) return e;
    struct net_sock *sk = o->priv;
    int64_t r = 0;
    if (optlen < 0) { r = -LX_EINVAL; goto out; }
    int32_t v = 0;
    int have_int = 0;
    if (optlen >= 4) {
        if (lkd_user_read(optval, &v, 4) < 0) { r = -LX_EFAULT; goto out; }
        have_int = 1;
    } else if (optlen >= 1 && level == LX_IPPROTO_IP) {
        uint8_t c;
        if (lkd_user_read(optval, &c, 1) < 0) { r = -LX_EFAULT; goto out; }
        v = c;
        have_int = 1;
    }
    if (level == 270 /* SOL_NETLINK */) {
        /* ADD/DROP_MEMBERSHIP, PKTINFO, BROADCAST_ERROR, NO_ENOBUFS, CAP_ACK, EXT_ACK, GET_STRICT_CHK...:
         * accepted, without effect (ifaces.c: synchronous answers, no notifications) */
        if (sk->domain != LX_AF_NETLINK) { r = -LX_ENOPROTOOPT; goto out; }
        if (opt < 1 || opt > 12) { r = -LX_ENOPROTOOPT; goto out; }
        if (!have_int) { r = -LX_EINVAL; goto out; }
        goto out;
    }
    if (level == LX_SOL_SOCKET) {
        switch (opt) {
        case LX_SO_TYPE: case LX_SO_PROTOCOL: case LX_SO_DOMAIN: case LX_SO_ERROR: case LX_SO_ACCEPTCONN:
        case LX_SO_PEERCRED:
            r = -LX_ENOPROTOOPT;
            goto out;
        case LX_SO_SNDLOWAT:
            r = -LX_ENOPROTOOPT;                       /* "not changeable on Linux" (socket(7)) */
            goto out;
        case LX_SO_RCVTIMEO: case LX_SO_RCVTIMEO_NEW:
            pthread_mutex_lock(&o->lock);
            r = set_timeo(&sk->rcvtimeo_ns, optval, (uint32_t)optlen);
            pthread_mutex_unlock(&o->lock);
            goto out;
        case LX_SO_SNDTIMEO: case LX_SO_SNDTIMEO_NEW:
            pthread_mutex_lock(&o->lock);
            r = set_timeo(&sk->sndtimeo_ns, optval, (uint32_t)optlen);
            pthread_mutex_unlock(&o->lock);
            goto out;
        case LX_SO_PASSCRED:
            if (!have_int) { r = -LX_EINVAL; goto out; }
            atomic_store(&sk->passcred, v ? 1 : 0);        /* recvmsg then adds the peer's real credentials */
            goto out;
        case LX_SO_SNDBUFFORCE: case LX_SO_RCVBUFFORCE:
            r = -LX_EPERM;                             /* CAP_NET_ADMIN */
            goto out;
        case LX_SO_LINGER: {
            struct lx_linger ll;
            if ((size_t)optlen < sizeof ll) { r = -LX_EINVAL; goto out; }
            if (lkd_user_read(optval, &ll, sizeof ll) < 0) { r = -LX_EFAULT; goto out; }
            struct linger l = { ll.l_onoff ? 1 : 0, ll.l_linger };
            if (setsockopt(o->host_fd, SOL_SOCKET, SO_LINGER_SEC, &l, sizeof l) < 0) r = LKD_HOST_ERR();
            goto out;
        }
        case LX_SO_RCVBUF: case LX_SO_SNDBUF: {
            if (!have_int) { r = -LX_EINVAL; goto out; }
            /* sk_setsockopt: val = min(val, sysctl_[rw]mem_max); sk_buf = max(val * 2, SOCK_MIN_*BUF) */
            int rcv = opt == LX_SO_RCVBUF;
            uint32_t uv = (uint32_t)v;
            uint32_t cap = rcv ? LX_SYSCTL_RMEM_MAX : LX_SYSCTL_WMEM_MAX;
            if (uv > cap) uv = cap;
            int hv = (int)(uv * 2);
            int mn = rcv ? LX_SOCK_MIN_RCVBUF : LX_SOCK_MIN_SNDBUF;
            if (hv < mn) hv = mn;
            if (setsockopt(o->host_fd, SOL_SOCKET, rcv ? SO_RCVBUF : SO_SNDBUF, &hv, sizeof hv) < 0) r = LKD_HOST_ERR();
            goto out;
        }
        }
    }
    if ((level == LX_IPPROTO_IP && opt == LX_IP_RECVERR) || (level == LX_IPPROTO_IPV6 && opt == LX_IPV6_RECVERR)) {
        /* ip_sockglue.c / ipv6_sockglue.c: a flag; turning it off purges the error queue. Errors the host
         * reports (ECONNREFUSED on a datagram socket) are queued for MSG_ERRQUEUE; ICMP details (type, code,
         * offender) are not available on the host (request wine-4). */
        if (sk->domain != LX_AF_INET && sk->domain != LX_AF_INET6) { r = -LX_ENOPROTOOPT; goto out; }
        if (!have_int) { r = -LX_EINVAL; goto out; }
        int bit = level == LX_IPPROTO_IP ? 1 : 2;
        if (v) atomic_fetch_or(&sk->recverr, bit);
        else if (!(atomic_fetch_and(&sk->recverr, ~bit) & ~bit)) atomic_store(&sk->errq_errno, 0);
        goto out;
    }
    if (is_pmtu_opt(level, opt)) {
        /* Wine sets IP_DONTFRAGMENT through IP_MTU_DISCOVER and IPV6_DONTFRAG; Unity's .NET does it on every
         * new socket and fails on an error. An IP option without a value reads as 0 (do_ip_setsockopt), an
         * IPv6 one too (do_ipv6_setsockopt), but IPV6_MTU_DISCOVER needs an int. */
        if (!pmtu_opt_applies(sk, level)) { r = -LX_ENOPROTOOPT; goto out; }
        if (opt == LX_IPV6_DONTFRAG && level == LX_IPPROTO_IPV6) {
            atomic_store(&sk->dontfrag6, v != 0);
        } else {
            if (level == LX_IPPROTO_IPV6 && !have_int) { r = -LX_EINVAL; goto out; }
            if (v < LX_IP_PMTUDISC_DONT || v > LX_IP_PMTUDISC_OMIT) { r = -LX_EINVAL; goto out; }
            atomic_store(level == LX_IPPROTO_IP ? &sk->pmtudisc : &sk->pmtudisc6, v);
        }
        r = apply_dontfrag(o, sk);
        goto out;
    }
    int hl, ho;
    if (lkd_sockopt_to_host(level, opt, &hl, &ho) < 0) {
        lkd_log_once("net: setsockopt level %d option %d not implemented (-ENOPROTOOPT)", level, opt);
        r = -LX_ENOPROTOOPT;
        goto out;
    }
    if (level == LX_IPPROTO_IP && (opt == LX_IP_ADD_MEMBERSHIP || opt == LX_IP_DROP_MEMBERSHIP)) {
        /* struct ip_mreq (8 bytes) or ip_mreqn (12 bytes: multiaddr, address, ifindex) */
        uint32_t m[3] = { 0 };
        if (optlen < 8) { r = -LX_EINVAL; goto out; }
        if (lkd_user_read(optval, m, optlen >= 12 ? 12 : 8) < 0) { r = -LX_EFAULT; goto out; }
        struct ip_mreq mr;
        mr.imr_multiaddr.s_addr = m[0];
        mr.imr_interface.s_addr = m[1];
        if (setsockopt(o->host_fd, hl, ho, &mr, sizeof mr) < 0) r = LKD_HOST_ERR();
        goto out;
    }
    if (!have_int) { r = -LX_EINVAL; goto out; }
    if (level == LX_IPPROTO_IP && (opt == LX_IP_MULTICAST_TTL || opt == LX_IP_MULTICAST_LOOP)) {
        if (v < -1 || v > 255) { r = -LX_EINVAL; goto out; }
        u_char c = (u_char)(v == -1 ? 1 : v);
        if (setsockopt(o->host_fd, hl, ho, &c, sizeof c) < 0) r = LKD_HOST_ERR();
        goto out;
    }
    int hv = v;
    if (is_bool_opt(level, opt)) hv = v ? 1 : 0;
    if (setsockopt(o->host_fd, hl, ho, &hv, sizeof hv) < 0) r = LKD_HOST_ERR();
out:
    lkd_fdobj_put(o);
    return r;
}

void net_register_sockopt(void)
{
    LKD_REGISTER(getsockopt);
    LKD_REGISTER(setsockopt);
}
