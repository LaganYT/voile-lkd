/* Witness (request wine-8): the interfaces a glibc program sees: getifaddrs and if_nameindex (both through
 * NETLINK_ROUTE in glibc 2.41), SIOCGIFCONF, and getaddrinfo(AI_ADDRCONFIG) (__check_pf, netlink). */
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
    int fails = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) < 0) { perror("getifaddrs"); fails++; }
    int n4 = 0, nlink = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        char a[64] = "-";
        int f = i->ifa_addr ? i->ifa_addr->sa_family : -1;
        if (f == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a); n4++; }
        if (f == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr, a, sizeof a);
        if (f == AF_PACKET) { struct sockaddr_ll *l = (void *)i->ifa_addr; snprintf(a, sizeof a, "link %02x:%02x:%02x:%02x:%02x:%02x",
                                l->sll_addr[0], l->sll_addr[1], l->sll_addr[2], l->sll_addr[3], l->sll_addr[4], l->sll_addr[5]); nlink++; }
        printf("getifaddrs %-6s family %2d flags 0x%05x %s\n", i->ifa_name, f, i->ifa_flags, a);
    }
    if (ifa) freeifaddrs(ifa);
    struct if_nameindex *ni = if_nameindex();
    int nn = 0;
    if (!ni) { perror("if_nameindex"); fails++; }
    for (struct if_nameindex *p = ni; p && p->if_index; p++, nn++) printf("if_nameindex %u %s\n", p->if_index, p->if_name);
    if (ni) if_freenameindex(ni);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    char buf[1024];
    struct ifconf ic = { .ifc_len = sizeof buf, .ifc_buf = buf };
    if (ioctl(s, SIOCGIFCONF, &ic) < 0) { perror("SIOCGIFCONF"); fails++; }
    else for (int off = 0; off < ic.ifc_len; off += sizeof(struct ifreq)) {
        struct ifreq *r = (void *)(buf + off);
        char a[64]; inet_ntop(AF_INET, &((struct sockaddr_in *)&r->ifr_addr)->sin_addr, a, sizeof a);
        struct ifreq q; memset(&q, 0, sizeof q); strcpy(q.ifr_name, r->ifr_name);
        int idx = ioctl(s, SIOCGIFINDEX, &q) == 0 ? q.ifr_ifindex : -1;
        printf("SIOCGIFCONF %s %s index %d\n", r->ifr_name, a, idx);
    }
    close(s);
    struct addrinfo hints = { .ai_flags = AI_ADDRCONFIG, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int g = getaddrinfo("localhost", "80", &hints, &res);
    printf("getaddrinfo(localhost, AI_ADDRCONFIG) = %d\n", g);
    if (res) freeaddrinfo(res);
    if (n4 < 1 || nlink < 1 || nn < 1 || nn != nlink) fails++;
    printf("%s: interfaces %d, IPv4 addresses %d\n", fails ? "FAIL" : "PASS", nn, n4);
    return fails ? 1 : 0;
}
