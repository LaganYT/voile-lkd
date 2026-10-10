/* test_net_scm.c: SCM_RIGHTS of objects that have no host descriptor of their own, between two host processes
 * (a real fork, as two guest processes of the engine are two host processes). Linux passes any open file over a
 * unix socket (net/core/scm.c scm_fp_copy, unix(7)) and the receiver uses it as the sender would: the memory
 * devices (drivers/char/mem.c), synthetic and virtual directories, O_PATH descriptors (fs/open.c), generated /proc
 * files (fs/seq_file.c), /proc/<pid>/mem, a signalfd, which reads the signals of its reader (fs/signalfd.c
 * signalfd_dequeue uses current). Children report the failing step by their exit code. */
#include <poll.h>

#include "../src/include/net.h"
#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX], BIND[PATH_MAX];
static lkd_task *T;
static int32_t g_sv[2];

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

/* Returns the number of descriptors received, or -1. Never waits: every message is sent before the receiver
 * runs, and a failed child sends nothing. */
static int recv_now(int sock, int32_t *fds, int cap)
{
    struct { struct lx_cmsghdr h; int32_t fd[8]; } ctl;
    memset(&ctl, 0, sizeof ctl);
    char buf[16];
    struct lx_iovec iov = { P(buf), sizeof buf };
    struct lx_msghdr m = { .msg_iov = P(&iov), .msg_iovlen = 1, .msg_control = P(&ctl), .msg_controllen = LX_CMSG_LEN(cap * 4) };
    int64_t r = SC(T, recvmsg, sock, P(&m), LX_MSG_DONTWAIT);
    if (r < 0 || (m.msg_flags & LX_MSG_CTRUNC)) return -1;
    if (m.msg_controllen < sizeof(struct lx_cmsghdr) || ctl.h.cmsg_level != LX_SOL_SOCKET || ctl.h.cmsg_type != LX_SCM_RIGHTS) return 0;
    int n = (int)((ctl.h.cmsg_len - sizeof(struct lx_cmsghdr)) / 4);
    memcpy(fds, ctl.fd, (size_t)n * 4);
    return n;
}

/* A character device with the Linux mode and numbers (Documentation/admin-guide/devices.txt: mem is major 1). */
static int is_chr(int fd, unsigned minor)
{
    struct lx_stat st;
    if (SC(T, fstat, fd, P(&st)) != 0) return 0;
    return st.st_mode == (LX_S_IFCHR | 0666) && st.st_rdev == ((1u << 8) | minor);
}

static int link_is(int fd, const char *want)
{
    char link[64], got[256] = { 0 };
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    int64_t n = SC(T, readlinkat, AT_CWD, P(link), P(got), sizeof got - 1);
    return n == (int64_t)strlen(want) && !strcmp(got, want);
}

static int dir_lists(int fd, const char *name)
{
    char ents[4096];
    int64_t n = SC(T, getdents64, fd, P(ents), sizeof ents);
    for (int64_t off = 0; n > 0 && off < n;) {
        struct lx_linux_dirent64 *e = (struct lx_linux_dirent64 *)(ents + off);
        if (!strcmp(e->d_name, name)) return 1;
        off += e->d_reclen;
    }
    return 0;
}

static int all_bytes(const uint8_t *b, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++) if (b[i] != v) return 0;
    return 1;
}

/* ---- memory devices ------------------------------------------------------------------------------- */

/* Received: /dev/null (O_RDWR|O_NONBLOCK), /dev/zero, /dev/urandom, /dev/random (O_RDONLY), /dev/full
 * (O_WRONLY). Each is the Linux device (read_null, write_null, read_zero, write_full, urandom_read_iter), then
 * all five go back to the parent, as wineserver hands a descriptor back to another client. */
static void child_devices(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t d[8];
    if (recv_now(g_sv[1], d, 8) != 5) _exit(1);
    uint8_t b[64];
    if (!is_chr(d[0], 3)) _exit(2);
    if (SC(T, read, d[0], P(b), sizeof b) != 0) _exit(3);
    if (SC(T, write, d[0], P("abc"), 3) != 3) _exit(4);
    if ((SC(T, fcntl, d[0], LX_F_GETFL) & (LX_O_ACCMODE | LX_O_NONBLOCK)) != (LX_O_RDWR | LX_O_NONBLOCK)) _exit(5);
    if (!link_is(d[0], "/dev/null")) _exit(6);
    if (!is_chr(d[1], 5)) _exit(7);
    memset(b, 0xff, sizeof b);
    if (SC(T, read, d[1], P(b), sizeof b) != (int64_t)sizeof b || !all_bytes(b, sizeof b, 0)) _exit(8);
    if ((SC(T, fcntl, d[1], LX_F_GETFL) & LX_O_ACCMODE) != LX_O_RDONLY) _exit(9);
    if (!is_chr(d[2], 9)) _exit(10);
    memset(b, 0, sizeof b);
    if (SC(T, read, d[2], P(b), sizeof b) != (int64_t)sizeof b || all_bytes(b, sizeof b, 0)) _exit(11);
    if (!is_chr(d[3], 8)) _exit(12);
    if (!is_chr(d[4], 7)) _exit(13);
    if (SC(T, write, d[4], P("x"), 1) != -LX_ENOSPC) _exit(14);
    if (SC(T, read, d[4], P(b), 1) != -LX_EBADF) _exit(15);       /* opened O_WRONLY */
    struct lx_pollfd pf = { d[0], LX_POLLIN | LX_POLLOUT, 0 };
    if (SC(T, poll, P(&pf), 1, 0) != 1 || pf.revents != (LX_POLLIN | LX_POLLOUT)) _exit(16);
    if (send_fds(g_sv[1], d, 5, "back") != 4) _exit(17);
    _exit(0);
}

LT_TEST(memory_devices_cross_host_processes)
{
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(g_sv)), 0);
    int32_t d[5];
    d[0] = (int32_t)SC(T, openat, AT_CWD, P("/dev/null"), LX_O_RDWR | LX_O_NONBLOCK, 0);
    d[1] = (int32_t)SC(T, openat, AT_CWD, P("/dev/zero"), LX_O_RDONLY, 0);
    d[2] = (int32_t)SC(T, openat, AT_CWD, P("/dev/urandom"), LX_O_RDONLY, 0);
    d[3] = (int32_t)SC(T, openat, AT_CWD, P("/dev/random"), LX_O_RDONLY, 0);
    d[4] = (int32_t)SC(T, openat, AT_CWD, P("/dev/full"), LX_O_WRONLY, 0);
    for (int i = 0; i < 5; i++) LT_CHECK(d[i] >= 0);
    LT_EQ(send_fds(g_sv[0], d, 5, "dev"), 3);   /* before the change: -EOPNOTSUPP */
    LT_EQ(lt_run_child(child_devices, NULL, 20), 0);
    int32_t back[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    LT_EQ(recv_now(g_sv[0], back, 8), 5);
    LT_CHECK(is_chr(back[0], 3));
    LT_CHECK(is_chr(back[1], 5));
    LT_CHECK(is_chr(back[2], 9));
    LT_CHECK(is_chr(back[3], 8));
    LT_CHECK(is_chr(back[4], 7));
    LT_EQ(SC(T, write, back[0], P("abc"), 3), 3);
    LT_EQ(SC(T, write, back[4], P("x"), 1), -LX_ENOSPC);
    uint8_t b[16];
    memset(b, 0xff, sizeof b);
    LT_EQ(SC(T, read, back[1], P(b), sizeof b), (int64_t)sizeof b);
    LT_CHECK(all_bytes(b, sizeof b, 0));
    LT_CHECK(link_is(back[2], "/dev/urandom"));
    for (int i = 0; i < 5; i++) { SC(T, close, d[i]); SC(T, close, back[i]); }
    SC(T, close, g_sv[0]); SC(T, close, g_sv[1]);
}

/* The runtime's standard descriptors are host descriptors; a host /dev/null among them is the Linux /dev/null
 * (1:3, always ready), not a host stream that macOS cannot poll (MESURE: POLLNVAL). */
static void child_stdio(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int h = open("/dev/null", O_RDWR);
    if (h < 0 || dup2(h, 0) < 0) _exit(1);
    if (lkd_fs_install_stdio(T) < 0) _exit(2);
    if (!is_chr(0, 3)) _exit(3);
    struct lx_pollfd pf = { 0, LX_POLLIN, 0 };
    if (SC(T, poll, P(&pf), 1, 0) != 1 || pf.revents != LX_POLLIN) _exit(4);
    char c;
    if (SC(T, read, 0, P(&c), 1) != 0) _exit(5);
    if ((SC(T, fcntl, 0, LX_F_GETFL) & LX_O_ACCMODE) != LX_O_RDWR) _exit(6);
    _exit(0);
}

LT_TEST(host_dev_null_stdio_is_the_linux_device)
{
    LT_EQ(lt_run_child(child_stdio, NULL, 10), 0);
}

/* ---- directories and O_PATH ----------------------------------------------------------------------- */

/* Received: the synthetic /syn (parent of the bind /syn/data), the virtual /dev, /proc/<pid> (opened as
 * /proc/self), an O_PATH descriptor of /plain and one of /syn. */
static int32_t g_pid;
static void child_dirs(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t d[8];
    if (recv_now(g_sv[1], d, 8) != 5) _exit(1);
    struct lx_stat st;
    if (SC(T, fstat, d[0], P(&st)) != 0 || st.st_mode != (LX_S_IFDIR | 0555)) _exit(2);
    if (!dir_lists(d[0], "data")) _exit(3);
    int f = (int)SC(T, openat, d[0], P("data/inner.txt"), LX_O_RDONLY, 0);
    char c[8] = { 0 };
    if (f < 0 || SC(T, read, f, P(c), sizeof c) != 5 || memcmp(c, "hello", 5)) _exit(4);
    SC(T, close, f);
    if (!link_is(d[0], "/syn")) _exit(5);
    if (SC(T, openat, d[0], P("data"), LX_O_RDONLY | LX_O_DIRECTORY, 0) < 0) _exit(6);
    int n = (int)SC(T, openat, d[1], P("null"), LX_O_WRONLY, 0);
    if (n < 0 || !is_chr(n, 3)) _exit(7);
    if (!dir_lists(d[1], "zero")) _exit(8);
    f = (int)SC(T, openat, d[2], P("status"), LX_O_RDONLY, 0);
    char s[256] = { 0 }, want[32];
    if (f < 0 || SC(T, read, f, P(s), sizeof s - 1) <= 0 || !strstr(s, "Pid:")) _exit(9);
    snprintf(want, sizeof want, "/proc/%d", g_pid);
    if (!link_is(d[2], want)) _exit(10);
    if (SC(T, fstat, d[3], P(&st)) != 0 || (st.st_mode & LX_S_IFMT) != LX_S_IFREG || st.st_size != 1) _exit(11);
    if (!(SC(T, fcntl, d[3], LX_F_GETFL) & LX_O_PATH)) _exit(12);
    if (SC(T, read, d[3], P(c), 1) != -LX_EBADF) _exit(13);
    if (SC(T, fstat, d[4], P(&st)) != 0 || (st.st_mode & LX_S_IFMT) != LX_S_IFDIR) _exit(14);
    if (SC(T, openat, d[4], P("data/inner.txt"), LX_O_RDONLY, 0) < 0) _exit(15);
    if (SC(T, fchdir, d[0], 0) != 0) _exit(16);
    char cwd[64] = { 0 };
    if (SC(T, getcwd, P(cwd), sizeof cwd) <= 0 || strcmp(cwd, "/syn")) _exit(17);
    _exit(0);
}

LT_TEST(directories_and_o_path_cross_host_processes)
{
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(g_sv)), 0);
    int32_t d[5];
    d[0] = (int32_t)SC(T, openat, AT_CWD, P("/syn"), LX_O_RDONLY | LX_O_DIRECTORY, 0);
    d[1] = (int32_t)SC(T, openat, AT_CWD, P("/dev"), LX_O_RDONLY | LX_O_DIRECTORY, 0);
    d[2] = (int32_t)SC(T, openat, AT_CWD, P("/proc/self"), LX_O_RDONLY | LX_O_DIRECTORY, 0);
    d[3] = (int32_t)SC(T, openat, AT_CWD, P("/plain"), LX_O_PATH, 0);
    d[4] = (int32_t)SC(T, openat, AT_CWD, P("/syn"), LX_O_PATH, 0);
    for (int i = 0; i < 5; i++) LT_CHECK(d[i] >= 0);
    g_pid = T->proc->pid;
    LT_EQ(send_fds(g_sv[0], d, 5, "dirs"), 4);   /* before the change: -EOPNOTSUPP */
    LT_EQ(lt_run_child(child_dirs, NULL, 20), 0);
    for (int i = 0; i < 5; i++) SC(T, close, d[i]);
    SC(T, close, g_sv[0]); SC(T, close, g_sv[1]);
}

/* ---- generated /proc files -------------------------------------------------------------------------- */

/* Received: /proc/<pid>/status (opened as /proc/self/status) after the parent read "Name:\t" from it. The receiver
 * continues at the same position (seq_file) and sees the same file. */
static void child_procfs(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t d[8];
    if (recv_now(g_sv[1], d, 8) != 1) _exit(1);
    char want[16], path[64], got[16] = { 0 }, full[256] = { 0 };
    snprintf(path, sizeof path, "/proc/%d/status", g_pid);
    int f = (int)SC(T, openat, AT_CWD, P(path), LX_O_RDONLY, 0);
    if (f < 0 || SC(T, read, f, P(full), sizeof full - 1) < 14) _exit(2);
    memcpy(want, full + 6, 8);
    if (SC(T, read, d[0], P(got), 8) != 8 || memcmp(got, want, 8)) _exit(3);
    struct lx_stat st;
    if (SC(T, fstat, d[0], P(&st)) != 0 || st.st_mode != (LX_S_IFREG | 0444)) _exit(4);
    if (!link_is(d[0], path)) _exit(5);
    if (SC(T, lseek, d[0], 0, LX_SEEK_CUR) != 14) _exit(6);
    _exit(0);
}

LT_TEST(proc_files_keep_their_position)
{
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(g_sv)), 0);
    g_pid = T->proc->pid;
    int32_t d = (int32_t)SC(T, openat, AT_CWD, P("/proc/self/status"), LX_O_RDONLY, 0);
    char head[8] = { 0 };
    LT_EQ(SC(T, read, d, P(head), 6), 6);
    LT_STREQ(head, "Name:\t");
    LT_EQ(send_fds(g_sv[0], &d, 1, "proc"), 4);   /* before the change: -EOPNOTSUPP */
    LT_EQ(lt_run_child(child_procfs, NULL, 20), 0);
    SC(T, close, d); SC(T, close, g_sv[0]); SC(T, close, g_sv[1]);
}

/* ---- signalfd -------------------------------------------------------------------------------------- */

/* The received signalfd reads the signals of the receiving process, with the sender's mask and O_NONBLOCK. */
static void child_signalfd(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    int32_t d[8];
    if (recv_now(g_sv[1], d, 8) != 1) _exit(1);
    uint8_t si[128];
    if (SC(T, read, d[0], P(si), sizeof si) != -LX_EAGAIN) _exit(2);
    if (!(SC(T, fcntl, d[0], LX_F_GETFL) & LX_O_NONBLOCK)) _exit(3);
    lkd_sig_send_task(T, LX_SIGUSR1, NULL, 0);                 /* a signal of this process */
    if (SC(T, read, d[0], P(si), sizeof si) != 128) _exit(5);
    uint32_t signo;
    memcpy(&signo, si, 4);
    if (signo != LX_SIGUSR1) _exit(6);
    _exit(0);
}

LT_TEST(signalfd_reads_the_receivers_signals)
{
    LT_EQ(SC(T, socketpair, LX_AF_UNIX, LX_SOCK_STREAM, 0, P(g_sv)), 0);
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE) | LX_SIGBIT(LX_SIGUSR1));
    uint64_t mask = LX_SIGBIT(LX_SIGUSR1);
    int32_t s = (int32_t)SC(T, signalfd4, (uint64_t)-1, P(&mask), 8, LX_SFD_NONBLOCK);
    LT_CHECK(s >= 0);
    LT_EQ(send_fds(g_sv[0], &s, 1, "sfd"), 3);   /* before the change: -EOPNOTSUPP */
    LT_EQ(lt_run_child(child_signalfd, NULL, 20), 0);
    uint8_t si[128];
    LT_EQ(SC(T, read, s, P(si), sizeof si), -LX_EAGAIN);   /* the child's signal was the child's */
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    SC(T, close, s); SC(T, close, g_sv[0]); SC(T, close, g_sv[1]);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "netscm");
    make_tmpdir(BIND, sizeof BIND, "netscmb");
    put_file(ROOT, "plain", "x");
    put_file(BIND, "inner.txt", "hello");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    if (lkd_vfs_mount_bind("/syn/data", BIND, 0) < 0) { fprintf(stderr, "bind failed\n"); return 2; }
    T = fs_task();
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));
    LT_RUN(memory_devices_cross_host_processes);
    LT_RUN(host_dev_null_stdio_is_the_linux_device);
    LT_RUN(directories_and_o_path_cross_host_processes);
    LT_RUN(proc_files_keep_their_position);
    LT_RUN(signalfd_reads_the_receivers_signals);
    rm_rf(ROOT);
    rm_rf(BIND);
    return lt_finish();
}
