/*
 * f5_scm_rights.c - F: SCM_RIGHTS on AF_UNIX (which descriptors can travel, how many per message, flags of the
 * received descriptors, MSG_CTRUNC leaks, in-flight lifetime, cross-process use of a received shared-memory fd),
 * peer credentials (LOCAL_PEERCRED family, SCM_CREDS) and fileport (fd <-> Mach port) conversion.
 */
#include "fg_util.h"
#include <libproc.h>
#include <mach/mach_vm.h>
#include <sys/fileport.h>
#include <sys/ucred.h>
#include <util.h>

/* the SDK only forward-declares struct fg_cmsgcred; this is the FreeBSD layout (84 bytes) */
struct fg_cmsgcred { pid_t pid; uid_t uid; uid_t euid; gid_t gid; short ngroups; gid_t groups[16]; };

/* counts the descriptors that are really open (the size-only query of PROC_PIDLISTFDS follows the table capacity, not the open count) */
static int count_open_fds(void)
{
    enum { CAP = 16384 };
    struct proc_fdinfo *buf = malloc(CAP * sizeof *buf);
    int sz = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, buf, CAP * (int)sizeof *buf);
    free(buf);
    return sz > 0 ? sz / (int)sizeof(struct proc_fdinfo) : -1;
}

static int send_fds(int sock, const int *fds, int n, const void *data, size_t len, int flags)
{
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    struct iovec iov = { (void *)data, len };
    size_t cl = CMSG_SPACE(n * sizeof(int));
    char *ctl = calloc(1, cl);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = cl;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(c), fds, n * sizeof(int));
    ssize_t r = sendmsg(sock, &msg, flags);
    int e = errno;
    free(ctl);
    errno = e;
    return (int)r;
}

static unsigned g_last_controllen, g_last_cmsg_len;

/* returns bytes received (or -1), number of fds in *nfds, msg_flags in *mflags */
static int recv_fds(int sock, int *fds, int maxfds, void *buf, size_t blen, int *nfds, int flags, int *mflags)
{
    struct msghdr msg;
    memset(&msg, 0, sizeof msg);
    struct iovec iov = { buf, blen };
    size_t cl = CMSG_SPACE(maxfds * sizeof(int));
    char *ctl = calloc(1, cl);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = cl;
    ssize_t r = recvmsg(sock, &msg, flags);
    int e = errno;
    *nfds = 0;
    if (mflags) *mflags = msg.msg_flags;
    g_last_controllen = msg.msg_controllen;
    g_last_cmsg_len = 0;
    if (r >= 0) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                g_last_cmsg_len = c->cmsg_len;
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                /* never trust cmsg_len beyond what the kernel returned in msg_controllen or what the caller has room for */
                size_t avail = ((char *)ctl + msg.msg_controllen > (char *)CMSG_DATA(c)) ? (size_t)((char *)ctl + msg.msg_controllen - (char *)CMSG_DATA(c)) / sizeof(int) : 0;
                if ((size_t)k > avail) k = (int)avail;
                if (*nfds + k > maxfds) k = maxfds - *nfds;
                memcpy(fds + *nfds, CMSG_DATA(c), k * sizeof(int));
                *nfds += k;
            }
        }
    }
    free(ctl);
    errno = e;
    return (int)r;
}

/* ---- S1: which descriptors can be passed ------------------------------------------------ */
struct kind { const char *name; int fd; int extra_fd; };

static void s1_types(void)
{
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    char *tmp = fg_tmpname("f5reg");
    int reg = mkstemp(tmp);
    ssize_t w = write(reg, "0123456789", 10);
    (void)w;
    lseek(reg, 3, SEEK_SET);
    int pipes[2];
    pipe(pipes);
    char shname[32];
    snprintf(shname, sizeof shname, "/fg5-%d", (int)getpid());
    int shm = shm_open(shname, O_CREAT | O_EXCL | O_RDWR, 0600);
    shm_unlink(shname);
    ftruncate(shm, FG_PAGE);
    char *tmp2 = fg_tmpname("f5unl");
    int unl = mkstemp(tmp2);
    unlink(tmp2);
    int dirfd = open("/private/tmp", O_RDONLY | O_DIRECTORY);
    int sock2[2];
    socketpair(AF_UNIX, SOCK_DGRAM, 0, sock2);
    int kq = kqueue();
    int devnull = open("/dev/null", O_RDWR);
    int evt = open(tmp, O_EVTONLY);
    int sym = open("/private/tmp", O_RDONLY | O_SYMLINK);
    int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    char spath[200];
    snprintf(spath, sizeof spath, "%s/f5l-%d.sock", dp_tmpdir(), (int)getpid());
    unlink(spath);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof sun);
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof sun.sun_path, "%s", spath);
    int lbind = bind(lsock, (struct sockaddr *)&sun, sizeof sun);
    int llisten = listen(lsock, 4);
    int tcp = socket(AF_INET, SOCK_STREAM, 0);
    int master = -1, slave = -1;
    int pty = openpty(&master, &slave, NULL, NULL, NULL);
    struct kind kinds[] = {
        { "regular file", reg, 0 }, { "pipe read end", pipes[0], 0 }, { "pipe write end", pipes[1], 0 },
        { "shm_open fd (unlinked)", shm, 0 }, { "mkstemp fd (unlinked)", unl, 0 }, { "directory (O_DIRECTORY)", dirfd, 0 },
        { "AF_UNIX dgram socket", sock2[0], 0 }, { "kqueue", kq, 0 }, { "/dev/null", devnull, 0 },
        { "O_EVTONLY file", evt, 0 }, { "O_SYMLINK directory", sym, 0 }, { "listening AF_UNIX socket", lsock, 0 },
        { "unconnected TCP socket", tcp, 0 }, { "pty master", pty == 0 ? master : -1, 0 }, { "pty slave", pty == 0 ? slave : -1, 0 },
    };
    INFO("F5.s1", "setup: bind=%d listen=%d openpty=%d", lbind, llisten, pty);
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        int fd = kinds[i].fd;
        if (fd < 0) { INFO("F5.s1", "%-28s not created", kinds[i].name); continue; }
        struct stat s0;
        fstat(fd, &s0);
        int r = send_fds(sp[0], &fd, 1, "x", 1, 0);
        int e = errno;
        if (r < 0) { FAIL("F5.s1", "%-28s sendmsg failed: %s", kinds[i].name, ERRS(e)); continue; }
        int got[4], n = 0, mf = 0;
        char b;
        int rr = recv_fds(sp[1], got, 4, &b, 1, &n, MSG_DONTWAIT, &mf);
        if (rr < 0 || n != 1) { FAIL("F5.s1", "%-28s recvmsg failed: r=%d n=%d %s", kinds[i].name, rr, n, ERRS(errno)); continue; }
        struct stat s1;
        fstat(got[0], &s1);
        int fl = fcntl(got[0], F_GETFD);
        int same = (s0.st_mode & S_IFMT) == (s1.st_mode & S_IFMT) && s0.st_ino == s1.st_ino && s0.st_dev == s1.st_dev;
        CHECK(same, "F5.s1", "%-28s passes, same object on arrival (fd %d -> %d, mode 0%o, FD flags on arrival %d)", kinds[i].name, fd, got[0], s1.st_mode & S_IFMT, fl);
        close(got[0]);
    }
    /* semantic checks on a few kinds */
    {
        int fd = reg;
        send_fds(sp[0], &fd, 1, "x", 1, 0);
        int got[2], n, mf;
        char b;
        recv_fds(sp[1], got, 2, &b, 1, &n, MSG_DONTWAIT, &mf);
        lseek(got[0], 7, SEEK_SET);
        off_t sender_off = lseek(reg, 0, SEEK_CUR);
        CHECK(sender_off == 7, "F5.s1", "regular file: the received fd shares the open file description (offset set to 7 via the copy, sender sees %lld)", (long long)sender_off);
        close(got[0]);
    }
    {
        int fd = lsock;
        send_fds(sp[0], &fd, 1, "x", 1, 0);
        int got[2], n, mf;
        char b;
        recv_fds(sp[1], got, 2, &b, 1, &n, MSG_DONTWAIT, &mf);
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        int cr = connect(cs, (struct sockaddr *)&sun, sizeof sun);
        int ac = accept(got[0], NULL, NULL);
        CHECK(cr == 0 && ac >= 0, "F5.s1", "listening socket: connect() to its path is accepted through the received fd (connect=%d accept=%d)", cr, ac);
        if (ac >= 0) close(ac);
        close(cs);
        close(got[0]);
    }
    {
        int fd = kq;
        int sr = send_fds(sp[0], &fd, 1, "x", 1, MSG_DONTWAIT);
        int got[2], n = 0, mf;
        char b;
        if (sr < 0) {
            INFO("F5.s1", "kqueue: sendmsg(SCM_RIGHTS) of a kqueue fd is refused (%s), so an epoll-like object cannot be passed to another process", ERRS(errno));
            goto skip_kq;
        }
        recv_fds(sp[1], got, 2, &b, 1, &n, MSG_DONTWAIT, &mf);
        int pp[2];
        pipe(pp);
        struct kevent ev;
        EV_SET(&ev, pp[0], EVFILT_READ, EV_ADD, 0, 0, NULL);
        int rc = kevent(got[0], &ev, 1, NULL, 0, NULL);
        ssize_t ww = write(pp[1], "z", 1);
        (void)ww;
        struct kevent out;
        struct timespec ts = { 1, 0 };
        int nev = kevent(kq, NULL, 0, &out, 1, &ts);
        CHECK(rc == 0 && nev == 1, "F5.s1", "kqueue: a registration made through the received fd is visible through the original kqueue fd (register=%d events=%d)", rc, nev);
        close(pp[0]); close(pp[1]); close(got[0]);
    skip_kq:;
    }
    unlink(tmp); free(tmp); free(tmp2); unlink(spath);
    close(sp[0]); close(sp[1]);
}

/* ---- S2/S3: flags, counts, truncation, in-flight lifetime --------------------------------- */
static void s2_flags_and_counts(void)
{
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    int f = open("/dev/null", O_RDONLY | O_CLOEXEC | O_CLOFORK);
    send_fds(sp[0], &f, 1, "x", 1, 0);
    int got[2], n, mf;
    char b;
    recv_fds(sp[1], got, 2, &b, 1, &n, MSG_DONTWAIT, &mf);
    int fl = fcntl(got[0], F_GETFD);
    INFO("F5.s2", "sent an fd with FD_CLOEXEC|FD_CLOFORK; received fd has fdflags=%d (0 means neither flag is transmitted and none is added)", fl);
    CHECK(fl == 0, "F5.s2", "received descriptors carry no FD_CLOEXEC/FD_CLOFORK: the receiver must set them itself");
    close(got[0]);
    /* Linux MSG_CMSG_CLOEXEC value */
    send_fds(sp[0], &f, 1, "x", 1, 0);
    int r = recv_fds(sp[1], got, 2, &b, 1, &n, 0x40000000, &mf);
    INFO("F5.s2", "recvmsg with the Linux MSG_CMSG_CLOEXEC value 0x40000000: result %d errno %s, fds=%d flags of the first=%d", r, r < 0 ? ERRS(errno) : "-", n, n ? fcntl(got[0], F_GETFD) : -1);
    if (n) close(got[0]);
    if (r < 0) { /* drain */ recv_fds(sp[1], got, 2, &b, 1, &n, MSG_DONTWAIT, &mf); if (n) close(got[0]); }

    /* how many fds in one message */
    int counts[] = { 1, 2, 4, 8, 16, 32, 64, 100, 127, 128, 200, 253, 254, 255, 256, 300, 400, 500, 508, 509, 510, 512, 600, 1000, 1024, 2048, 4096 };
    int maxok = 0, firstfail = 0;
    char fe[48] = "";
    int dupfd = open("/dev/null", O_RDONLY);
    int before = count_open_fds();
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        int nn = counts[k];
        int *arr = malloc(nn * sizeof(int));
        for (int i = 0; i < nn; i++) arr[i] = dupfd;
        int rs = send_fds(sp[0], arr, nn, "x", 1, MSG_DONTWAIT);
        int es = errno;
        if (rs < 0) {
            if (!firstfail) { firstfail = nn; snprintf(fe, sizeof fe, "%s", ERRS(es)); }
            free(arr);
            continue;
        }
        int *rcv = malloc(nn * sizeof(int));
        int nr = 0;
        recv_fds(sp[1], rcv, nn, &b, 1, &nr, MSG_DONTWAIT, &mf);
        for (int i = 0; i < nr; i++) close(rcv[i]);
        if (nr == nn && !firstfail) maxok = nn;
        if (nr != nn) INFO("F5.s3", "%d fds sent, %d received, msg_flags=0x%x", nn, nr, mf);
        free(rcv);
        free(arr);
    }
    INFO("F5.s3", "SCM_RIGHTS fds per message over AF_UNIX stream: largest count that worked in the sweep = %d; first count refused = %d with %s", maxok, firstfail, firstfail ? fe : "(none refused)");
    int after = count_open_fds();
    CHECK(after == before, "F5.s3", "no descriptor leaked by the sweep (open fds %d -> %d)", before, after);

    /* find the exact limit between maxok and firstfail */
    if (firstfail && maxok) {
        int lo = maxok, hi = firstfail;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            int *arr = malloc(mid * sizeof(int));
            for (int i = 0; i < mid; i++) arr[i] = dupfd;
            int rs = send_fds(sp[0], arr, mid, "x", 1, MSG_DONTWAIT);
            if (rs >= 0) {
                int *rcv = malloc(mid * sizeof(int));
                int nr = 0;
                recv_fds(sp[1], rcv, mid, &b, 1, &nr, MSG_DONTWAIT, &mf);
                for (int i = 0; i < nr; i++) close(rcv[i]);
                free(rcv);
                lo = mid;
            } else hi = mid;
            free(arr);
        }
        INFO("F5.s3", "exact limit by bisection: %d fds per sendmsg succeed, %d fail (control length %zu bytes for the largest success)", lo, lo + 1, (size_t)CMSG_SPACE(lo * sizeof(int)));
    }

    /* MSG_CTRUNC: send 10 fds, receive with room for 3 */
    {
        int arr[10];
        for (int i = 0; i < 10; i++) arr[i] = dupfd;
        int b0 = count_open_fds();
        send_fds(sp[0], arr, 10, "x", 1, 0);
        int rcv[3], nr = 0;
        recv_fds(sp[1], rcv, 3, &b, 1, &nr, MSG_DONTWAIT, &mf);
        int b1 = count_open_fds();
        INFO("F5.s3", "10 fds sent, receiver control buffer for 3: got %d fds, MSG_CTRUNC=%d, kernel returned msg_controllen=%u cmsg_len=%u (room given %zu), open fds %d -> %d (a leak would show 10 new fds)",
             nr, !!(mf & MSG_CTRUNC), g_last_controllen, g_last_cmsg_len, (size_t)CMSG_SPACE(3 * sizeof(int)), b0, b1);
        for (int i = 0; i < nr; i++) close(rcv[i]);
        int b2 = count_open_fds();
        CHECK(b2 == b0, "F5.s3", "Linux closes the fds that do not fit in the control buffer; here open fds before the receive %d, after closing the 3 reported ones %d (a difference means the kernel installed the unreported fds in this process: %d stray descriptors)", b0, b2, b2 - b0);
    }

    /* zero-length payload with ancillary data */
    {
        int fd1 = dupfd;
        int rs = send_fds(sp[0], &fd1, 1, "", 0, MSG_DONTWAIT);
        int es = errno;
        INFO("F5.s3", "stream socket: sendmsg of 0 data bytes + 1 fd: %s", rs < 0 ? ERRS(es) : "accepted");
        if (rs >= 0) {
            int rcv[2], nr = 0;
            int rr = recv_fds(sp[1], rcv, 2, &b, 1, &nr, MSG_DONTWAIT, &mf);
            INFO("F5.s3", "  receive: r=%d nfds=%d errno=%s", rr, nr, rr < 0 ? ERRS(errno) : "-");
            if (nr) close(rcv[0]);
        }
        int dg[2];
        socketpair(AF_UNIX, SOCK_DGRAM, 0, dg);
        rs = send_fds(dg[0], &fd1, 1, "", 0, MSG_DONTWAIT);
        es = errno;
        INFO("F5.s3", "dgram socket: sendmsg of 0 data bytes + 1 fd: %s", rs < 0 ? ERRS(es) : "accepted");
        if (rs >= 0) {
            int rcv[2], nr = 0;
            int rr = recv_fds(dg[1], rcv, 2, &b, 1, &nr, MSG_DONTWAIT, &mf);
            INFO("F5.s3", "  receive: r=%d nfds=%d errno=%s", rr, nr, rr < 0 ? ERRS(errno) : "-");
            if (nr) close(rcv[0]);
        }
        /* many fds over dgram */
        int arr[300];
        for (int i = 0; i < 300; i++) arr[i] = dupfd;
        int big_ok = 0;
        for (int nn = 1; nn <= 300; nn = nn < 8 ? nn + 1 : nn * 2) {
            rs = send_fds(dg[0], arr, nn, "x", 1, MSG_DONTWAIT);
            es = errno;
            if (rs < 0) { INFO("F5.s3", "dgram socket: %d fds in one message: %s", nn, ERRS(es)); break; }
            int rcv[300], nr = 0;
            recv_fds(dg[1], rcv, 300, &b, 1, &nr, MSG_DONTWAIT, &mf);
            for (int i = 0; i < nr; i++) close(rcv[i]);
            big_ok = nn;
        }
        INFO("F5.s3", "dgram socket: largest count tried and accepted in the doubling sweep = %d", big_ok);
        {
            int lo = big_ok, hi = 300;
            while (hi - lo > 1) {
                int mid = (lo + hi) / 2;
                int rs2 = send_fds(dg[0], arr, mid, "x", 1, MSG_DONTWAIT);
                if (rs2 >= 0) {
                    int rcv[300], nr = 0;
                    recv_fds(dg[1], rcv, 300, &b, 1, &nr, MSG_DONTWAIT, &mf);
                    for (int i = 0; i < nr; i++) close(rcv[i]);
                    lo = mid;
                } else hi = mid;
            }
            INFO("F5.s3", "dgram socket: exact limit by bisection: %d fds per sendmsg succeed, %d fail", lo, lo + 1);
        }
        close(dg[0]); close(dg[1]);
    }

    /* in-flight lifetime: close the sender's copy before the receiver reads */
    {
        char shname[32];
        snprintf(shname, sizeof shname, "/fg5b-%d", (int)getpid());
        int shm = shm_open(shname, O_CREAT | O_EXCL | O_RDWR, 0600);
        shm_unlink(shname);
        ftruncate(shm, FG_PAGE);
        unsigned char *m = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
        m[0] = 0x5a;
        send_fds(sp[0], &shm, 1, "x", 1, 0);
        close(shm);
        int rcv[2], nr = 0;
        recv_fds(sp[1], rcv, 2, &b, 1, &nr, MSG_DONTWAIT, &mf);
        unsigned char *m2 = nr ? mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, rcv[0], 0) : MAP_FAILED;
        CHECK(nr == 1 && m2 != MAP_FAILED && m2[0] == 0x5a, "F5.s3", "an fd closed by the sender while in flight still arrives and maps the same shared memory (%s)", nr ? "mapped" : "no fd");
        if (m2 != MAP_FAILED) munmap(m2, FG_PAGE);
        munmap(m, FG_PAGE);
        if (nr) close(rcv[0]);
    }
    close(dupfd);
    close(sp[0]);
    close(sp[1]);
}

/* ---- S4: cross-process: forked child receives a shm fd and writes ----------------------------- */
static int s4_child(void *arg)
{
    int sock = *(int *)arg;
    int rcv[4], nr = 0, mf;
    char b;
    int r = recv_fds(sock, rcv, 4, &b, 1, &nr, 0, &mf);
    if (r < 0 || nr != 1) return 2;
    unsigned char *m = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, rcv[0], 0);
    if (m == MAP_FAILED) return 3;
    int ok = (m[0] == 0x11);
    m[1] = 0x22;
    /* send an fd back: the write end of a pipe */
    int pp[2];
    pipe(pp);
    send_fds(sock, &pp[1], 1, "y", 1, 0);
    ssize_t w = write(pp[1], "hello", 5);
    (void)w;
    return ok ? 0 : 4;
}

static void s4_cross_process(void)
{
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    char shname[32];
    snprintf(shname, sizeof shname, "/fg5c-%d", (int)getpid());
    int shm = shm_open(shname, O_CREAT | O_EXCL | O_RDWR, 0600);
    shm_unlink(shname);
    ftruncate(shm, FG_PAGE);
    unsigned char *m = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    m[0] = 0x11;
    pid_t pid = fork();
    if (pid == 0) {
        dp_role = "child";
        dp_watchdog(20);
        close(sp[0]);
        _exit(s4_child(&sp[1]));
    }
    dp_track(pid);
    close(sp[1]);
    send_fds(sp[0], &shm, 1, "x", 1, 0);
    int rcv[2], nr = 0, mf;
    char b;
    int rr = recv_fds(sp[0], rcv, 2, &b, 1, &nr, 0, &mf);
    char buf[16] = { 0 };
    ssize_t n = nr ? read(rcv[0] - 0, buf, 0) : -1; /* placeholder: read end is not what we got */
    (void)n;
    int st = 0;
    dp_wait_timeout(pid, &st, 10000);
    CHECK(m[1] == 0x22, "F5.s4", "forked child received the shm fd over SCM_RIGHTS, mapped it MAP_SHARED and the parent sees its write (0x%x)", m[1]);
    CHECK(rr >= 0 && nr == 1, "F5.s4", "the child sent a pipe write end back to the parent (r=%d, fds=%d)", rr, nr);
    if (nr == 1) {
        /* we received the write end; data written by the child sits in the pipe, but we only hold the write end: just prove the fd is a pipe */
        struct stat s;
        fstat(rcv[0], &s);
        CHECK((s.st_mode & S_IFMT) == S_IFIFO, "F5.s4", "the fd received from the child is a pipe end (mode 0%o)", s.st_mode & S_IFMT);
        close(rcv[0]);
    }
    (void)buf;
    munmap(m, FG_PAGE);
    close(shm);
    close(sp[0]);
}

/* ---- S5: credentials --------------------------------------------------------------------------- */
static void peer_report(const char *what, int fd, pid_t expect_pid)
{
    struct xucred xc;
    socklen_t len = sizeof xc;
    memset(&xc, 0, sizeof xc);
    int r = getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &xc, &len);
    int e = errno;
    if (r == 0) INFO("F5.s5", "%s: LOCAL_PEERCRED ok: version=%u uid=%u ngroups=%d group0=%u (len=%u)", what, xc.cr_version, xc.cr_uid, xc.cr_ngroups, xc.cr_ngroups ? xc.cr_groups[0] : 0, len);
    else INFO("F5.s5", "%s: LOCAL_PEERCRED failed %s", what, ERRS(e));
    pid_t pp = -1, ep = -1;
    len = sizeof pp;
    int r2 = getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pp, &len);
    int e2 = errno;
    len = sizeof ep;
    int r3 = getsockopt(fd, SOL_LOCAL, LOCAL_PEEREPID, &ep, &len);
    int e3 = errno;
    INFO("F5.s5", "%s: LOCAL_PEERPID %s pid=%d (expected %d) ; LOCAL_PEEREPID %s pid=%d", what, r2 == 0 ? "ok" : ERRS(e2), pp, expect_pid, r3 == 0 ? "ok" : ERRS(e3), ep);
    if (expect_pid > 0) CHECK(r2 == 0 && pp == expect_pid, "F5.s5", "%s: LOCAL_PEERPID returns the peer's pid, the SO_PEERCRED pid equivalent", what);
    unsigned char uu[16];
    len = sizeof uu;
    int r4 = getsockopt(fd, SOL_LOCAL, LOCAL_PEERUUID, uu, &len);
    int e4 = errno;
    audit_token_t tok;
    len = sizeof tok;
    int r5 = getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &tok, &len);
    int e5 = errno;
    INFO("F5.s5", "%s: LOCAL_PEERUUID %s ; LOCAL_PEERTOKEN %s (len=%u, pid field=%d)", what, r4 == 0 ? "ok" : ERRS(e4), r5 == 0 ? "ok" : ERRS(e5), len, r5 == 0 ? (int)tok.val[5] : -1);
    uid_t u;
    gid_t g;
    int r6 = getpeereid(fd, &u, &g);
    INFO("F5.s5", "%s: getpeereid %s uid=%u gid=%u", what, r6 == 0 ? "ok" : ERRS(errno), u, g);
}

static int s5_child(void *arg)
{
    int s = *(int *)arg;
    peer_report("child end of socketpair", s, getppid());
    return 0;
}

static void s5_creds(void)
{
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    peer_report("socketpair in one process (peer is ourselves)", sp[0], getpid());
    int st;
    fg_run_in_child(s5_child, &sp[1], 5000, &st);
    close(sp[0]); close(sp[1]);

    /* bound and connected path socket, peer is a forked child */
    char path[200];
    snprintf(path, sizeof path, "%s/f5c-%d.sock", dp_tmpdir(), (int)getpid());
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof sun);
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof sun.sun_path, "%s", path);
    bind(ls, (struct sockaddr *)&sun, sizeof sun);
    listen(ls, 1);
    pid_t pid = fork();
    if (pid == 0) {
        dp_role = "child";
        dp_watchdog(20);
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&sun, sizeof sun)) _exit(2);
        usleep(300000);
        _exit(0);
    }
    dp_track(pid);
    int a = accept(ls, NULL, NULL);
    peer_report("accepted path socket (peer is a forked child)", a, pid);
    struct sockaddr_un sa;
    socklen_t sl = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int gp = getpeername(a, (struct sockaddr *)&sa, &sl);
    INFO("F5.s5", "getpeername on the accepted socket: %d, len=%u, family=%d, path='%s' (client was unbound, so an empty name is expected)", gp, sl, sa.sun_family, sa.sun_path);
    close(a);
    dp_wait_timeout(pid, &st, 5000);
    close(ls);
    unlink(path);

    /* SCM_CREDS */
    {
        int dg[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, dg);
        struct msghdr msg;
        memset(&msg, 0, sizeof msg);
        struct iovec iov = { "x", 1 };
        char ctl[CMSG_SPACE(sizeof(struct fg_cmsgcred))];
        memset(ctl, 0, sizeof ctl);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctl;
        msg.msg_controllen = sizeof ctl;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_CREDS;
        c->cmsg_len = CMSG_LEN(sizeof(struct fg_cmsgcred));
        int rs = (int)sendmsg(dg[0], &msg, 0);
        int es = errno;
        INFO("F5.s5", "sendmsg with an SCM_CREDS control message on AF_UNIX: %s", rs < 0 ? ERRS(es) : "accepted");
        if (rs >= 0) {
            char rb[1], rc[256];
            struct iovec riov = { rb, 1 };
            struct msghdr rm;
            memset(&rm, 0, sizeof rm);
            rm.msg_iov = &riov;
            rm.msg_iovlen = 1;
            rm.msg_control = rc;
            rm.msg_controllen = sizeof rc;
            ssize_t rr = recvmsg(dg[1], &rm, 0);
            int types = 0;
            for (struct cmsghdr *cc = CMSG_FIRSTHDR(&rm); cc; cc = CMSG_NXTHDR(&rm, cc)) types |= 1 << cc->cmsg_type;
            INFO("F5.s5", "  receive: %zd bytes, control messages received (bit per cmsg_type): 0x%x, controllen=%u", rr, types, rm.msg_controllen);
        }
        /* SCM_TIMESTAMP via SO_TIMESTAMP */
        int on = 1;
        int rt = setsockopt(dg[1], SOL_SOCKET, SO_TIMESTAMP, &on, sizeof on);
        INFO("F5.s5", "setsockopt(SO_TIMESTAMP) on an AF_UNIX socket: %s", rt ? ERRS(errno) : "accepted");
        close(dg[0]); close(dg[1]);
    }
}

/* ---- S6: fileport -------------------------------------------------------------------------------- */
static int s6_child(void *arg)
{
    fileport_t *p = arg;
    int fd = fileport_makefd(*p);
    dp_emit("INFO", "F5.s6", "in a forked child fileport_makefd(port inherited by name) returned %d errno %s", fd, fd < 0 ? ERRS(errno) : "-");
    return 0;
}

static void s6_fileport(void)
{
    int pp[2];
    pipe(pp);
    fileport_t port = MACH_PORT_NULL;
    int r = fileport_makeport(pp[1], &port);
    INFO("F5.s6", "fileport_makeport(pipe write end): %d port=0x%x", r, port);
    if (r != 0) return;
    int fd2 = fileport_makefd(port);
    ssize_t w = write(fd2, "ab", 2);
    (void)w;
    char b[4] = { 0 };
    ssize_t n = read(pp[0], b, 2);
    CHECK(fd2 >= 0 && n == 2 && !memcmp(b, "ab", 2), "F5.s6", "fileport_makefd returns a new fd on the same pipe (fd %d, read back '%s')", fd2, b);
    int st;
    fg_run_in_child(s6_child, &port, 5000, &st);
    char *tmp = fg_tmpname("f5fp");
    int f = mkstemp(tmp);
    unlink(tmp);
    free(tmp);
    fileport_t p2;
    fileport_makeport(f, &p2);
    int f2 = fileport_makefd(p2);
    lseek(f, 5, SEEK_SET);
    off_t off2 = lseek(f2, 0, SEEK_CUR);
    CHECK(off2 == 5, "F5.s6", "fileport copies share the open file description (offset set to 5 through one fd, other fd reports %lld)", (long long)off2);
    /* a shm fd through a fileport, mapped */
    char shname[32];
    snprintf(shname, sizeof shname, "/fg5d-%d", (int)getpid());
    int shm = shm_open(shname, O_CREAT | O_EXCL | O_RDWR, 0600);
    shm_unlink(shname);
    ftruncate(shm, FG_PAGE);
    fileport_t p3;
    int rp = fileport_makeport(shm, &p3);
    int shm2 = rp == 0 ? fileport_makefd(p3) : -1;
    unsigned char *m1 = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    unsigned char *m2 = shm2 >= 0 ? mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, shm2, 0) : MAP_FAILED;
    if (m1 != MAP_FAILED && m2 != MAP_FAILED) m1[0] = 0x77;
    CHECK(m2 != MAP_FAILED && m2[0] == 0x77, "F5.s6", "shm fd -> fileport -> fd: both mappings alias the same pages");
    mach_port_deallocate(mach_task_self(), port);
}

int main(int argc, char **argv)
{
    dp_watchdog(120);
    signal(SIGPIPE, SIG_IGN);
    fg_load_info("F5.env");
    s1_types();
    s2_flags_and_counts();
    s4_cross_process();
    s5_creds();
    s6_fileport();
    dp_summary("f5_scm_rights");
    return 0;
}
