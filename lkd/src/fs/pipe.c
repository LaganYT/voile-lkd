/* pipe.c: pipe(2)/pipe2(2) objects over host pipes.
 *
 * The host pipe is always O_NONBLOCK (it is created by LKD and private to the family): a blocking guest
 * read or write waits interruptibly in lkd_block_wait_fd and never sleeps inside the host kernel. The
 * guest O_NONBLOCK flag lives in the object (consequence: F_SETFL O_NONBLOCK in one process is not seen
 * by a forked process sharing the pipe, documented gap). Host F_SETNOSIGPIPE: the host never raises
 * SIGPIPE; a write to a pipe without readers returns -EPIPE and sends SIGPIPE to the calling thread
 * like Linux pipe_write (send_sig(SIGPIPE, current, 0)).
 * Capacity: 65536 bytes, MESURE on macOS 27 (non-blocking fill of a fresh pipe), the Linux default.
 */
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fs_priv.h"
#include "sig.h"

#define FS_PIPE_CAPACITY 65536

void fs_sigpipe(lkd_task *t)
{
    if (t) lkd_sig_send_task(t, LX_SIGPIPE, NULL, 1);
}

static int64_t pipe_rw(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, int is_write)
{
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    int nonblock = (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0;
    size_t chunk = count > FS_PIPE_CAPACITY ? FS_PIPE_CAPACITY : (size_t)count;
    char small[4096];
    char *buf = chunk <= sizeof small ? small : malloc(chunk);
    if (!buf) return -LX_ENOMEM;
    int64_t done = 0, ret = 0;
    size_t pending = 0, pend_off = 0; /* bytes of buf not yet written */
    while ((uint64_t)done < count || pending) {
        if (is_write && !pending) {
            size_t n = (size_t)(count - (uint64_t)done);
            if (n > chunk) n = chunk;
            if (lkd_user_read(gva + (uint64_t)done, buf, n) < 0) { ret = -LX_EFAULT; break; }
            pending = n;
            pend_off = 0;
        }
        ssize_t r;
        if (is_write) r = write(o->host_fd, buf + pend_off, pending);
        else r = read(o->host_fd, buf, (size_t)((count - (uint64_t)done) > chunk ? chunk : (count - (uint64_t)done)));
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                if (nonblock || (!is_write && done > 0)) { ret = -LX_EAGAIN; break; }
                int w = lkd_block_wait_fd(t, o->host_fd, is_write ? POLLOUT : POLLIN, -1);
                if (w < 0) { ret = w; break; }
                continue;
            }
            ret = LKD_HOST_ERR();
            if (ret == -LX_EPIPE) fs_sigpipe(t);
            break;
        }
        if (!is_write) {
            if (r > 0 && lkd_user_write(gva + (uint64_t)done, buf, (size_t)r) < 0) { ret = -LX_EFAULT; break; }
            done += r;
            break;                       /* read returns what is there (0 at EOF) */
        }
        pend_off += (size_t)r;
        pending -= (size_t)r;
        done += r;
        if (pending == 0 && (uint64_t)done >= count) break;
    }
    if (buf != small) free(buf);
    if (done > 0) return done;
    return ret;
}

static int64_t pipe_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    if (fs_obj_access(o) != LX_O_RDONLY) return -LX_EBADF;
    if (count == 0) return 0;
    return pipe_rw(t, o, gva, count, 0);
}

static int64_t pipe_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    if (fs_obj_access(o) != LX_O_WRONLY) return -LX_EBADF;
    if (count == 0) return 0;
    return pipe_rw(t, o, gva, count, 1);
}

static int64_t pipe_ioctl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t;
    if (cmd != LX_FIONREAD) return -LX_ENOTTY;
    int avail = 0;
    if (ioctl(o->host_fd, FIONREAD, &avail) < 0) return LKD_HOST_ERR();
    int32_t v = avail;
    return lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
}

/* F_GETPIPE_SZ / F_SETPIPE_SZ (fs/pipe.c pipe_fcntl). The host capacity is fixed at 65536: a request
 * up to it keeps 65536 (Linux rounds up to a power of two of pages and may grow, never below the data
 * held); a larger request is refused with -EPERM, the answer Linux gives an unprivileged caller above
 * fs.pipe-max-size (which is therefore 65536 here, documented). */
static int64_t pipe_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o;
    if (cmd == LX_F_GETPIPE_SZ) return FS_PIPE_CAPACITY;
    if (cmd == LX_F_SETPIPE_SZ) {
        if (arg == 0 || arg > 0x80000000ull) return -LX_EINVAL;
        if (arg > FS_PIPE_CAPACITY) return -LX_EPERM;
        return FS_PIPE_CAPACITY;
    }
    return -LX_EINVAL;
}

static uint32_t pipe_poll(lkd_fdobj *o, uint32_t want)
{
    struct pollfd p = { .fd = o->host_fd, .events = POLLIN | POLLOUT };
    if (poll(&p, 1, 0) < 0) return 0;
    uint32_t ev = 0;
    if ((atomic_load(&o->status_flags) & LX_O_ACCMODE) == LX_O_WRONLY && (p.revents & POLLHUP)) {
        /* write end whose readers are gone: Linux pipe_poll gives EPOLLERR and, while the pipe is not
         * full, EPOLLOUT (a write returns EPIPE at once); never HUP or IN. macOS reports IN|HUP here
         * (MESURE, request net-1). A full pipe without readers is reported OUT too (gap: Linux ERR only). */
        ev = LX_POLLOUT | LX_POLLWRNORM | LX_POLLERR;
        return ev & (want | LX_POLLERR);
    }
    if (p.revents & POLLIN) ev |= LX_POLLIN | LX_POLLRDNORM;
    if (p.revents & POLLOUT) ev |= LX_POLLOUT | LX_POLLWRNORM;
    if (p.revents & POLLHUP) ev |= LX_POLLHUP;
    if (p.revents & POLLERR) ev |= LX_POLLERR;
    return ev & (want | LX_POLLHUP | LX_POLLERR);
}

/* pipefs inode: mode S_IFIFO|0600, owner the creator, st_dev of pipefs, nlink 1 (fs/pipe.c
 * get_pipe_inode). The inode number is the host pipe's, equal for both ends and across fork. */
static int pipe_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    (void)t;
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) return LKD_HOST_ERR();
    memset(out, 0, sizeof *out);
    out->st_dev = 0xd;              /* anonymous pipefs superblock: 0:13 is typical, any fixed value */
    out->st_ino = o->ino;
    out->st_nlink = 1;
    out->st_mode = LX_S_IFIFO | 0600;
    out->st_uid = fs_uid_from_host(geteuid());
    out->st_gid = fs_gid_from_host(getegid());
    out->st_blksize = 4096;
    out->st_atime_sec = out->st_mtime_sec = out->st_ctime_sec = st.st_mtimespec.tv_sec;
    out->st_atime_nsec = out->st_mtime_nsec = out->st_ctime_nsec = st.st_mtimespec.tv_nsec;
    return 0;
}

static int pipe_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "pipe:[%llu]", (unsigned long long)o->ino);
    return n < 0 ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

static int pipe_fsync(lkd_task *t, lkd_fdobj *o, int datasync) { (void)t; (void)o; (void)datasync; return -LX_EINVAL; }

const lkd_fd_ops fs_pipe_ops = {
    .name = "pipe", .read = pipe_read, .write = pipe_write, .ioctl = pipe_ioctl, .fcntl = pipe_fcntl,
    .poll = pipe_poll, .fstat = pipe_fstat, .fsync = pipe_fsync, .describe = pipe_describe,
};

int fs_pipe_create(int nonblock, lkd_fdobj **rd, lkd_fdobj **wr)
{
    int p[2];
    if (pipe(p) < 0) return LKD_HOST_ERR();
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
        fcntl(p[i], F_SETFL, O_NONBLOCK);
        fcntl(p[i], F_SETNOSIGPIPE, 1);
    }
    struct stat st;
    uint64_t ino = fstat(p[0], &st) == 0 ? (uint64_t)st.st_ino : 0;
    uint32_t nb = nonblock ? LX_O_NONBLOCK : 0;
    lkd_fdobj *r = lkd_fdobj_new(LKD_FD_PIPE, &fs_pipe_ops, p[0], LX_O_RDONLY | nb, NULL);
    lkd_fdobj *w = r ? lkd_fdobj_new(LKD_FD_PIPE, &fs_pipe_ops, p[1], LX_O_WRONLY | nb, NULL) : NULL;
    if (!r || !w) {
        if (r) lkd_fdobj_put(r); else close(p[0]);
        close(p[1]);
        return -LX_ENFILE;
    }
    if (ino) { r->ino = ino; w->ino = ino; }
    else w->ino = r->ino;
    *rd = r;
    *wr = w;
    return 0;
}

/* pipe2(2): flags O_CLOEXEC, O_NONBLOCK; O_DIRECT (packet mode) is not available on a host pipe and is
 * refused with -EINVAL (the answer of kernels before 3.4, documented). */
static int64_t do_pipe2(lkd_task *t, uint64_t fds_gva, uint32_t flags)
{
    if (flags & ~(uint32_t)(LX_O_CLOEXEC | LX_O_NONBLOCK)) return -LX_EINVAL;
    lkd_fdobj *r, *w;
    int rc = fs_pipe_create((flags & LX_O_NONBLOCK) != 0, &r, &w);
    if (rc < 0) return rc;
    int cloexec = (flags & LX_O_CLOEXEC) != 0;
    int fr = lkd_fd_alloc(t, r, 0, cloexec);
    int fw = fr >= 0 ? lkd_fd_alloc(t, w, 0, cloexec) : -1;
    lkd_fdobj_put(r);
    lkd_fdobj_put(w);
    if (fr < 0) return fr;
    if (fw < 0) { lkd_fd_close(t, fr); return fw; }
    int32_t v[2] = { fr, fw };
    if (lkd_user_write(fds_gva, v, sizeof v) < 0) {
        lkd_fd_close(t, fr);
        lkd_fd_close(t, fw);
        return -LX_EFAULT;
    }
    return 0;
}

LKD_SYSCALL(pipe) { return do_pipe2(t, LKD_ARGP(0), 0); }
LKD_SYSCALL(pipe2) { return do_pipe2(t, LKD_ARGP(0), LKD_ARGUI(1)); }

void fs_register_pipe(void)
{
    LKD_REGISTER(pipe);
    LKD_REGISTER(pipe2);
}
