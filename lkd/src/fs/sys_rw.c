/* sys_rw.c: read, write, readv, writev, pread64, pwrite64, preadv, pwritev, preadv2, pwritev2, lseek,
 * sendfile, copy_file_range, splice, vmsplice.
 * Linux references: fs/read_write.c (vfs_read, vfs_write, do_readv, do_iter_readv_writev, ksys_lseek,
 * do_sendfile, vfs_copy_file_range), fs/splice.c.
 */
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include "fs_priv.h"
#include "mm.h"

#define LX_RWF_HIPRI 0x01u
#define LX_RWF_DSYNC 0x02u
#define LX_RWF_SYNC 0x04u
#define LX_RWF_NOWAIT 0x08u
#define LX_RWF_APPEND 0x10u
#define LX_RWF_NOAPPEND 0x20u
#define LX_RWF_SUPPORTED (LX_RWF_HIPRI | LX_RWF_DSYNC | LX_RWF_SYNC | LX_RWF_NOWAIT | LX_RWF_APPEND | LX_RWF_NOAPPEND)

static int can_read(lkd_fdobj *o)
{
    if (fs_obj_is_path(o)) return 0;
    uint32_t a = fs_obj_access(o);
    return a == LX_O_RDONLY || a == LX_O_RDWR;
}
static int can_write(lkd_fdobj *o)
{
    if (fs_obj_is_path(o)) return 0;
    uint32_t a = fs_obj_access(o);
    return a == LX_O_WRONLY || a == LX_O_RDWR;
}
static int seekable(lkd_fdobj *o) { return o->ops && o->ops->lseek && o->kind != LKD_FD_DIR; }

/* Kernel-buffer I/O on an object's host fd (sendfile, splice, copy_file_range, gathered pipe writes).
 * off: explicit offset (updated), or NULL for the file position. */
extern int fs_memfd_limit_write(lkd_fdobj *o, int64_t pos, size_t *n);
static int64_t kio(lkd_task *t, lkd_fdobj *o, void *buf, size_t n, int64_t *off, int is_write, int nowait)
{
    if (o->host_fd < 0) return -LX_EINVAL;
    if (seekable(o) && (o->kind == LKD_FD_FILE || o->kind == LKD_FD_MEMFD)) {
        int64_t pos;
        if (off) pos = *off;
        else {
            pos = o->ops->lseek(t, o, 0, LX_SEEK_CUR);
            if (pos < 0) return pos;
        }
        int64_t b = (int64_t)fs_memfd_bias(o);   /* host phase of a memfd (memfd.c) */
        if (is_write && !off && (fs_obj_getfl(o) & LX_O_APPEND)) {
            struct stat st;
            if (fstat(o->host_fd, &st) < 0) return LKD_HOST_ERR();
            pos = st.st_size - b;
        }
        if (is_write && o->kind == LKD_FD_MEMFD) {
            int rc = fs_memfd_limit_write(o, pos, &n);
            if (rc < 0) return rc;
        }
        ssize_t r;
        do { r = is_write ? pwrite(o->host_fd, buf, n, pos + b) : pread(o->host_fd, buf, n, pos + b); } while (r < 0 && errno == EINTR);
        if (r < 0) return LKD_HOST_ERR();
        if (is_write && r > 0 && o->kind == LKD_FD_MEMFD) lkd_mm_file_written(o, (uint64_t)pos, (uint64_t)r);
        if (off) *off = pos + r;
        else o->ops->lseek(t, o, pos + r, LX_SEEK_SET);
        return r;
    }
    int nb = nowait || (fs_obj_getfl(o) & LX_O_NONBLOCK);
    for (;;) {
        ssize_t r = is_write ? write(o->host_fd, buf, n) : read(o->host_fd, buf, n);
        if (r >= 0) return r;
        if (errno == EINTR) continue;
        if (errno == EAGAIN) {
            if (nb) return -LX_EAGAIN;
            int w = lkd_block_wait_fd(t, o->host_fd, is_write ? POLLOUT : POLLIN, -1);
            if (w < 0) return w;
            continue;
        }
        int64_t e = LKD_HOST_ERR();
        if (e == -LX_EPIPE) fs_sigpipe(t);
        return e;
    }
}

/* Write all of buf (a blocking pipe write is complete unless interrupted). */
static int64_t kio_write_all(lkd_task *t, lkd_fdobj *o, const char *buf, size_t n, int nowait)
{
    size_t done = 0;
    while (done < n) {
        int64_t r = kio(t, o, (void *)(buf + done), n - done, NULL, 1, nowait);
        if (r < 0) return done ? (int64_t)done : r;
        done += (size_t)r;
        if (nowait || (fs_obj_getfl(o) & LX_O_NONBLOCK)) break;
    }
    return (int64_t)done;
}

static int64_t do_rw(lkd_task *t, int fd, uint64_t buf, uint64_t count, const int64_t *off, int is_write)
{
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int64_t r;
    if (!(is_write ? can_write(o) : can_read(o))) r = -LX_EBADF;
    else if (off && !seekable(o)) r = -LX_ESPIPE;
    else if (off && *off < 0) r = -LX_EINVAL;
    else if (o->kind == LKD_FD_DIR && !is_write) r = -LX_EISDIR;
    else if (!(is_write ? o->ops->write : o->ops->read)) r = -LX_EINVAL;
    else {
        if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
        r = is_write ? o->ops->write(t, o, buf, count, off) : o->ops->read(t, o, buf, count, off);
    }
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(read) { return do_rw(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), NULL, 0); }
LKD_SYSCALL(write) { return do_rw(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), NULL, 1); }
LKD_SYSCALL(pread64) { int64_t off = LKD_ARGL(3); return do_rw(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), &off, 0); }
LKD_SYSCALL(pwrite64) { int64_t off = LKD_ARGL(3); return do_rw(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGU(2), &off, 1); }

/* readv/writev family: iovec array read through lkd_user_read; UIO_MAXIOV 1024; the sum of the
 * lengths must fit in ssize_t (-EINVAL), then each segment is transferred in turn and a short segment
 * ends the call (do_loop_readv_writev). A pipe receives the gathered data in one host write when the
 * total is at most the pipe capacity, so writes up to PIPE_BUF stay atomic. */
static int64_t do_rwv(lkd_task *t, int fd, uint64_t iov_gva, int64_t iovcnt, const int64_t *offp, uint32_t rwf, int is_write)
{
    if (iovcnt < 0 || iovcnt > LX_IOV_MAX) return -LX_EINVAL;
    if (rwf & ~LX_RWF_SUPPORTED) return -LX_EOPNOTSUPP;
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int64_t ret = 0;
    struct lx_iovec *iov = NULL;
    int64_t off = offp ? *offp : 0;
    int have_off = offp != NULL;
    if (!(is_write ? can_write(o) : can_read(o))) { ret = -LX_EBADF; goto out; }
    if (have_off && !seekable(o)) { ret = -LX_ESPIPE; goto out; }
    if (have_off && off < 0) { ret = -LX_EINVAL; goto out; }
    if (iovcnt == 0) goto out;
    iov = malloc((size_t)iovcnt * sizeof *iov);
    if (!iov) { ret = -LX_ENOMEM; goto out; }
    if (lkd_user_read(iov_gva, iov, (size_t)iovcnt * sizeof *iov) < 0) { ret = -LX_EFAULT; goto out; }
    uint64_t total = 0;
    for (int64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len > (uint64_t)INT64_MAX - total) { ret = -LX_EINVAL; goto out; }
        total += iov[i].iov_len;
    }
    if (o->kind == LKD_FD_DIR && !is_write) { ret = -LX_EISDIR; goto out; }
    if (!(is_write ? o->ops->write : o->ops->read)) { ret = -LX_EINVAL; goto out; }
    if (is_write && (rwf & LX_RWF_APPEND) && have_off) {
        struct stat st;
        if (o->host_fd >= 0 && fstat(o->host_fd, &st) == 0) off = st.st_size - (int64_t)fs_memfd_bias(o);
    }
    int nowait = (rwf & LX_RWF_NOWAIT) != 0;
    if (o->ops == &fs_pipe_ops && total <= 65536 && iovcnt > 1) {
        char *buf = malloc(total ? total : 1);
        if (!buf) { ret = -LX_ENOMEM; goto out; }
        if (is_write) {
            size_t p = 0;
            for (int64_t i = 0; i < iovcnt && ret == 0; i++) {
                if (iov[i].iov_len && lkd_user_read(iov[i].iov_base, buf + p, iov[i].iov_len) < 0) ret = -LX_EFAULT;
                p += iov[i].iov_len;
            }
            if (ret == 0) ret = kio_write_all(t, o, buf, total, nowait);
        } else {
            ret = kio(t, o, buf, total, NULL, 0, nowait);
            size_t p = 0;
            for (int64_t i = 0; ret > 0 && i < iovcnt && p < (size_t)ret; i++) {
                size_t n = iov[i].iov_len < (size_t)ret - p ? iov[i].iov_len : (size_t)ret - p;
                if (n && lkd_user_write(iov[i].iov_base, buf + p, n) < 0) { ret = p ? (int64_t)p : -LX_EFAULT; break; }
                p += n;
            }
        }
        free(buf);
        goto sync;
    }
    int64_t done = 0;
    for (int64_t i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0) continue;
        uint64_t len = iov[i].iov_len;
        if ((uint64_t)done + len > (uint64_t)FS_MAX_RW) len = (uint64_t)FS_MAX_RW - (uint64_t)done;
        int64_t cur = off + done;
        int64_t r = is_write ? o->ops->write(t, o, iov[i].iov_base, len, have_off ? &cur : NULL)
                             : o->ops->read(t, o, iov[i].iov_base, len, have_off ? &cur : NULL);
        if (r < 0) { if (!done) done = r; break; }
        done += r;
        if ((uint64_t)r < len || (uint64_t)done >= (uint64_t)FS_MAX_RW) break;
    }
    ret = done;
sync:
    if (ret > 0 && is_write && (rwf & (LX_RWF_DSYNC | LX_RWF_SYNC)) && o->ops->fsync) {
        int s = o->ops->fsync(t, o, (rwf & LX_RWF_SYNC) == 0);
        if (s < 0 && s != -LX_EINVAL) ret = s;
    }
out:
    free(iov);
    lkd_fdobj_put(o);
    return ret;
}

LKD_SYSCALL(readv) { return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), NULL, 0, 0); }
LKD_SYSCALL(writev) { return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), NULL, 0, 1); }
/* x86-64: pos_l carries the whole 64-bit offset (pos_from_hilo shifts pos_h out). */
LKD_SYSCALL(preadv) { int64_t off = LKD_ARGL(3); if (off < 0) return -LX_EINVAL; return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), &off, 0, 0); }
LKD_SYSCALL(pwritev) { int64_t off = LKD_ARGL(3); if (off < 0) return -LX_EINVAL; return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), &off, 0, 1); }
LKD_SYSCALL(preadv2)
{
    int64_t off = LKD_ARGL(3);
    return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), off == -1 ? NULL : &off, LKD_ARGUI(5), 0);
}
LKD_SYSCALL(pwritev2)
{
    int64_t off = LKD_ARGL(3);
    return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), off == -1 ? NULL : &off, LKD_ARGUI(5), 1);
}

LKD_SYSCALL(lseek)
{
    int whence = LKD_ARGI(2);
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r;
    if (fs_obj_is_path(o)) r = -LX_EBADF;
    else if ((unsigned)whence > LX_SEEK_HOLE) r = -LX_EINVAL;
    else if (!o->ops->lseek) r = -LX_ESPIPE;
    else r = o->ops->lseek(t, o, LKD_ARGL(1), whence);
    lkd_fdobj_put(o);
    return r;
}

/* sendfile(2): out must be writable and not O_APPEND, in readable; data moves through a 64 KiB kernel
 * buffer. offset NULL: in's file position is used and advanced. */
LKD_SYSCALL(sendfile)
{
    lkd_fdobj *in = lkd_fd_get(t, LKD_ARGI(1), NULL);
    lkd_fdobj *out = lkd_fd_get(t, LKD_ARGI(0), NULL);
    int64_t ret = 0, off = 0;
    uint64_t count = LKD_ARGU(3);
    char *buf = NULL;
    if (!in || !out) { ret = -LX_EBADF; goto done; }
    if (!can_read(in) || !can_write(out)) { ret = -LX_EBADF; goto done; }
    if (fs_obj_getfl(out) & LX_O_APPEND) { ret = -LX_EINVAL; goto done; }
    if (in->host_fd < 0 || out->host_fd < 0 || in->kind == LKD_FD_DIR) { ret = -LX_EINVAL; goto done; }
    if (LKD_ARGP(2)) {
        if (!seekable(in)) { ret = -LX_ESPIPE; goto done; }
        if (lkd_user_read(LKD_ARGP(2), &off, sizeof off) < 0) { ret = -LX_EFAULT; goto done; }
        if (off < 0) { ret = -LX_EINVAL; goto done; }
    }
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    buf = malloc(65536);
    if (!buf) { ret = -LX_ENOMEM; goto done; }
    int64_t moved = 0;
    while ((uint64_t)moved < count) {
        size_t n = (size_t)((count - (uint64_t)moved) > 65536 ? 65536 : (count - (uint64_t)moved));
        int64_t r = kio(t, in, buf, n, LKD_ARGP(2) ? &off : NULL, 0, 0);
        if (r <= 0) { if (r < 0 && !moved) moved = r; break; }
        int64_t w = kio_write_all(t, out, buf, (size_t)r, 0);
        if (w < 0) { if (!moved) moved = w; break; }
        moved += w;
        if (w < r) break;
    }
    ret = moved;
    if (LKD_ARGP(2) && lkd_user_write(LKD_ARGP(2), &off, sizeof off) < 0) ret = -LX_EFAULT;
done:
    free(buf);
    lkd_fdobj_put(in);
    lkd_fdobj_put(out);
    return ret;
}

/* copy_file_range(2): regular files only (-EINVAL), flags must be 0, out not O_APPEND (-EBADF),
 * overlapping ranges of the same file -EINVAL. Copies through a kernel buffer. */
LKD_SYSCALL(copy_file_range)
{
    if (LKD_ARGUI(5) != 0) return -LX_EINVAL;
    lkd_fdobj *in = lkd_fd_get(t, LKD_ARGI(0), NULL);
    lkd_fdobj *out = lkd_fd_get(t, LKD_ARGI(2), NULL);
    int64_t ret = 0, oin = 0, oout = 0;
    uint64_t len = LKD_ARGU(4);
    char *buf = NULL;
    if (!in || !out) { ret = -LX_EBADF; goto done; }
    if (!can_read(in) || !can_write(out) || (fs_obj_getfl(out) & LX_O_APPEND)) { ret = -LX_EBADF; goto done; }
    if (in->kind == LKD_FD_DIR || out->kind == LKD_FD_DIR) { ret = -LX_EISDIR; goto done; }
    if (!((in->kind == LKD_FD_FILE || in->kind == LKD_FD_MEMFD) && (out->kind == LKD_FD_FILE || out->kind == LKD_FD_MEMFD)) ||
        in->host_fd < 0 || out->host_fd < 0) { ret = -LX_EINVAL; goto done; }
    if (LKD_ARGP(1) && lkd_user_read(LKD_ARGP(1), &oin, 8) < 0) { ret = -LX_EFAULT; goto done; }
    if (LKD_ARGP(3) && lkd_user_read(LKD_ARGP(3), &oout, 8) < 0) { ret = -LX_EFAULT; goto done; }
    if (!LKD_ARGP(1)) oin = in->ops->lseek(t, in, 0, LX_SEEK_CUR);
    if (!LKD_ARGP(3)) oout = out->ops->lseek(t, out, 0, LX_SEEK_CUR);
    if (oin < 0 || oout < 0) { ret = -LX_EINVAL; goto done; }
    struct stat si, so;
    if (fstat(in->host_fd, &si) == 0 && fstat(out->host_fd, &so) == 0 && si.st_dev == so.st_dev &&
        si.st_ino == so.st_ino && oin < oout + (int64_t)len && oout < oin + (int64_t)len) { ret = -LX_EINVAL; goto done; }
    if (len > (uint64_t)FS_MAX_RW) len = (uint64_t)FS_MAX_RW;
    buf = malloc(1 << 20);
    if (!buf) { ret = -LX_ENOMEM; goto done; }
    int64_t moved = 0, pin = oin, pout = oout;
    while ((uint64_t)moved < len) {
        size_t n = (size_t)((len - (uint64_t)moved) > (1u << 20) ? (1u << 20) : (len - (uint64_t)moved));
        int64_t r = kio(t, in, buf, n, &pin, 0, 0);
        if (r <= 0) { if (r < 0 && !moved) moved = r; break; }
        int64_t w = kio(t, out, buf, (size_t)r, &pout, 1, 0);
        if (w <= 0) { if (w < 0 && !moved) moved = w; break; }
        moved += w;
        if (w < r) break;
    }
    ret = moved;
    if (moved > 0) {
        if (LKD_ARGP(1)) { if (lkd_user_write(LKD_ARGP(1), &pin, 8) < 0) ret = -LX_EFAULT; }
        else in->ops->lseek(t, in, pin, LX_SEEK_SET);
        if (LKD_ARGP(3)) { if (lkd_user_write(LKD_ARGP(3), &pout, 8) < 0) ret = -LX_EFAULT; }
        else out->ops->lseek(t, out, pout, LX_SEEK_SET);
    }
done:
    free(buf);
    lkd_fdobj_put(in);
    lkd_fdobj_put(out);
    return ret;
}

/* splice(2): one side must be a pipe (-EINVAL), the pipe side takes no offset (-ESPIPE). Data moves
 * through a kernel buffer (one transfer of at most 64 KiB per call), SPLICE_F_NONBLOCK makes the pipe
 * side non-blocking. Not zero-copy (documented). */
#define LX_SPLICE_F_NONBLOCK 0x02u
LKD_SYSCALL(splice)
{
    lkd_fdobj *in = lkd_fd_get(t, LKD_ARGI(0), NULL);
    lkd_fdobj *out = lkd_fd_get(t, LKD_ARGI(2), NULL);
    uint64_t len = LKD_ARGU(4);
    uint32_t flags = LKD_ARGUI(5);
    int64_t ret = 0, oin = 0, oout = 0;
    char *buf = NULL;
    if (!in || !out) { ret = -LX_EBADF; goto done; }
    if (!can_read(in) || !can_write(out)) { ret = -LX_EBADF; goto done; }
    int pin = in->kind == LKD_FD_PIPE, pout = out->kind == LKD_FD_PIPE;
    if (!pin && !pout) { ret = -LX_EINVAL; goto done; }
    if ((pin && LKD_ARGP(1)) || (pout && LKD_ARGP(3))) { ret = -LX_ESPIPE; goto done; }
    if (in->host_fd < 0 || out->host_fd < 0) { ret = -LX_EINVAL; goto done; }
    if (LKD_ARGP(1) && lkd_user_read(LKD_ARGP(1), &oin, 8) < 0) { ret = -LX_EFAULT; goto done; }
    if (LKD_ARGP(3) && lkd_user_read(LKD_ARGP(3), &oout, 8) < 0) { ret = -LX_EFAULT; goto done; }
    if (len == 0) goto done;
    size_t n = len > 65536 ? 65536 : (size_t)len;
    buf = malloc(n);
    if (!buf) { ret = -LX_ENOMEM; goto done; }
    int nb = (flags & LX_SPLICE_F_NONBLOCK) != 0;
    int64_t r = kio(t, in, buf, n, LKD_ARGP(1) ? &oin : NULL, 0, nb && pin);
    if (r <= 0) { ret = r; goto done; }
    if (LKD_ARGP(3)) {
        ret = kio(t, out, buf, (size_t)r, &oout, 1, 0);
        if (ret > 0 && lkd_user_write(LKD_ARGP(3), &oout, 8) < 0) ret = -LX_EFAULT;
    } else {
        ret = kio_write_all(t, out, buf, (size_t)r, nb && pout);
    }
    if (LKD_ARGP(1) && lkd_user_write(LKD_ARGP(1), &oin, 8) < 0) ret = -LX_EFAULT;
done:
    free(buf);
    lkd_fdobj_put(in);
    lkd_fdobj_put(out);
    return ret;
}

/* vmsplice(2): to the write end of a pipe it is writev, from the read end it is readv (Linux >= 6.5
 * copies for the read side as well). Other fds -EBADF. */
LKD_SYSCALL(vmsplice)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int is_pipe = o->kind == LKD_FD_PIPE;
    int wr = can_write(o);
    lkd_fdobj_put(o);
    if (!is_pipe) return -LX_EBADF;
    if (LKD_ARGUI(3) & ~0xfu) return -LX_EINVAL;
    return do_rwv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGL(2), NULL, (LKD_ARGUI(3) & LX_SPLICE_F_NONBLOCK) ? LX_RWF_NOWAIT : 0, wr);
}

void fs_register_rw(void)
{
    LKD_REGISTER(read);
    LKD_REGISTER(write);
    LKD_REGISTER(pread64);
    LKD_REGISTER(pwrite64);
    LKD_REGISTER(readv);
    LKD_REGISTER(writev);
    LKD_REGISTER(preadv);
    LKD_REGISTER(pwritev);
    LKD_REGISTER(preadv2);
    LKD_REGISTER(pwritev2);
    LKD_REGISTER(lseek);
    LKD_REGISTER(sendfile);
    LKD_REGISTER(copy_file_range);
    LKD_REGISTER(splice);
    LKD_REGISTER(vmsplice);
}
