/* test_core_xlate.c: Linux x86-64 <-> Darwin translations. Linux values come from include/uapi
 * (v6.12) and are written out here as literals, independent of abi_x64.h, so that a typo in the header
 * is caught. Darwin values come from the system headers. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ltest.h"
#include <lkd/xlate.h>

/* ---- errno ---- */
LT_TEST(errno_darwin_to_linux)
{
    /* errno-base.h / errno.h of Linux x86-64 versus <sys/errno.h> of Darwin */
    LT_EQ(lkd_errno_from_darwin(0), 0);
    LT_EQ(lkd_errno_from_darwin(EPERM), 1);
    LT_EQ(lkd_errno_from_darwin(ENOENT), 2);
    LT_EQ(lkd_errno_from_darwin(EINTR), 4);
    LT_EQ(lkd_errno_from_darwin(EBADF), 9);
    LT_EQ(lkd_errno_from_darwin(EAGAIN), 11);       /* Darwin 35 */
    LT_EQ(EAGAIN, 35);
    LT_EQ(lkd_errno_from_darwin(EDEADLK), 35);      /* Darwin 11 */
    LT_EQ(lkd_errno_from_darwin(ENOMEM), 12);
    LT_EQ(lkd_errno_from_darwin(EACCES), 13);
    LT_EQ(lkd_errno_from_darwin(EEXIST), 17);
    LT_EQ(lkd_errno_from_darwin(ENOTDIR), 20);
    LT_EQ(lkd_errno_from_darwin(EINVAL), 22);
    LT_EQ(lkd_errno_from_darwin(EMFILE), 24);
    LT_EQ(lkd_errno_from_darwin(ENOSPC), 28);
    LT_EQ(lkd_errno_from_darwin(ERANGE), 34);
    LT_EQ(lkd_errno_from_darwin(ENAMETOOLONG), 36); /* Darwin 63 */
    LT_EQ(lkd_errno_from_darwin(ENOSYS), 38);       /* Darwin 78 */
    LT_EQ(lkd_errno_from_darwin(ENOTEMPTY), 39);    /* Darwin 66 */
    LT_EQ(lkd_errno_from_darwin(ELOOP), 40);        /* Darwin 62 */
    LT_EQ(lkd_errno_from_darwin(ENOTSOCK), 88);
    LT_EQ(lkd_errno_from_darwin(EADDRINUSE), 98);
    LT_EQ(lkd_errno_from_darwin(ECONNRESET), 104);
    LT_EQ(lkd_errno_from_darwin(ETIMEDOUT), 110);
    LT_EQ(lkd_errno_from_darwin(ECONNREFUSED), 111);
    LT_EQ(lkd_errno_from_darwin(EINPROGRESS), 115);
    LT_EQ(lkd_errno_from_darwin(ENOTSUP), 95);      /* both map to EOPNOTSUPP */
    LT_EQ(lkd_errno_from_darwin(EOPNOTSUPP), 95);
    LT_EQ(lkd_errno_from_darwin(EOVERFLOW), 75);
    LT_EQ(lkd_errno_from_darwin(EILSEQ), 84);
    LT_EQ(lkd_errno_from_darwin(ENOATTR), 61);      /* missing xattr: ENODATA on Linux */
    LT_EQ(lkd_errno_from_darwin(EFTYPE), 22);
    LT_EQ(lkd_errno_from_darwin(EPROCLIM), 11);
    LT_EQ(lkd_errno_from_darwin(EBADARCH), 8);
    LT_EQ(lkd_errno_from_darwin(EQFULL), 105);
    LT_EQ(lkd_errno_from_darwin(ECANCELED), 125);
    LT_EQ(lkd_errno_from_darwin(EOWNERDEAD), 130);
    LT_EQ(lkd_errno_from_darwin(9999), 5);          /* unknown Darwin code: EIO */
    LT_EQ(lkd_errno_from_darwin(-1), 5);
}

LT_TEST(errno_roundtrip_and_names)
{
    int bad = 0, covered = 0;
    for (int lx = 1; lx <= 133; lx++) {
        int h = lkd_errno_to_darwin(lx);
        if (!h) continue;
        covered++;
        if (lkd_errno_from_darwin(h) != lx) { bad++; printf("roundtrip %d -> %d -> %d\n", lx, h, lkd_errno_from_darwin(h)); }
    }
    LT_EQ(bad, 0);
    LT_CHECK(covered > 80);
    LT_EQ(lkd_errno_to_darwin(0), 0);
    LT_EQ(lkd_errno_to_darwin(11), EAGAIN);
    LT_EQ(lkd_errno_to_darwin(95), EOPNOTSUPP);
    LT_EQ(lkd_errno_to_darwin(133), 0);   /* EHWPOISON has no Darwin twin */
    LT_STREQ(lkd_errno_name(2), "ENOENT");
    LT_STREQ(lkd_errno_name(38), "ENOSYS");
    LT_STREQ(lkd_errno_name(11), "EAGAIN");
    LT_STREQ(lkd_errno_name(110), "ETIMEDOUT");
    LT_STREQ(lkd_errno_name(512), "ERESTARTSYS");
    LT_CHECK(lkd_errno_name(0) == NULL);
    LT_CHECK(lkd_errno_name(4000) == NULL);
}

/* ---- open flags ---- */
LT_TEST(open_flags_table)
{
    int h; uint32_t un;
    /* access modes */
    LT_EQ(lkd_open_flags_to_host(0, &h, &un), 0); LT_EQ(h, O_RDONLY);
    LT_EQ(lkd_open_flags_to_host(1, &h, &un), 0); LT_EQ(h, O_WRONLY);
    LT_EQ(lkd_open_flags_to_host(2, &h, &un), 0); LT_EQ(h, O_RDWR);
    LT_ERRNO(lkd_open_flags_to_host(3, &h, &un), LX_EINVAL);
    /* one bit at a time: Linux x86-64 octal value, Darwin constant */
    struct { uint32_t lx; int host; } one[] = {
        { 0100, O_CREAT }, { 0200, O_EXCL }, { 0400, O_NOCTTY }, { 01000, O_TRUNC }, { 02000, O_APPEND },
        { 04000, O_NONBLOCK }, { 010000, O_DSYNC }, { 020000, O_ASYNC }, { 0200000, O_DIRECTORY },
        { 0400000, O_NOFOLLOW }, { 02000000, O_CLOEXEC }, { 04010000, O_SYNC },
    };
    for (size_t i = 0; i < sizeof one / sizeof one[0]; i++) {
        LT_EQ(lkd_open_flags_to_host(one[i].lx, &h, &un), 0);
        LT_EQ(h, one[i].host);
        LT_EQ(un, 0);
    }
    /* the numeric values differ from Darwin's: that is the reason for the table */
    LT_EQ(O_CREAT, 0x200); LT_EQ(O_APPEND, 0x8); LT_EQ(O_NONBLOCK, 0x4); LT_EQ(O_CLOEXEC, 0x1000000);
    /* untranslated bits */
    LT_EQ(lkd_open_flags_to_host(040000, &h, &un), 0); LT_EQ(un, 040000);      /* O_DIRECT */
    LT_EQ(lkd_open_flags_to_host(01000000, &h, &un), 0); LT_EQ(un, 01000000);  /* O_NOATIME */
    LT_EQ(lkd_open_flags_to_host(010000000, &h, &un), 0); LT_EQ(un, 010000000); /* O_PATH */
    LT_EQ(lkd_open_flags_to_host(020200000 | 2, &h, &un), 0);                  /* O_TMPFILE|O_RDWR */
    LT_EQ(un & 020200000, 020200000);
    LT_EQ(h & O_DIRECTORY, 0);                                                 /* not mistaken for O_DIRECTORY */
    LT_EQ(lkd_open_flags_to_host(0100000, &h, &un), 0); LT_EQ(un, 0);         /* O_LARGEFILE dropped */
    LT_EQ(lkd_open_flags_to_host(0x40000000u, &h, &un), 0); LT_EQ(un, 0x40000000u); /* unknown stays visible */
    /* a typical glibc open: O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC */
    LT_EQ(lkd_open_flags_to_host(2 | 0100 | 0200 | 02000000, &h, &un), 0);
    LT_EQ(h, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC);
}

LT_TEST(open_flags_work_with_the_real_open)
{
    char dir[] = "/tmp/lkd_xlate_XXXXXX";
    LT_CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/f", dir);
    int h; uint32_t un;
    LT_EQ(lkd_open_flags_to_host(2 | 0100 | 0200 | 02000000, &h, &un), 0);
    int fd = open(path, h, 0600);
    LT_CHECK(fd >= 0);
    LT_CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    int fd2 = open(path, h, 0600);
    LT_EQ(fd2, -1);
    LT_EQ(lkd_errno_from_darwin(errno), LX_EEXIST);      /* O_EXCL honoured, errno translated */
    close(fd);
    LT_EQ(lkd_open_flags_to_host(0 | 0200000, &h, &un), 0);
    fd = open(path, h);
    LT_EQ(fd, -1);
    LT_EQ(lkd_errno_from_darwin(errno), LX_ENOTDIR);     /* O_DIRECTORY honoured */
    LT_EQ(lkd_open_flags_to_host(1 | 02000, &h, &un), 0);
    fd = open(path, h);
    LT_CHECK(fd >= 0);
    LT_EQ(write(fd, "ab", 2), 2);
    int fl = fcntl(fd, F_GETFL);
    LT_EQ(lkd_open_flags_from_host(fl) & 3, 1);
    LT_CHECK(lkd_open_flags_from_host(fl) & 02000);       /* O_APPEND read back as Linux 02000 */
    LT_CHECK(lkd_open_flags_from_host(fl) & 0100000);     /* O_LARGEFILE shown as Linux x86-64 does */
    LT_EQ(lkd_open_flags_from_host(O_RDWR | O_NONBLOCK), 0100000 | 2 | 04000);
    close(fd);
    unlink(path);
    rmdir(dir);
}

/* ---- mmap ---- */
LT_TEST(mmap_flags_and_prot)
{
    int h; uint32_t un;
    LT_EQ(lkd_mmap_flags_to_host(0x02 | 0x20, &h, &un), 0);
    LT_EQ(h, MAP_PRIVATE | MAP_ANON); LT_EQ(un, 0);
    LT_EQ(lkd_mmap_flags_to_host(0x01 | 0x10, &h, &un), 0);
    LT_EQ(h, MAP_SHARED | MAP_FIXED);
    LT_EQ(lkd_mmap_flags_to_host(0x03, &h, &un), 0);              /* MAP_SHARED_VALIDATE */
    LT_EQ(h, MAP_SHARED);
    LT_EQ(lkd_mmap_flags_to_host(0x02 | 0x4000, &h, &un), 0);
    LT_EQ(h, MAP_PRIVATE | MAP_NORESERVE);
    LT_ERRNO(lkd_mmap_flags_to_host(0, &h, &un), LX_EINVAL);
    LT_ERRNO(lkd_mmap_flags_to_host(0x20, &h, &un), LX_EINVAL);
    LT_ERRNO(lkd_mmap_flags_to_host(0x0f | 0x20, &h, &un), LX_EINVAL);
    uint32_t leftover = 0x100 | 0x8000 | 0x20000 | 0x40 | 0x100000 | 0x800 | 0x1000 | 0x40000;
    LT_EQ(lkd_mmap_flags_to_host(0x02 | leftover, &h, &un), 0);
    LT_EQ(un, leftover);   /* GROWSDOWN POPULATE STACK 32BIT FIXED_NOREPLACE DENYWRITE EXECUTABLE HUGETLB */
    LT_EQ(h, MAP_PRIVATE);
    LT_EQ(lkd_prot_to_host(0), 0);
    LT_EQ(lkd_prot_to_host(1 | 2 | 4), PROT_READ | PROT_WRITE | PROT_EXEC);
    LT_EQ(lkd_prot_to_host(8 | 1), PROT_READ);
    LT_EQ(lkd_prot_to_host(0x01000000 | 3), PROT_READ | PROT_WRITE);
    LT_ERRNO(lkd_prot_to_host(0x10), LX_EINVAL);
    /* the translated flags make a working mapping */
    LT_EQ(lkd_mmap_flags_to_host(0x02 | 0x20, &h, &un), 0);
    void *p = mmap(NULL, 16384, lkd_prot_to_host(3), h, -1, 0);
    LT_CHECK(p != MAP_FAILED);
    if (p != MAP_FAILED) { ((char *)p)[100] = 1; munmap(p, 16384); }
}

/* ---- signals ---- */
LT_TEST(signal_numbers)
{
    struct { int lx, host; } m[] = {
        { 1, SIGHUP }, { 2, SIGINT }, { 3, SIGQUIT }, { 4, SIGILL }, { 5, SIGTRAP }, { 6, SIGABRT },
        { 7, SIGBUS }, { 8, SIGFPE }, { 9, SIGKILL }, { 10, SIGUSR1 }, { 11, SIGSEGV }, { 12, SIGUSR2 },
        { 13, SIGPIPE }, { 14, SIGALRM }, { 15, SIGTERM }, { 17, SIGCHLD }, { 18, SIGCONT }, { 19, SIGSTOP },
        { 20, SIGTSTP }, { 21, SIGTTIN }, { 22, SIGTTOU }, { 23, SIGURG }, { 24, SIGXCPU }, { 25, SIGXFSZ },
        { 26, SIGVTALRM }, { 27, SIGPROF }, { 28, SIGWINCH }, { 29, SIGIO }, { 31, SIGSYS },
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        if (lkd_signo_to_host(m[i].lx) != m[i].host) bad++;
        if (lkd_signo_from_host(m[i].host) != m[i].lx) bad++;
    }
    LT_EQ(bad, 0);
    /* the numbers that differ between the systems */
    LT_EQ(lkd_signo_to_host(7), 10);    /* SIGBUS */
    LT_EQ(lkd_signo_to_host(10), 30);   /* SIGUSR1 */
    LT_EQ(lkd_signo_to_host(12), 31);   /* SIGUSR2 */
    LT_EQ(lkd_signo_to_host(17), 20);   /* SIGCHLD */
    LT_EQ(lkd_signo_to_host(19), 17);   /* SIGSTOP */
    LT_EQ(lkd_signo_to_host(31), 12);   /* SIGSYS */
    LT_EQ(lkd_signo_to_host(16), 0);    /* SIGSTKFLT */
    LT_EQ(lkd_signo_to_host(30), 0);    /* SIGPWR */
    LT_EQ(lkd_signo_to_host(34), 0);    /* real-time */
    LT_EQ(lkd_signo_to_host(0), 0);
    LT_EQ(lkd_signo_from_host(SIGEMT), 0);
    LT_EQ(lkd_signo_from_host(SIGINFO), 0);
    LT_EQ(lkd_signo_from_host(0), 0);
}

/* ---- fcntl ---- */
LT_TEST(fcntl_commands)
{
    int h;
    LT_EQ(lkd_fcntl_cmd_to_host(0, &h), 0); LT_EQ(h, F_DUPFD);
    LT_EQ(lkd_fcntl_cmd_to_host(1, &h), 0); LT_EQ(h, F_GETFD);
    LT_EQ(lkd_fcntl_cmd_to_host(2, &h), 0); LT_EQ(h, F_SETFD);
    LT_EQ(lkd_fcntl_cmd_to_host(3, &h), 0); LT_EQ(h, F_GETFL);
    LT_EQ(lkd_fcntl_cmd_to_host(4, &h), 0); LT_EQ(h, F_SETFL);
    LT_EQ(lkd_fcntl_cmd_to_host(5, &h), 0); LT_EQ(h, F_GETLK);    /* Linux 5, Darwin 7 */
    LT_EQ(lkd_fcntl_cmd_to_host(6, &h), 0); LT_EQ(h, F_SETLK);    /* Linux 6, Darwin 8 */
    LT_EQ(lkd_fcntl_cmd_to_host(7, &h), 0); LT_EQ(h, F_SETLKW);   /* Linux 7, Darwin 9 */
    LT_EQ(lkd_fcntl_cmd_to_host(8, &h), 0); LT_EQ(h, F_SETOWN);   /* Linux 8, Darwin 6 */
    LT_EQ(lkd_fcntl_cmd_to_host(9, &h), 0); LT_EQ(h, F_GETOWN);   /* Linux 9, Darwin 5 */
    LT_EQ(lkd_fcntl_cmd_to_host(1030, &h), 0); LT_EQ(h, F_DUPFD_CLOEXEC);
    LT_EQ(F_GETLK, 7); LT_EQ(F_DUPFD_CLOEXEC, 67);
    LT_ERRNO(lkd_fcntl_cmd_to_host(1033, &h), LX_EINVAL);   /* F_ADD_SEALS: fs implements it */
    LT_ERRNO(lkd_fcntl_cmd_to_host(1031, &h), LX_EINVAL);   /* F_SETPIPE_SZ */
    LT_ERRNO(lkd_fcntl_cmd_to_host(1024, &h), LX_EINVAL);   /* F_SETLEASE */
    LT_ERRNO(lkd_fcntl_cmd_to_host(-5, &h), LX_EINVAL);
    int fd = open("/dev/null", O_RDONLY);
    int fh; lkd_fcntl_cmd_to_host(1030, &fh);
    int d = fcntl(fd, fh, 20);
    LT_CHECK(d >= 20);
    LT_CHECK(fcntl(d, F_GETFD) & FD_CLOEXEC);
    close(d); close(fd);
}

/* ---- sockets ---- */
LT_TEST(socket_constants)
{
    LT_EQ(lkd_af_to_host(1), AF_UNIX);
    LT_EQ(lkd_af_to_host(2), AF_INET);
    LT_EQ(lkd_af_to_host(10), AF_INET6);             /* Darwin 30 */
    LT_EQ(AF_INET6, 30);
    LT_ERRNO(lkd_af_to_host(16), LX_EAFNOSUPPORT);   /* AF_NETLINK */
    LT_ERRNO(lkd_af_to_host(17), LX_EAFNOSUPPORT);   /* AF_PACKET */
    LT_ERRNO(lkd_af_to_host(999), LX_EAFNOSUPPORT);
    LT_EQ(lkd_af_from_host(AF_INET6), 10);
    LT_EQ(lkd_af_from_host(AF_UNIX), 1);
    LT_ERRNO(lkd_af_from_host(AF_ROUTE), LX_EAFNOSUPPORT);
    for (int t = 1; t <= 5; t++) LT_EQ(lkd_socktype_to_host(t), t);
    LT_ERRNO(lkd_socktype_to_host(0), LX_EINVAL);
    LT_ERRNO(lkd_socktype_to_host(6), LX_EINVAL);
    int hl, ho;
    LT_EQ(lkd_sockopt_to_host(1, 2, &hl, &ho), 0);   /* SOL_SOCKET, SO_REUSEADDR */
    LT_EQ(hl, SOL_SOCKET); LT_EQ(ho, SO_REUSEADDR);
    LT_EQ(SOL_SOCKET, 0xffff);
    LT_EQ(lkd_sockopt_to_host(1, 7, &hl, &ho), 0); LT_EQ(ho, SO_SNDBUF);
    LT_EQ(lkd_sockopt_to_host(1, 8, &hl, &ho), 0); LT_EQ(ho, SO_RCVBUF);
    LT_EQ(lkd_sockopt_to_host(1, 3, &hl, &ho), 0); LT_EQ(ho, SO_TYPE);
    LT_EQ(lkd_sockopt_to_host(1, 4, &hl, &ho), 0); LT_EQ(ho, SO_ERROR);
    LT_EQ(lkd_sockopt_to_host(1, 15, &hl, &ho), 0); LT_EQ(ho, SO_REUSEPORT);
    LT_EQ(lkd_sockopt_to_host(1, 20, &hl, &ho), 0); LT_EQ(ho, SO_RCVTIMEO);
    LT_ERRNO(lkd_sockopt_to_host(1, 17, &hl, &ho), LX_ENOPROTOOPT);   /* SO_PEERCRED: net implements it */
    LT_ERRNO(lkd_sockopt_to_host(1, 999, &hl, &ho), LX_ENOPROTOOPT);
    LT_EQ(lkd_sockopt_to_host(6, 1, &hl, &ho), 0);   /* TCP_NODELAY */
    LT_EQ(hl, IPPROTO_TCP); LT_EQ(ho, TCP_NODELAY);
    LT_EQ(lkd_sockopt_to_host(6, 4, &hl, &ho), 0); LT_EQ(ho, TCP_KEEPALIVE);   /* TCP_KEEPIDLE */
    LT_EQ(lkd_sockopt_to_host(6, 5, &hl, &ho), 0); LT_EQ(ho, TCP_KEEPINTVL);
    LT_EQ(lkd_sockopt_to_host(6, 6, &hl, &ho), 0); LT_EQ(ho, TCP_KEEPCNT);
    LT_EQ(lkd_sockopt_to_host(0, 1, &hl, &ho), 0); LT_EQ(hl, IPPROTO_IP); LT_EQ(ho, IP_TOS);
    LT_EQ(lkd_sockopt_to_host(0, 2, &hl, &ho), 0); LT_EQ(ho, IP_TTL);
    LT_EQ(lkd_sockopt_to_host(41, 26, &hl, &ho), 0); LT_EQ(hl, IPPROTO_IPV6); LT_EQ(ho, IPV6_V6ONLY);
    LT_ERRNO(lkd_sockopt_to_host(12345, 1, &hl, &ho), LX_ENOPROTOOPT);
    int ll, lo;
    LT_EQ(lkd_sockopt_from_host(SOL_SOCKET, SO_REUSEADDR, &ll, &lo), 0); LT_EQ(ll, 1); LT_EQ(lo, 2);
    LT_EQ(lkd_sockopt_from_host(IPPROTO_TCP, TCP_KEEPALIVE, &ll, &lo), 0); LT_EQ(ll, 6); LT_EQ(lo, 4);
    LT_ERRNO(lkd_sockopt_from_host(SOL_SOCKET, 0x7777, &ll, &lo), LX_ENOPROTOOPT);
    /* works on a real socket: set SO_REUSEADDR with the translated pair and read it back */
    int s = socket(lkd_af_to_host(2), lkd_socktype_to_host(1), 0);
    LT_CHECK(s >= 0);
    LT_EQ(lkd_sockopt_to_host(1, 2, &hl, &ho), 0);
    int one = 1, got = 0; socklen_t len = sizeof got;
    LT_EQ(setsockopt(s, hl, ho, &one, sizeof one), 0);
    LT_EQ(getsockopt(s, hl, ho, &got, &len), 0);
    LT_CHECK(got != 0);
    LT_EQ(lkd_sockopt_to_host(1, 3, &hl, &ho), 0);
    LT_EQ(getsockopt(s, hl, ho, &got, &len), 0);
    LT_EQ(got, SOCK_STREAM);
    close(s);
}

LT_TEST(message_flags)
{
    int un;
    LT_EQ(lkd_msgflags_to_host(0x40, &un), MSG_DONTWAIT); LT_EQ(un, 0);   /* Linux 0x40, Darwin 0x80 */
    LT_EQ(MSG_DONTWAIT, 0x80);
    LT_EQ(lkd_msgflags_to_host(0x2, &un), MSG_PEEK);
    LT_EQ(lkd_msgflags_to_host(0x100, &un), MSG_WAITALL);                 /* Darwin 0x40 */
    LT_EQ(lkd_msgflags_to_host(0x1, &un), MSG_OOB);
    LT_EQ(lkd_msgflags_to_host(0x80, &un), MSG_EOR);                       /* Darwin 8 */
    LT_EQ(lkd_msgflags_to_host(0x4000 | 0x40, &un), MSG_DONTWAIT);         /* MSG_NOSIGNAL stays visible */
    LT_EQ(un, 0x4000);
    LT_EQ(lkd_msgflags_to_host(0x20 | 0x8, &un), MSG_TRUNC | MSG_CTRUNC);
    LT_EQ(lkd_msgflags_from_host(MSG_TRUNC | MSG_CTRUNC | MSG_EOR), 0x20 | 0x8 | 0x80);
    LT_EQ(lkd_msgflags_from_host(MSG_DONTWAIT | MSG_PEEK), 0x40 | 0x2);
}

/* ---- device numbers and stat ---- */
LT_TEST(device_numbers)
{
    /* Linux new_encode_dev: (minor & 0xff) | (major << 8) | ((minor & ~0xff) << 12) */
    LT_EQ(lkd_dev_to_linux(makedev(1, 3)), 0x103);          /* /dev/null on Linux is 1:3 */
    LT_EQ(lkd_dev_to_linux(makedev(8, 1)), 0x801);
    LT_EQ(lkd_dev_to_linux(makedev(255, 0)), 255u << 8);   /* Darwin majors are 8 bits */
    LT_EQ(lkd_dev_to_linux(makedev(1, 0x1234)), 0x34 | (1 << 8) | (0x1200ull << 12));
    LT_EQ(lkd_dev_to_linux(0), 0);
    dev_t samples[] = { makedev(1, 3), makedev(8, 1), makedev(12, 255), makedev(20, 256), makedev(100, 70000) };
    for (size_t i = 0; i < sizeof samples / sizeof samples[0]; i++)
        LT_EQ(lkd_dev_from_linux(lkd_dev_to_linux(samples[i])), samples[i]);
}

static uint64_t rd64(const void *buf, size_t off) { uint64_t v; memcpy(&v, (const char *)buf + off, 8); return v; }
static uint32_t rd32(const void *buf, size_t off) { uint32_t v; memcpy(&v, (const char *)buf + off, 4); return v; }

LT_TEST(stat_layout_matches_linux_x86_64_byte_for_byte)
{
    /* arch/x86/include/uapi/asm/stat.h struct stat, 144 bytes */
    struct stat hs;
    memset(&hs, 0, sizeof hs);
    hs.st_dev = makedev(1, 2);
    hs.st_ino = 0x1122334455667788ull;
    hs.st_nlink = 3;
    hs.st_mode = S_IFREG | 0644;
    hs.st_uid = 501; hs.st_gid = 20;
    hs.st_rdev = makedev(5, 6);
    hs.st_size = 4097; hs.st_blksize = 4096; hs.st_blocks = 16;
    hs.st_atimespec = (struct timespec){ 1000, 1 };
    hs.st_mtimespec = (struct timespec){ 2000, 2 };
    hs.st_ctimespec = (struct timespec){ 3000, 3 };
    hs.st_birthtimespec = (struct timespec){ 4000, 4 };
    struct lx_stat ls;
    memset(&ls, 0xaa, sizeof ls);
    lkd_stat_from_host(&hs, &ls);
    LT_EQ(sizeof ls, 144);
    LT_EQ(rd64(&ls, 0), 0x102);          /* st_dev (1<<8 | 2) */
    LT_EQ(rd64(&ls, 8), 0x1122334455667788ull);
    LT_EQ(rd64(&ls, 16), 3);             /* st_nlink is 64-bit on x86-64 */
    LT_EQ(rd32(&ls, 24), S_IFREG | 0644);
    LT_EQ(rd32(&ls, 28), 501);
    LT_EQ(rd32(&ls, 32), 20);
    LT_EQ(rd32(&ls, 36), 0);             /* __pad0 */
    LT_EQ(rd64(&ls, 40), 0x506);         /* st_rdev */
    LT_EQ(rd64(&ls, 48), 4097);
    LT_EQ(rd64(&ls, 56), 4096);
    LT_EQ(rd64(&ls, 64), 16);
    LT_EQ(rd64(&ls, 72), 1000); LT_EQ(rd64(&ls, 80), 1);
    LT_EQ(rd64(&ls, 88), 2000); LT_EQ(rd64(&ls, 96), 2);
    LT_EQ(rd64(&ls, 104), 3000); LT_EQ(rd64(&ls, 112), 3);
    LT_EQ(rd64(&ls, 120), 0); LT_EQ(rd64(&ls, 128), 0); LT_EQ(rd64(&ls, 136), 0);   /* __unused[3] */
}

LT_TEST(stat_of_real_files)
{
    struct stat hs; struct lx_stat ls;
    LT_EQ(stat("/dev/null", &hs), 0);
    lkd_stat_from_host(&hs, &ls);
    LT_EQ(ls.st_mode & LX_S_IFMT, LX_S_IFCHR);
    LT_EQ(ls.st_mode, hs.st_mode);        /* file type bits have the same values */
    LT_EQ(ls.st_rdev, lkd_dev_to_linux(hs.st_rdev));
    LT_EQ(stat("/", &hs), 0);
    lkd_stat_from_host(&hs, &ls);
    LT_EQ(ls.st_mode & LX_S_IFMT, LX_S_IFDIR);
    LT_EQ(ls.st_ino, hs.st_ino);
    LT_EQ(ls.st_nlink, hs.st_nlink);
    LT_EQ(S_IFLNK, 0120000); LT_EQ(S_IFSOCK, 0140000); LT_EQ(S_IFIFO, 0010000);   /* identical to Linux */
    char dir[] = "/tmp/lkd_stat_XXXXXX";
    LT_CHECK(mkdtemp(dir) != NULL);
    char p[128], l[128];
    snprintf(p, sizeof p, "%s/file", dir);
    snprintf(l, sizeof l, "%s/link", dir);
    int fd = open(p, O_CREAT | O_WRONLY, 0640);
    LT_EQ(write(fd, "0123456789", 10), 10);
    close(fd);
    LT_EQ(symlink("file", l), 0);
    LT_EQ(lstat(l, &hs), 0);
    lkd_stat_from_host(&hs, &ls);
    LT_EQ(ls.st_mode & LX_S_IFMT, LX_S_IFLNK);
    LT_EQ(ls.st_size, 4);
    LT_EQ(stat(p, &hs), 0);
    lkd_stat_from_host(&hs, &ls);
    LT_EQ(ls.st_size, 10);
    LT_EQ(ls.st_mode & 07777, 0640);
    LT_EQ(ls.st_uid, getuid());
    LT_EQ(ls.st_mtime_sec, hs.st_mtimespec.tv_sec);
    LT_EQ(ls.st_mtime_nsec, hs.st_mtimespec.tv_nsec);
    struct lx_statx x;
    lkd_statx_from_host(&hs, 0xfff, &x);
    LT_EQ(sizeof x, 256);
    LT_EQ(x.stx_size, 10);
    LT_EQ(x.stx_mode, hs.st_mode);
    LT_EQ(x.stx_nlink, 1);
    LT_EQ(x.stx_ino, hs.st_ino);
    LT_EQ(x.stx_mtime.tv_sec, hs.st_mtimespec.tv_sec);
    LT_EQ(x.stx_btime.tv_sec, hs.st_birthtimespec.tv_sec);
    LT_EQ(x.stx_mask & 0x7ff, 0x7ff);
    LT_CHECK(x.stx_mask & 0x800);        /* STATX_BTIME */
    LT_EQ(x.stx_blksize, hs.st_blksize);
    unlink(p); unlink(l); rmdir(dir);
}

LT_TEST(statfs_and_dtype)
{
    struct statfs hs; struct lx_statfs ls;
    LT_EQ(statfs("/", &hs), 0);
    lkd_statfs_from_host(&hs, &ls);
    LT_EQ(sizeof ls, 120);
    LT_EQ(ls.f_bsize, hs.f_bsize);
    LT_EQ(ls.f_blocks, hs.f_blocks);
    LT_EQ(ls.f_bfree, hs.f_bfree);
    LT_EQ(ls.f_bavail, hs.f_bavail);
    LT_EQ(ls.f_namelen, 255);
    if (!strcmp(hs.f_fstypename, "apfs")) LT_EQ(ls.f_type, 0x42535041);
    else LT_CHECK(1);
    LT_EQ(ls.f_flags & 1, (hs.f_flags & MNT_RDONLY) ? 1 : 0);
    LT_EQ(lkd_dtype_from_host(DT_DIR), 4);
    LT_EQ(lkd_dtype_from_host(DT_REG), 8);
    LT_EQ(lkd_dtype_from_host(DT_LNK), 10);
    LT_EQ(lkd_dtype_from_host(DT_CHR), 2);
    LT_EQ(lkd_dtype_from_host(DT_BLK), 6);
    LT_EQ(lkd_dtype_from_host(DT_FIFO), 1);
    LT_EQ(lkd_dtype_from_host(DT_SOCK), 12);
    LT_EQ(lkd_dtype_from_host(DT_UNKNOWN), 0);
    LT_EQ(lkd_dtype_from_host(DT_WHT), 0);
}

int main(void)
{
    LT_RUN(errno_darwin_to_linux);
    LT_RUN(errno_roundtrip_and_names);
    LT_RUN(open_flags_table);
    LT_RUN(open_flags_work_with_the_real_open);
    LT_RUN(mmap_flags_and_prot);
    LT_RUN(signal_numbers);
    LT_RUN(fcntl_commands);
    LT_RUN(socket_constants);
    LT_RUN(message_flags);
    LT_RUN(device_numbers);
    LT_RUN(stat_layout_matches_linux_x86_64_byte_for_byte);
    LT_RUN(stat_of_real_files);
    LT_RUN(statfs_and_dtype);
    return lt_finish();
}
