/* test_fs_fd.c: descriptor table, sharing of open file descriptions, limits, close-on-exec, locks.
 * Linux behaviors modeled: fs/file.c (alloc_fd lowest free, do_dup2, ksys_dup3, close_range,
 * RLIMIT_NOFILE), dup(2) shares offset and status flags, fork(2) shares descriptions, fcntl(2)
 * F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL/F_GETLK/F_SETLK/F_OFD_*, flock(2). */
#include <sys/file.h>
#include <sys/wait.h>

#include "test_fs_util.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

static int64_t gopen(lkd_task *t, const char *p, uint32_t fl) { return SC(t, openat, AT_CWD, P(p), fl, 0644); }

static void set_nofile(lkd_task *t, uint64_t cur) { t->proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur = cur; }

LT_TEST(lowest_free_fd)
{
    lkd_task *t = fs_task();
    LT_EQ(gopen(t, "/f1", LX_O_RDONLY), 0);
    LT_EQ(gopen(t, "/f1", LX_O_RDONLY), 1);
    LT_EQ(gopen(t, "/f1", LX_O_RDONLY), 2);
    LT_EQ(SC(t, close, 1), 0);
    LT_EQ(gopen(t, "/f1", LX_O_RDONLY), 1);           /* lowest free */
    LT_ERRNO(SC(t, close, 1000), LX_EBADF);
    LT_ERRNO(SC(t, close, (uint64_t)-1), LX_EBADF);
    LT_EQ(lkd_fd_count(t), 3);
    int fds[8];
    LT_EQ(lkd_fd_list(t, fds, 8), 3);
    LT_EQ(fds[2], 2);
    lkd_set_current(T);
}

/* dup(2): "the two file descriptors refer to the same open file description and thus share file
 * offset and file status flags"; FD_CLOEXEC is per descriptor and off on the copy. */
LT_TEST(dup_shares_description)
{
    int64_t a = gopen(T, "/f1", LX_O_RDWR | LX_O_CLOEXEC);
    int64_t b = SC(T, dup, a);
    LT_CHECK(b >= 0 && b != a);
    char buf[8];
    LT_EQ(SC(T, read, a, P(buf), 3), 3);
    LT_EQ(SC(T, lseek, b, 0, LX_SEEK_CUR), 3);
    LT_EQ(SC(T, fcntl, a, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, fcntl, b, LX_F_GETFD), 0);
    LT_EQ(SC(T, dup2, a, 40), 40);
    LT_EQ(SC(T, lseek, 40, 1, LX_SEEK_CUR), 4);
    LT_EQ(SC(T, lseek, a, 0, LX_SEEK_CUR), 4);
    LT_EQ(SC(T, fcntl, b, LX_F_SETFL, LX_O_APPEND), 0);  /* status flag shared */
    LT_CHECK(SC(T, fcntl, a, LX_F_GETFL) & LX_O_APPEND);
    LT_EQ(SC(T, fcntl, a, LX_F_SETFL, 0), 0);
    LT_EQ(SC(T, dup3, a, 41, LX_O_CLOEXEC), 41);
    LT_EQ(SC(T, fcntl, 41, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_ERRNO(SC(T, dup3, a, a, 0), LX_EINVAL);
    LT_ERRNO(SC(T, dup3, a, 42, LX_O_NONBLOCK), LX_EINVAL);
    LT_EQ(SC(T, dup2, a, a), a);
    LT_ERRNO(SC(T, dup2, 777, 42), LX_EBADF);
    LT_ERRNO(SC(T, dup2, 777, 777), LX_EBADF);
    /* dup2 onto an open fd closes it first */
    int64_t c = gopen(T, "/f2", LX_O_RDONLY);
    LT_EQ(SC(T, dup2, a, c), c);
    LT_EQ(SC(T, lseek, c, 0, LX_SEEK_CUR), 4);
    /* independent opens have independent offsets */
    int64_t d = gopen(T, "/f1", LX_O_RDONLY);
    LT_EQ(SC(T, lseek, d, 0, LX_SEEK_CUR), 0);
    int64_t fds[] = { a, b, 40, 41, c, d };
    for (int i = 0; i < 6; i++) SC(T, close, fds[i]);
}

/* RLIMIT_NOFILE: dup2 at or above the soft limit -EBADF, F_DUPFD with arg >= limit -EINVAL, a full
 * table -EMFILE; fds well above 1024 work once the limit allows them. */
LT_TEST(limits_and_high_fds)
{
    lkd_task *t = fs_task();
    set_nofile(t, 1024);
    int64_t a = gopen(t, "/f1", LX_O_RDONLY);
    LT_EQ(SC(t, dup2, a, 1023), 1023);
    LT_ERRNO(SC(t, dup2, a, 1024), LX_EBADF);
    LT_ERRNO(SC(t, fcntl, a, LX_F_DUPFD, 1024), LX_EINVAL);
    set_nofile(t, 65536);
    LT_EQ(SC(t, dup2, a, 65535), 65535);
    LT_EQ(SC(t, fcntl, a, LX_F_DUPFD, 60000), 60000);
    LT_EQ(SC(t, fcntl, a, LX_F_DUPFD_CLOEXEC, 60000), 60001);
    LT_EQ(SC(t, fcntl, 60001, LX_F_GETFD), LX_FD_CLOEXEC);
    char c;
    LT_EQ(SC(t, read, 65535, P(&c), 1), 1);
    LT_EQ(SC(t, lseek, a, 0, LX_SEEK_CUR), 1);
    LT_ERRNO(SC(t, dup2, a, 65536), LX_EBADF);
    LT_EQ(SC(t, close_range, 1024, 0xffffffffu, 0), 0);
    /* fill to a small limit: EMFILE */
    set_nofile(t, 200);
    int last = -1;
    for (;;) { int64_t r = SC(t, dup, a); if (r < 0) { LT_ERRNO(r, LX_EMFILE); break; } last = (int)r; }
    LT_EQ(last, 199);
    LT_EQ(lkd_fd_count(t), 201);   /* 0..199 + 1023 */
    lkd_set_current(T);
}

/* close-on-exec: exec closes exactly the FD_CLOEXEC descriptors; the flag is mirrored on the host fd. */
LT_TEST(cloexec_and_exec)
{
    lkd_task *t = fs_task();
    int64_t a = gopen(t, "/f1", LX_O_RDONLY | LX_O_CLOEXEC);
    int64_t b = gopen(t, "/f1", LX_O_RDONLY);
    int64_t c = gopen(t, "/f1", LX_O_RDONLY);
    LT_EQ(SC(t, fcntl, c, LX_F_SETFD, LX_FD_CLOEXEC), 0);
    LT_EQ(SC(t, ioctl, b, LX_FIOCLEX), 0);
    LT_EQ(SC(t, ioctl, b, LX_FIONCLEX), 0);
    lkd_fdobj *o = lkd_fd_get(t, (int)c, NULL);
    LT_CHECK(o && (fcntl(o->host_fd, F_GETFD) & FD_CLOEXEC));
    lkd_fdobj_put(o);
    o = lkd_fd_get(t, (int)b, NULL);
    LT_CHECK(o && !(fcntl(o->host_fd, F_GETFD) & FD_CLOEXEC));
    lkd_fdobj_put(o);
    lkd_fd_close_on_exec(t);
    LT_ERRNO(SC(t, fcntl, a, LX_F_GETFD), LX_EBADF);
    LT_EQ(SC(t, fcntl, b, LX_F_GETFD), 0);
    LT_ERRNO(SC(t, fcntl, c, LX_F_GETFD), LX_EBADF);
    lkd_set_current(T);
}

/* close_range(2): flags CLOSE_RANGE_UNSHARE (2) and CLOSE_RANGE_CLOEXEC (4). */
LT_TEST(close_range_rules)
{
    lkd_task *t = fs_task();
    for (int i = 0; i < 6; i++) gopen(t, "/f1", LX_O_RDONLY);
    LT_ERRNO(SC(t, close_range, 3, 2, 0), LX_EINVAL);
    LT_ERRNO(SC(t, close_range, 0, 5, 1), LX_EINVAL);
    LT_EQ(SC(t, close_range, 4, 0xffffffffu, 4), 0);
    LT_EQ(SC(t, fcntl, 5, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(t, fcntl, 3, LX_F_GETFD), 0);
    LT_EQ(SC(t, close_range, 3, 4, 0), 0);
    LT_ERRNO(SC(t, fcntl, 3, LX_F_GETFD), LX_EBADF);
    LT_EQ(SC(t, fcntl, 5, LX_F_GETFD), LX_FD_CLOEXEC);
    /* a thread sharing the table (CLONE_FILES) sees the closes; after UNSHARE it no longer does */
    lkd_task *th = fake_make_thread(t);
    LT_CHECK(th->fdt == t->fdt);
    LT_EQ(SC(th, close, 0), 0);
    LT_ERRNO(SC(t, fcntl, 0, LX_F_GETFD), LX_EBADF);
    LT_EQ(SC(th, close_range, 1, 1, 2), 0);   /* unshare, then close fd 1 in the private copy */
    LT_CHECK(th->fdt != t->fdt);
    LT_EQ(SC(t, fcntl, 1, LX_F_GETFD), 0);
    LT_ERRNO(SC(th, fcntl, 1, LX_F_GETFD), LX_EBADF);
    lkd_set_current(T);
}

/* Table copy without CLONE_FILES: descriptors are copied, the descriptions shared. */
LT_TEST(table_clone)
{
    int64_t a = gopen(T, "/f1", LX_O_RDONLY);
    lkd_fdtable *copy = lkd_fdtable_clone(T->fdt, 0);
    LT_CHECK(copy && copy != T->fdt);
    lkd_fdtable *same = lkd_fdtable_clone(T->fdt, LX_CLONE_FILES);
    LT_CHECK(same == T->fdt);
    lkd_fdtable_put(same);
    lkd_task *t2 = fake_make_task();
    t2->fdt = copy;
    t2->fsctx = lkd_fsctx_new();
    char c;
    LT_EQ(SC(t2, read, a, P(&c), 1), 1);
    LT_EQ(SC(T, lseek, a, 0, LX_SEEK_CUR), 1);           /* same description */
    LT_EQ(SC(t2, close, a), 0);
    LT_EQ(SC(T, lseek, a, 0, LX_SEEK_CUR), 1);           /* still open in the original table */
    SC(T, close, a);
    lkd_set_current(T);
}

/* fork(2): the child's descriptors refer to the same open file descriptions (offset shared). */
static int64_t g_fork_fd;
static void child_read(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    char buf[5];
    _exit(SC(T, read, g_fork_fd, P(buf), 5) == 5 ? 0 : 1);
}
LT_TEST(fork_shares_offset)
{
    g_fork_fd = gopen(T, "/f1", LX_O_RDONLY);
    LT_EQ(lt_run_child(child_read, NULL, 10), 0);
    LT_EQ(SC(T, lseek, g_fork_fd, 0, LX_SEEK_CUR), 5);
    SC(T, close, g_fork_fd);
}

/* POSIX record locks: a lock held by the parent is reported to a child by F_GETLK with the parent's
 * pid; F_SETLK conflicts -EAGAIN; OFD locks need l_pid == 0; bad l_type -EINVAL. flock(2) conflicts
 * between two descriptions: LOCK_NB gives -EWOULDBLOCK. */
static int64_t g_lock_fd;
static int32_t g_parent_pid;
static void child_locks(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    struct lx_flock fl = { .l_type = LX_F_WRLCK, .l_whence = LX_SEEK_SET, .l_start = 0, .l_len = 10 };
    if (SC(T, fcntl, g_lock_fd, LX_F_GETLK, P(&fl)) != 0) _exit(1);
    if (fl.l_type != LX_F_WRLCK || fl.l_pid != g_parent_pid || fl.l_len != 10) _exit(2);
    fl.l_type = LX_F_WRLCK;
    if (SC(T, fcntl, g_lock_fd, LX_F_SETLK, P(&fl)) != -LX_EAGAIN) _exit(3);
    int64_t other = gopen(T, "/f2", LX_O_RDONLY);
    if (SC(T, flock, other, LX_LOCK_EX | LX_LOCK_NB) != -LX_EWOULDBLOCK) _exit(4);
    _exit(0);
}
LT_TEST(record_locks)
{
    g_lock_fd = gopen(T, "/f1", LX_O_RDWR);
    g_parent_pid = T->proc->pid;
    struct lx_flock fl = { .l_type = LX_F_WRLCK, .l_whence = LX_SEEK_SET, .l_start = 0, .l_len = 10 };
    LT_EQ(SC(T, fcntl, g_lock_fd, LX_F_SETLK, P(&fl)), 0);
    fl.l_type = LX_F_RDLCK;
    LT_EQ(SC(T, fcntl, g_lock_fd, LX_F_GETLK, P(&fl)), 0);    /* own locks never conflict */
    LT_EQ(fl.l_type, LX_F_UNLCK);
    /* flock on another file: on macOS flock and POSIX locks of the same file conflict (MESURE), Linux
     * keeps them independent; documented gap, so the two kinds are tested on different files */
    int64_t fd2 = gopen(T, "/f2", LX_O_RDONLY);
    LT_EQ(SC(T, flock, fd2, LX_LOCK_EX), 0);
    LT_EQ(lt_run_child(child_locks, NULL, 10), 0);
    struct lx_flock ofd = { .l_type = LX_F_RDLCK, .l_whence = 0, .l_start = 0, .l_len = 1, .l_pid = 5 };
    LT_ERRNO(SC(T, fcntl, g_lock_fd, LX_F_OFD_SETLK, P(&ofd)), LX_EINVAL);
    ofd.l_type = 7;
    ofd.l_pid = 0;
    LT_ERRNO(SC(T, fcntl, g_lock_fd, LX_F_SETLK, P(&ofd)), LX_EINVAL);
    LT_ERRNO(SC(T, flock, fd2, 0), LX_EINVAL);
    LT_EQ(SC(T, flock, fd2, LX_LOCK_UN), 0);
    LT_ERRNO(SC(T, fcntl, 999, LX_F_GETFD), LX_EBADF);
    LT_ERRNO(SC(T, fcntl, g_lock_fd, 0x7777), LX_EINVAL);
    SC(T, close, g_lock_fd);
    SC(T, close, fd2);
}

/* F_GETFL reports the access mode, O_LARGEFILE (x86-64 open forces it) and the changeable flags;
 * O_PATH descriptors only allow F_DUPFD/F_GETFD/F_SETFD/F_GETFL (-EBADF otherwise). */
LT_TEST(status_flags)
{
    int64_t a = gopen(T, "/f1", LX_O_RDWR | LX_O_APPEND);
    int64_t fl = SC(T, fcntl, a, LX_F_GETFL);
    LT_EQ(fl & LX_O_ACCMODE, LX_O_RDWR);
    LT_CHECK(fl & LX_O_APPEND);
    LT_CHECK(fl & LX_O_LARGEFILE);
    LT_EQ(SC(T, fcntl, a, LX_F_SETFL, LX_O_NONBLOCK | LX_O_RDONLY), 0);  /* access mode bits ignored */
    fl = SC(T, fcntl, a, LX_F_GETFL);
    LT_EQ(fl & LX_O_ACCMODE, LX_O_RDWR);
    LT_CHECK(fl & LX_O_NONBLOCK);
    LT_CHECK(!(fl & LX_O_APPEND));
    int32_t on = 0;
    LT_EQ(SC(T, ioctl, a, LX_FIONBIO, P(&on)), 0);
    LT_CHECK(!(SC(T, fcntl, a, LX_F_GETFL) & LX_O_NONBLOCK));
    int64_t p = gopen(T, "/f1", LX_O_PATH);
    LT_CHECK(SC(T, fcntl, p, LX_F_GETFL) & LX_O_PATH);
    LT_ERRNO(SC(T, fcntl, p, LX_F_SETFL, 0), LX_EBADF);
    LT_ERRNO(SC(T, fcntl, p, LX_F_GETLK, 0), LX_EBADF);
    LT_EQ(SC(T, fcntl, p, 1027 /* F_DUPFD_QUERY */, p), 1);
    LT_EQ(SC(T, fcntl, a, 1027, p), 0);
    SC(T, close, a);
    SC(T, close, p);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "fd");
    put_file(ROOT, "f1", "0123456789abcdef");
    put_file(ROOT, "f2", "x");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(lowest_free_fd);
    LT_RUN(dup_shares_description);
    LT_RUN(limits_and_high_fds);
    LT_RUN(cloexec_and_exec);
    LT_RUN(close_range_rules);
    LT_RUN(table_clone);
    LT_RUN(fork_shares_offset);
    LT_RUN(record_locks);
    LT_RUN(status_flags);
    rm_rf(ROOT);
    return lt_finish();
}
