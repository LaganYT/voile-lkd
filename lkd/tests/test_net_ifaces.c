/* test_net_ifaces.c: network interfaces as the guest sees them (request wine-8). Linux references:
 * netdevice(7) (SIOCGIF*), rtnetlink(7), netlink(7), net/core/rtnetlink.c and net/core/dev_ioctl.c of v6.12.
 * The values come from the host (getifaddrs): the test compares with the host's own view (lo0 -> lo). */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "test_fs_util.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

struct ifreq_ { char name[16]; union { struct lx_sockaddr sa; int32_t ival; int16_t flags; uint8_t pad[24]; } u; };
struct ifconf_ { int32_t len; int32_t pad; uint64_t buf; };
struct nlh_ { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct rta_ { uint16_t len, type; };

/* Does the host have a non-loopback interface with IPv4 that is up and running? Then the guest sees one. */
static int host_v4_iface(char *ip, size_t cap)
{
    struct ifaddrs *ifa = NULL;
    int found = 0;
    if (getifaddrs(&ifa) < 0) return 0;
    for (struct ifaddrs *i = ifa; i && !found; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK) &&
            (i->ifa_flags & IFF_UP) && (i->ifa_flags & IFF_RUNNING)) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, (socklen_t)cap);
            found = 1;
        }
    freeifaddrs(ifa);
    return found;
}

LT_TEST(ioctls)
{
    int s = (int)SC(T, socket, LX_AF_INET, LX_SOCK_DGRAM, 0);
    LT_CHECK(s >= 0);
    struct ifreq_ r;
    memset(&r, 0, sizeof r);
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x8933 /* SIOCGIFINDEX */, P(&r)), 0);
    LT_EQ(r.u.ival, 1);
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x8913 /* SIOCGIFFLAGS */, P(&r)), 0);
    LT_EQ(r.u.flags & 0x49, 0x49);                                   /* UP | LOOPBACK | RUNNING */
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x8915 /* SIOCGIFADDR */, P(&r)), 0);
    struct lx_sockaddr_in *sin = (struct lx_sockaddr_in *)&r.u.sa;
    LT_EQ(sin->sin_family, LX_AF_INET);
    LT_EQ(sin->sin_addr, htonl(0x7f000001));
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x891b /* SIOCGIFNETMASK */, P(&r)), 0);
    LT_EQ(sin->sin_addr, htonl(0xff000000));
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x8921 /* SIOCGIFMTU */, P(&r)), 0);
    LT_EQ(r.u.ival, 65536);
    strcpy(r.name, "lo");
    LT_EQ(SC(T, ioctl, s, 0x8927 /* SIOCGIFHWADDR */, P(&r)), 0);
    LT_EQ(r.u.sa.sa_family, 772);                                     /* ARPHRD_LOOPBACK */
    memset(&r, 0, sizeof r);
    r.u.ival = 1;
    LT_EQ(SC(T, ioctl, s, 0x8910 /* SIOCGIFNAME */, P(&r)), 0);
    LT_STREQ(r.name, "lo");
    strcpy(r.name, "nosuch0");
    LT_ERRNO(SC(T, ioctl, s, 0x8933, P(&r)), LX_ENODEV);
    strcpy(r.name, "lo");
    LT_ERRNO(SC(T, ioctl, s, 0x8914 /* SIOCSIFFLAGS */, P(&r)), LX_EPERM);
    /* SIOCGIFCONF: size probe with a NULL buffer, then one ifreq per IPv4 address, lo first */
    struct ifconf_ ic = { 0, 0, 0 };
    LT_EQ(SC(T, ioctl, s, 0x8912, P(&ic)), 0);
    LT_CHECK(ic.len >= 40 && ic.len % 40 == 0);
    struct ifreq_ list[16];
    ic.buf = P(list);
    ic.len = sizeof list;
    LT_EQ(SC(T, ioctl, s, 0x8912, P(&ic)), 0);
    LT_STREQ(list[0].name, "lo");
    char ip[64];
    if (host_v4_iface(ip, sizeof ip)) {
        LT_CHECK(ic.len >= 80);
        char got[64];
        inet_ntop(AF_INET, &((struct lx_sockaddr_in *)&list[1].u.sa)->sin_addr, got, sizeof got);
        LT_STREQ(got, ip);
        LT_CHECK(!strncmp(list[1].name, "eth", 3) || !strncmp(list[1].name, "tun", 3) || !strncmp(list[1].name, "net", 3));
        /* a too small buffer: whole entries only */
        ic.len = 60;
        LT_EQ(SC(T, ioctl, s, 0x8912, P(&ic)), 0);
        LT_EQ(ic.len, 40);
    }
    SC(T, close, s);
}

/* One dump; returns the number of messages of type want before NLMSG_DONE, checks seq and pid. */
static int dump(int fd, uint16_t type, uint8_t family, uint32_t seq, uint32_t pid, uint8_t *out, size_t cap, size_t *outlen)
{
    struct { struct nlh_ h; uint8_t body[16]; } q;
    memset(&q, 0, sizeof q);
    q.h.len = sizeof q.h + 16;
    q.h.type = type;
    q.h.flags = 1 | 0x300;                                            /* NLM_F_REQUEST | NLM_F_DUMP */
    q.h.seq = seq;
    q.body[0] = family;
    struct lx_sockaddr_storage kern;
    memset(&kern, 0, sizeof kern);
    kern.ss_family = LX_AF_NETLINK;
    if (SC(T, sendto, fd, P(&q), q.h.len, 0, P(&kern), 12) != (int64_t)q.h.len) return -1;
    int count = 0;
    size_t used = 0;
    for (;;) {
        uint8_t buf[8192];
        struct lx_sockaddr_storage from;
        int32_t fl = sizeof from;
        int64_t n = SC(T, recvfrom, fd, P(buf), sizeof buf, 0, P(&from), P(&fl));
        if (n <= 0) return -2;
        if (fl != 12 || from.ss_family != LX_AF_NETLINK) return -3;
        for (int64_t off = 0; off + (int64_t)sizeof(struct nlh_) <= n;) {
            struct nlh_ *h = (struct nlh_ *)(buf + off);
            if (h->seq != seq || h->pid != pid) return -4;
            if (h->type == 3 /* NLMSG_DONE */) return count;
            if (h->type == 2 /* NLMSG_ERROR */) return -5;
            if (out && used + h->len <= cap) { memcpy(out + used, h, h->len); used += h->len; *outlen = used; }
            count++;
            off += (h->len + 3) & ~3u;
        }
    }
}

LT_TEST(netlink_route)
{
    int fd = (int)SC(T, socket, LX_AF_NETLINK, LX_SOCK_RAW | LX_SOCK_CLOEXEC, 0);
    LT_CHECK(fd >= 0);
    struct lx_sockaddr_storage a;
    memset(&a, 0, sizeof a);
    a.ss_family = LX_AF_NETLINK;
    LT_EQ(SC(T, bind, fd, P(&a), 12), 0);
    int32_t al = sizeof a;
    LT_EQ(SC(T, getsockname, fd, P(&a), P(&al)), 0);
    LT_EQ(al, 12);
    uint32_t pid;
    memcpy(&pid, (uint8_t *)&a + 4, 4);
    LT_EQ(pid, (uint32_t)T->proc->pid);                              /* autobind: the thread group id */
    int32_t one = 1;
    LT_EQ(SC(T, setsockopt, fd, 270, 11 /* NETLINK_EXT_ACK */, P(&one), 4), 0);
    static uint8_t links[65536];
    size_t ll = 0;
    int nl = dump(fd, 18 /* RTM_GETLINK */, 0, 7, pid, links, sizeof links, &ll);
    LT_CHECK(nl >= 1);
    /* the first link is lo, index 1, ARPHRD_LOOPBACK, with IFLA_IFNAME "lo" */
    struct nlh_ *h = (struct nlh_ *)links;
    LT_EQ(h->type, 16);                                               /* RTM_NEWLINK */
    uint8_t *ii = (uint8_t *)(h + 1);
    uint16_t arphrd;
    int32_t index;
    memcpy(&arphrd, ii + 2, 2);
    memcpy(&index, ii + 4, 4);
    LT_EQ(arphrd, 772);
    LT_EQ(index, 1);
    int named = 0;
    for (uint32_t off = sizeof *h + 16; off + 4 <= h->len;) {
        struct rta_ *r = (struct rta_ *)((uint8_t *)h + off);
        if (r->len < 4) break;
        if (r->type == 3 && !strcmp((char *)(r + 1), "lo")) named = 1;
        off += (r->len + 3) & ~3u;
    }
    LT_CHECK(named);
    int na = dump(fd, 22 /* RTM_GETADDR */, LX_AF_INET, 8, pid, NULL, 0, NULL);
    char ip[64];
    LT_CHECK(na >= 1 + host_v4_iface(ip, sizeof ip));
    int nr = dump(fd, 26 /* RTM_GETROUTE */, LX_AF_INET, 9, pid, NULL, 0, NULL);
    LT_CHECK(nr >= 0);
    if (host_v4_iface(ip, sizeof ip)) LT_CHECK(nr >= 1);              /* at least the connected prefix */
    /* changes are refused with NLMSG_ERROR -EPERM */
    struct { struct nlh_ h; uint8_t body[16]; } q;
    memset(&q, 0, sizeof q);
    q.h.len = sizeof q;
    q.h.type = 16;                                                    /* RTM_NEWLINK */
    q.h.flags = 1 | 4;
    q.h.seq = 10;
    LT_EQ(SC(T, sendto, fd, P(&q), sizeof q, 0, 0, 0), (int64_t)sizeof q);
    uint8_t buf[256];
    LT_CHECK(SC(T, recvfrom, fd, P(buf), sizeof buf, 0, 0, 0) >= 36);
    struct nlh_ *e = (struct nlh_ *)buf;
    int32_t err;
    memcpy(&err, e + 1, 4);
    LT_EQ(e->type, 2);
    LT_EQ(err, -LX_EPERM);
    /* nothing left, nonblocking read: EAGAIN; poll says not readable */
    LT_ERRNO(SC(T, recvfrom, fd, P(buf), sizeof buf, LX_MSG_DONTWAIT, 0, 0), LX_EAGAIN);
    SC(T, close, fd);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netif");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(ioctls);
    LT_RUN(netlink_route);
    rm_rf(ROOT);
    return lt_finish();
}
