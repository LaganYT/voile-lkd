/* w30_scmdev: SCM_RIGHTS of descriptors that LKD emulates, between guest processes that live in different host
 * processes (net/core/scm.c: the receiver gets the open file and uses it as the sender would).
 * 1. A child (a fork: another host process) opens and sends /dev/null (O_RDWR|O_NONBLOCK), /dev/zero, /dev/urandom,
 *    /dev/full (O_WRONLY), the synthetic directory SYN (a parent of a bind mount, absent from the root file system),
 *    /proc/self (its /proc/<pid>), /proc/self/status after reading "Name:\t", an O_PATH descriptor of the witness and
 *    a signalfd for SIGUSR1. The witness uses each one: fstat (1:3, 1:5, 1:9, 1:7, directories), read_null and
 *    write_null, read_zero, urandom bytes, write_full (ENOSPC), poll, mmap of /dev/zero, getdents64, openat and fchdir
 *    relative to the directories, the /proc position, /proc/self/fd links, O_PATH rules, and a SIGUSR1 of its own
 *    read from the signalfd (signalfd_dequeue reads current). It sends /dev/null, /dev/zero and SYN back and the child
 *    checks them again, as wineserver hands a client's descriptor to another client.
 * 2. Wine's case: a child with /dev/null as stdin, stdout and stderr execs the witness again, and the new image sends
 *    its descriptors 0, 1 and 2 (ntdll/unix/env.c get_initial_console -> wine_server_fd_to_handle).
 * Usage: w30_scmdev SYN ENTRY (ENTRY: a name inside SYN, the bind mount point). Prints W30 PASS or W30 FAIL. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("W30 FAIL " __VA_ARGS__); printf(" (errno %d)\n", errno); fflush(stdout); fails++; } } while (0)

static int send_fds(int s, const int *fds, int n)
{
    char c = 'f';
    struct iovec iov = { &c, 1 };
    char ctl[CMSG_SPACE(16 * sizeof(int))];
    memset(ctl, 0, sizeof ctl);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = CMSG_SPACE(n * sizeof(int)) };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(h), fds, n * sizeof(int));
    return sendmsg(s, &m, 0) == 1 ? 0 : -1;
}

static int recv_fds(int s, int *fds, int cap)
{
    char c;
    struct iovec iov = { &c, 1 };
    char ctl[CMSG_SPACE(16 * sizeof(int))];
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = CMSG_SPACE(cap * sizeof(int)) };
    if (recvmsg(s, &m, MSG_CMSG_CLOEXEC) != 1 || (m.msg_flags & MSG_CTRUNC)) return -1;
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    if (!h || h->cmsg_level != SOL_SOCKET || h->cmsg_type != SCM_RIGHTS) return 0;
    int n = (int)((h->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    memcpy(fds, CMSG_DATA(h), n * sizeof(int));
    return n;
}

static int is_chr(int fd, unsigned minor)
{
    struct stat st;
    return fstat(fd, &st) == 0 && st.st_mode == (S_IFCHR | 0666) && st.st_rdev == makedev(1, minor);
}

static int link_is(int fd, const char *want)
{
    char p[64], got[512];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, got, sizeof got - 1);
    if (n < 0) return 0;
    got[n] = 0;
    return !strcmp(got, want);
}

static int lists(int dfd, const char *name)
{
    char buf[8192];
    long n = syscall(SYS_getdents64, dfd, buf, sizeof buf);
    for (long off = 0; n > 0 && off < n;) {
        struct dirent64 *e = (struct dirent64 *)(buf + off);
        if (!strcmp(e->d_name, name)) return 1;
        off += e->d_reclen;
    }
    return 0;
}

static int all(const unsigned char *b, size_t n, unsigned char v)
{
    for (size_t i = 0; i < n; i++) if (b[i] != v) return 0;
    return 1;
}

/* Part 2, after the exec: send descriptors 0, 1 and 2 over the inherited socket, then exit. */
static int send_stdio(int sock)
{
    int fds[3] = { 0, 1, 2 };
    return send_fds(sock, fds, 3) == 0 ? 0 : 3;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--send-stdio")) return send_stdio(atoi(argv[2]));
    const char *syn = argc > 1 ? argv[1] : "/lkdsyn", *entry = argc > 2 ? argv[2] : "w";
    const char *self = argv[0], *base = strrchr(self, '/') ? strrchr(self, '/') + 1 : self;
    setvbuf(stdout, NULL, _IONBF, 0);
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { printf("W30 FAIL socketpair\n"); return 1; }

    pid_t c = fork();
    if (c == 0) {
        close(sv[0]);
        int fds[9];
        char head[8] = { 0 };
        fds[0] = open("/dev/null", O_RDWR | O_NONBLOCK);
        fds[1] = open("/dev/zero", O_RDONLY);
        fds[2] = open("/dev/urandom", O_RDONLY);
        fds[3] = open("/dev/full", O_WRONLY);
        fds[4] = open(syn, O_RDONLY | O_DIRECTORY);
        fds[5] = open("/proc/self", O_RDONLY | O_DIRECTORY);
        fds[6] = open("/proc/self/status", O_RDONLY);
        if (fds[6] >= 0 && read(fds[6], head, 6) != 6) _exit(10);
        fds[7] = open(self, O_PATH);
        fds[8] = signalfd(-1, &usr1, SFD_NONBLOCK);
        for (int i = 0; i < 9; i++) if (fds[i] < 0) _exit(11 + i);
        if (send_fds(sv[1], fds, 9) < 0) _exit(20);
        int back[3];
        if (recv_fds(sv[1], back, 3) != 3) _exit(21);
        if (!is_chr(back[0], 3) || write(back[0], "abc", 3) != 3) _exit(22);
        unsigned char z[32];
        memset(z, 0xff, sizeof z);
        if (!is_chr(back[1], 5) || read(back[1], z, sizeof z) != (ssize_t)sizeof z || !all(z, sizeof z, 0)) _exit(23);
        struct stat st;
        if (fstat(back[2], &st) != 0 || !S_ISDIR(st.st_mode) || !lists(back[2], entry) || !link_is(back[2], syn)) _exit(24);
        _exit(0);
    }
    close(sv[1]);
    int r[9];
    int n = recv_fds(sv[0], r, 9);
    CHECK(n == 9, "received %d descriptors, want 9", n);
    if (n != 9) { kill(c, SIGKILL); waitpid(c, NULL, 0); printf("W30 FAIL %d\n", fails); return 1; }

    unsigned char b[64];
    /* /dev/null: read_null, write_null, always ready, the sender's O_NONBLOCK */
    CHECK(is_chr(r[0], 3), "/dev/null mode or numbers");
    CHECK(read(r[0], b, sizeof b) == 0, "/dev/null read");
    CHECK(write(r[0], "abc", 3) == 3, "/dev/null write");
    CHECK((fcntl(r[0], F_GETFL) & (O_ACCMODE | O_NONBLOCK)) == (O_RDWR | O_NONBLOCK), "/dev/null flags %#x", fcntl(r[0], F_GETFL));
    struct pollfd pf = { r[0], POLLIN | POLLOUT, 0 };
    CHECK(poll(&pf, 1, 0) == 1 && pf.revents == (POLLIN | POLLOUT), "/dev/null poll %#x", pf.revents);
    CHECK(link_is(r[0], "/dev/null"), "/dev/null link");
    /* /dev/zero: read_zero, mmap_zero */
    CHECK(is_chr(r[1], 5), "/dev/zero mode or numbers");
    memset(b, 0xff, sizeof b);
    CHECK(read(r[1], b, sizeof b) == (ssize_t)sizeof b && all(b, sizeof b, 0), "/dev/zero read");
    unsigned char *mz = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, r[1], 0);
    CHECK(mz != MAP_FAILED && all(mz, 8192, 0), "/dev/zero mmap");
    if (mz != MAP_FAILED) { mz[100] = 7; CHECK(mz[100] == 7, "/dev/zero mapping writable"); munmap(mz, 8192); }
    /* /dev/urandom */
    memset(b, 0, sizeof b);
    CHECK(is_chr(r[2], 9), "/dev/urandom mode or numbers");
    CHECK(read(r[2], b, sizeof b) == (ssize_t)sizeof b && !all(b, sizeof b, 0), "/dev/urandom read");
    /* /dev/full: write_full, opened write-only */
    CHECK(is_chr(r[3], 7), "/dev/full mode or numbers");
    errno = 0;
    CHECK(write(r[3], "x", 1) == -1 && errno == ENOSPC, "/dev/full write");
    errno = 0;
    CHECK(read(r[3], b, 1) == -1 && errno == EBADF, "/dev/full read on O_WRONLY");
    /* the synthetic directory */
    struct stat st;
    CHECK(fstat(r[4], &st) == 0 && S_ISDIR(st.st_mode), "synthetic dir fstat");
    CHECK(lists(r[4], entry), "synthetic dir getdents64 lacks %s", entry);
    char rel[256];
    snprintf(rel, sizeof rel, "%s/%s", entry, base);
    int f = openat(r[4], rel, O_RDONLY);
    CHECK(f >= 0 && read(f, b, 4) == 4 && !memcmp(b, "\177ELF", 4), "openat(synthetic dir, %s)", rel);
    if (f >= 0) close(f);
    CHECK(link_is(r[4], syn), "synthetic dir link");
    char cwd[256] = { 0 };
    CHECK(fchdir(r[4]) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, syn), "fchdir synthetic dir: %s", cwd);
    chdir("/");
    /* /proc/<child> */
    char want[64];
    snprintf(want, sizeof want, "/proc/%d", (int)c);
    CHECK(link_is(r[5], want), "/proc/self of the child is %s", want);
    f = openat(r[5], "comm", O_RDONLY);
    memset(b, 0, sizeof b);
    CHECK(f >= 0 && read(f, b, sizeof b - 1) > 0 && !strncmp((char *)b, base, strlen(base) > 15 ? 15 : strlen(base)), "openat(/proc/<child>, comm): %s", b);
    if (f >= 0) close(f);
    /* /proc/<child>/status at position 6 */
    char path[64], full[512] = { 0 }, got[9] = { 0 };
    snprintf(path, sizeof path, "/proc/%d/status", (int)c);
    f = open(path, O_RDONLY);
    CHECK(f >= 0 && read(f, full, sizeof full - 1) > 14, "own read of %s", path);
    if (f >= 0) close(f);
    CHECK(read(r[6], got, 8) == 8 && !memcmp(got, full + 6, 8), "status continues at 6: '%s'", got);
    CHECK(lseek(r[6], 0, SEEK_CUR) == 14, "status position");
    CHECK(link_is(r[6], path), "status link");
    /* O_PATH */
    CHECK(fstat(r[7], &st) == 0 && S_ISREG(st.st_mode), "O_PATH fstat");
    CHECK(fcntl(r[7], F_GETFL) & O_PATH, "O_PATH flag");
    errno = 0;
    CHECK(read(r[7], b, 1) == -1 && errno == EBADF, "O_PATH read");
    CHECK(link_is(r[7], self), "O_PATH link");
    /* signalfd: reads the receiver's own signals */
    struct signalfd_siginfo si;
    errno = 0;
    CHECK(read(r[8], &si, sizeof si) == -1 && errno == EAGAIN, "signalfd empty");
    kill(getpid(), SIGUSR1);
    memset(&si, 0, sizeof si);
    CHECK(read(r[8], &si, sizeof si) == (ssize_t)sizeof si && si.ssi_signo == SIGUSR1 && si.ssi_pid == (uint32_t)getpid(),
          "signalfd read signo %u pid %u", si.ssi_signo, si.ssi_pid);

    int back[3] = { r[0], r[1], r[4] };
    CHECK(send_fds(sv[0], back, 3) == 0, "send back");
    int ws = 0;
    CHECK(waitpid(c, &ws, 0) == c && WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "child status %#x", ws);
    for (int i = 0; i < 9; i++) close(r[i]);
    close(sv[0]);

    /* 2. /dev/null as the standard descriptors of a process that execs, then sends them */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { printf("W30 FAIL socketpair 2\n"); return 1; }
    pid_t w = fork();
    if (w == 0) {
        close(sv[0]);
        int nul = open("/dev/null", O_RDWR);
        if (nul < 0 || dup2(nul, 0) < 0 || dup2(nul, 1) < 0 || dup2(nul, 2) < 0) _exit(30);
        close(nul);
        char sock[16];
        snprintf(sock, sizeof sock, "%d", sv[1]);
        execl(self, self, "--send-stdio", sock, (char *)NULL);
        _exit(31);
    }
    close(sv[1]);
    int s3[3];
    n = recv_fds(sv[0], s3, 3);
    CHECK(n == 3, "stdio: received %d descriptors, want 3", n);
    for (int i = 0; i < n && i < 3; i++) {
        CHECK(is_chr(s3[i], 3), "stdio fd %d is not /dev/null", i);
        CHECK(write(s3[i], "x", 1) == 1, "stdio fd %d write", i);
        close(s3[i]);
    }
    CHECK(waitpid(w, &ws, 0) == w && WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "stdio child status %#x", ws);
    close(sv[0]);

    if (fails) printf("W30 FAIL %d\n", fails);
    else printf("W30 PASS\n");
    return fails ? 1 : 0;
}
