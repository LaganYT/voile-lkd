/* test_fs_pipe.c: pipe(2)/pipe2(2) semantics (fs/pipe.c, pipe(7)): capacity 65536, EOF when no writer,
 * EPIPE plus SIGPIPE to the writing thread when no reader, O_NONBLOCK -EAGAIN, blocking read and write
 * that wait and are interruptible (restartable: -ERESTARTSYS inside, -EINTR to the guest when a handler
 * runs without SA_RESTART), F_GETPIPE_SZ, FIONREAD, splice/vmsplice, fork sharing. */
#include <pthread.h>

#include "test_fs_util.h"
#include "sig.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

/* Works with the placeholder sig module and with the real one: only the interruption hint is cleared;
 * signals the tests leave pending stay blocked. */
static void clear_signals(lkd_task *t)
{
    atomic_fetch_and(&t->flags, ~(uint32_t)(LKD_TF_SIGPENDING | LKD_TF_FATAL));
    char b[64];
    while (read(t->wake_rd, b, sizeof b) > 0) { }
}

static int sigpipe_pending(lkd_task *t)
{
    struct lkd_sig_status st;
    lkd_sig_status(t, &st);
    return ((st.pnd | st.shdpnd) & LX_SIGBIT(LX_SIGPIPE)) != 0;
}

LT_TEST(pipe2_flags)
{
    int32_t p[2] = { -1, -1 };
    LT_ERRNO(SC(T, pipe2, P(p), 0x1), LX_EINVAL);
    LT_ERRNO(SC(T, pipe2, P(p), LX_O_DIRECT), LX_EINVAL);   /* packet mode not available on host pipes */
    LT_EQ(SC(T, pipe2, P(p), LX_O_CLOEXEC | LX_O_NONBLOCK), 0);
    LT_EQ(SC(T, fcntl, p[0], LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, fcntl, p[1], LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, fcntl, p[0], LX_F_GETFL), LX_O_RDONLY | LX_O_NONBLOCK);
    LT_EQ(SC(T, fcntl, p[1], LX_F_GETFL), LX_O_WRONLY | LX_O_NONBLOCK);
    char c;
    LT_ERRNO(SC(T, read, p[0], P(&c), 1), LX_EAGAIN);
    LT_ERRNO(SC(T, write, p[0], P("x"), 1), LX_EBADF);
    LT_ERRNO(SC(T, read, p[1], P(&c), 1), LX_EBADF);
    LT_ERRNO(SC(T, lseek, p[0], 0, LX_SEEK_CUR), LX_ESPIPE);
    LT_ERRNO(SC(T, pread64, p[0], P(&c), 1, 0), LX_ESPIPE);
    LT_ERRNO(SC(T, fcntl, p[1], LX_F_SETFL, LX_O_ASYNC), LX_ENOSYS);   /* SIGIO not implemented: visible */
    LT_ERRNO(SC(T, fsync, p[1]), LX_EINVAL);
    LT_EQ(SC(T, pipe, P(p)), 0);
    LT_EQ(SC(T, fcntl, p[0], LX_F_GETFD), 0);
    SC(T, close, p[0]);
    SC(T, close, p[1]);
}

LT_TEST(capacity_nonblock_eof)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe2, P(p), LX_O_NONBLOCK), 0);
    LT_EQ(SC(T, fcntl, p[0], LX_F_GETPIPE_SZ), 65536);
    LT_EQ(SC(T, fcntl, p[1], LX_F_SETPIPE_SZ, 4096), 65536);
    LT_ERRNO(SC(T, fcntl, p[1], LX_F_SETPIPE_SZ, 1 << 20), LX_EPERM);
    static char buf[70000];
    memset(buf, 'q', sizeof buf);
    LT_EQ(SC(T, write, p[1], P(buf), sizeof buf), 65536);          /* partial, then full */
    LT_ERRNO(SC(T, write, p[1], P(buf), 1), LX_EAGAIN);
    int32_t avail = 0;
    LT_EQ(SC(T, ioctl, p[0], LX_FIONREAD, P(&avail)), 0);
    LT_EQ(avail, 65536);
    LT_EQ(SC(T, read, p[0], P(buf), sizeof buf), 65536);
    LT_EQ(SC(T, write, p[1], P("abc"), 3), 3);
    LT_EQ(SC(T, close, p[1]), 0);
    char r[8];
    LT_EQ(SC(T, read, p[0], P(r), sizeof r), 3);
    LT_EQ(SC(T, read, p[0], P(r), sizeof r), 0);                   /* EOF: no writer left */
    SC(T, close, p[0]);
    int fd = (int)SC(T, openat, AT_CWD, P("/f"), LX_O_RDONLY, 0);
    LT_ERRNO(SC(T, fcntl, fd, LX_F_GETPIPE_SZ), LX_EBADF);
    SC(T, close, fd);
}

/* pipe(7): "If all file descriptors referring to the read end of a pipe have been closed, then a
 * write(2) will cause a SIGPIPE signal to be generated for the calling process" (fs/pipe.c sends it
 * to current) and the write fails with EPIPE. */
LT_TEST(epipe_and_sigpipe)
{
    clear_signals(T);
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGPIPE));   /* blocked: it stays pending instead of killing */
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    SC(T, close, p[0]);
    LT_CHECK(!sigpipe_pending(T));
    LT_ERRNO(SC(T, write, p[1], P("x"), 1), LX_EPIPE);
    LT_CHECK(sigpipe_pending(T));
    clear_signals(T);
    struct lx_iovec iov[2] = { { P("a"), 1 }, { P("b"), 1 } };
    LT_ERRNO(SC(T, writev, p[1], P(iov), 2), LX_EPIPE);
    LT_CHECK(sigpipe_pending(T));
    clear_signals(T);
    SC(T, close, p[1]);
}

struct helper { lkd_task *t; int fd; int mode; int delay_ms; char data[8]; lkd_task *victim; };
static void *helper_main(void *arg)
{
    struct helper *h = arg;
    lkd_set_current(h->t);
    usleep((useconds_t)h->delay_ms * 1000);
    if (h->mode == 0) SC(h->t, write, h->fd, P(h->data), strlen(h->data));
    else if (h->mode == 1) { static char sink[70000]; SC(h->t, read, h->fd, P(sink), sizeof sink); }
    else { atomic_fetch_or(&h->victim->flags, LKD_TF_SIGPENDING); lkd_task_wake(h->victim); }
    return NULL;
}

/* A blocking read sleeps until data arrives; a blocking write on a full pipe waits for room; an
 * interruption ends the wait with -ERESTARTSYS (read is restartable, CONVENTIONS 4.2). */
LT_TEST(blocking_and_interrupt)
{
    clear_signals(T);
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    lkd_task *th = fake_make_thread(T);
    lkd_set_current(T);
    struct helper h = { .t = th, .fd = p[1], .mode = 0, .delay_ms = 50 };
    snprintf(h.data, sizeof h.data, "late");
    pthread_t pt;
    pthread_create(&pt, NULL, helper_main, &h);
    char buf[16] = { 0 };
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(SC(T, read, p[0], P(buf), sizeof buf), 4);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 30000000);
    LT_STREQ(buf, "late");
    pthread_join(pt, NULL);
    /* full pipe: a blocking 70000-byte write completes once a reader drains it */
    struct helper r = { .t = th, .fd = p[0], .mode = 1, .delay_ms = 50 };
    pthread_create(&pt, NULL, helper_main, &r);
    static char big[70000];
    memset(big, 'z', sizeof big);
    LT_EQ(SC(T, write, p[1], P(big), sizeof big), 70000);
    pthread_join(pt, NULL);
    /* drain what is left after the helper's single read */
    int32_t avail = 0;
    SC(T, ioctl, p[0], LX_FIONREAD, P(&avail));
    static char sink[70000];
    if (avail > 0) SC(T, read, p[0], P(sink), (uint64_t)avail);
    /* interruption */
    struct helper s = { .t = th, .mode = 2, .delay_ms = 50, .victim = T };
    pthread_create(&pt, NULL, helper_main, &s);
    lkd_sysreq rq = { .nr = LKD_NR_read, .a = { (uint64_t)p[0], P(buf), sizeof buf } };
    LT_ERRNO(lkd_dispatch(T, &rq), LX_ERESTARTSYS);
    pthread_join(pt, NULL);
    /* Through lkd_syscall the restart decision belongs to sig: no handler to run means restart
     * (LKD_RESTART), a handler without SA_RESTART means -EINTR. Either is correct here. */
    int64_t g = SC(T, read, p[0], P(buf), sizeof buf);
    LT_CHECK(g == -LX_EINTR || g == -100000 - LKD_RESTART);
    clear_signals(T);
    SC(T, close, p[0]);
    SC(T, close, p[1]);
}

/* pipefs inode: S_IFIFO|0600, both ends share the inode number, readlink text "pipe:[ino]". */
LT_TEST(pipe_identity)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    struct lx_stat a, b;
    LT_EQ(SC(T, fstat, p[0], P(&a)), 0);
    LT_EQ(SC(T, fstat, p[1], P(&b)), 0);
    LT_EQ(a.st_mode, LX_S_IFIFO | 0600);
    LT_EQ(a.st_ino, b.st_ino);
    LT_EQ(a.st_nlink, 1);
    lkd_fdobj *o = lkd_fd_get(T, p[0], NULL);
    char txt[64], want[64];
    int n = lkd_fs_describe(o, txt, sizeof txt);
    snprintf(want, sizeof want, "pipe:[%llu]", (unsigned long long)a.st_ino);
    LT_EQ(n, (int)strlen(want));
    txt[n > 0 ? n : 0] = 0;
    LT_STREQ(txt, want);
    /* /proc/self/fd reopen: the same direction works (shares the pipe), the other end is not held */
    lkd_fdobj *n1 = NULL;
    LT_EQ(lkd_fdobj_reopen(T, o, LX_O_RDONLY, &n1), 0);
    lkd_fdobj *n2 = NULL;
    LT_ERRNO(lkd_fdobj_reopen(T, o, LX_O_WRONLY, &n2), LX_ENXIO);
    lkd_fdobj_put(n1);
    lkd_fdobj_put(o);
    struct lx_statfs sf;
    LT_EQ(SC(T, fstatfs, p[0], P(&sf)), 0);
    LT_EQ(sf.f_type, 0x50495045);   /* PIPEFS_MAGIC */
    SC(T, close, p[0]);
    SC(T, close, p[1]);
}

/* splice(2) needs a pipe on one side (-EINVAL), no offset on the pipe side (-ESPIPE); vmsplice(2) to a
 * write end works like writev. */
LT_TEST(splice_vmsplice)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    int64_t f = SC(T, openat, AT_CWD, P("/f"), LX_O_RDWR, 0);
    int64_t g = SC(T, openat, AT_CWD, P("/g"), LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0644);
    LT_EQ(SC(T, splice, f, 0, p[1], 0, 5, 0), 5);
    int64_t off = 0;
    LT_EQ(SC(T, splice, p[0], 0, g, P(&off), 5, 0), 5);
    LT_EQ(off, 5);
    char buf[8];
    LT_EQ(SC(T, pread64, g, P(buf), 5, 0), 5);
    LT_MEMEQ(buf, "hello", 5);
    LT_ERRNO(SC(T, splice, f, 0, g, 0, 5, 0), LX_EINVAL);
    LT_ERRNO(SC(T, splice, p[0], P(&off), g, 0, 5, 0), LX_ESPIPE);
    struct lx_iovec iov[2] = { { P("ab"), 2 }, { P("cd"), 2 } };
    LT_EQ(SC(T, vmsplice, p[1], P(iov), 2, 0), 4);
    LT_EQ(SC(T, read, p[0], P(buf), sizeof buf), 4);
    LT_MEMEQ(buf, "abcd", 4);
    LT_ERRNO(SC(T, vmsplice, f, P(iov), 2, 0), LX_EBADF);
    SC(T, close, f); SC(T, close, g); SC(T, close, p[0]); SC(T, close, p[1]);
}

/* fork: the child writes into the inherited write end, the parent reads it. */
static int32_t g_wp;
static void child_writes(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    _exit(SC(T, write, g_wp, P("kid"), 3) == 3 ? 0 : 1);
}
LT_TEST(fork_shares_pipe)
{
    int32_t p[2];
    LT_EQ(SC(T, pipe, P(p)), 0);
    g_wp = p[1];
    LT_EQ(lt_run_child(child_writes, NULL, 10), 0);
    char buf[8] = { 0 };
    LT_EQ(SC(T, read, p[0], P(buf), sizeof buf), 3);
    LT_STREQ(buf, "kid");
    SC(T, close, p[0]);
    SC(T, close, p[1]);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "pipe");
    put_file(ROOT, "f", "hello world");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(pipe2_flags);
    LT_RUN(capacity_nonblock_eof);
    LT_RUN(epipe_and_sigpipe);
    LT_RUN(blocking_and_interrupt);
    LT_RUN(pipe_identity);
    LT_RUN(splice_vmsplice);
    LT_RUN(fork_shares_pipe);
    rm_rf(ROOT);
    return lt_finish();
}
