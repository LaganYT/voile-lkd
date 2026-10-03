/* file.c: host-backed file, directory, O_PATH and stream objects; stat translation; reopen.
 *
 * Ownership mapping (documented in docs/status/fs.md): a host file owned by the host user of the
 * runtime is presented as owned by the guest identity (lkd_config uid/gid, default 1000), host root
 * stays 0, any other host owner is presented as the overflow id 65534 (Linux overflowuid), the value
 * Linux shows for ids that are not mapped in a user namespace.
 */
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <unistd.h>

#include "fs_priv.h"
#include "mm.h"

/* ---- Identity mapping -------------------------------------------------------------------------- */

static uint32_t guest_uid(void) { const lkd_config *c = lkd_config_get(); return c && c->uid ? c->uid : 1000; }
static uint32_t guest_gid(void) { const lkd_config *c = lkd_config_get(); return c && c->gid ? c->gid : 1000; }

uint32_t fs_uid_from_host(uid_t u)
{
    if (u == getuid() || u == geteuid()) return guest_uid();
    return u == 0 ? 0 : 65534;
}
uint32_t fs_gid_from_host(gid_t g)
{
    if (g == getgid() || g == getegid()) return guest_gid();
    return g == 0 ? 0 : 65534;
}
/* chown(2) as an unprivileged Linux user: only to oneself (CAP_CHOWN otherwise, -EPERM). */
int fs_uid_to_host(uint32_t u, uid_t *out)
{
    if (u == (uint32_t)-1) { *out = (uid_t)-1; return 0; }
    if (u == guest_uid()) { *out = geteuid(); return 0; }
    return -LX_EPERM;
}
int fs_gid_to_host(uint32_t g, gid_t *out)
{
    if (g == (uint32_t)-1) { *out = (gid_t)-1; return 0; }
    if (g == guest_gid()) { *out = getegid(); return 0; }
    return -LX_EPERM;
}

void fs_stat_from_host(const struct stat *hs, struct lx_stat *ls)
{
    lkd_stat_from_host(hs, ls);
    ls->st_uid = fs_uid_from_host(hs->st_uid);
    ls->st_gid = fs_gid_from_host(hs->st_gid);
}

void fs_statx_from_host(const struct stat *hs, uint32_t mask, struct lx_statx *x)
{
    lkd_statx_from_host(hs, mask, x);
    x->stx_uid = fs_uid_from_host(hs->st_uid);
    x->stx_gid = fs_gid_from_host(hs->st_gid);
    uint64_t d = lkd_dev_to_linux(hs->st_dev), rd = lkd_dev_to_linux(hs->st_rdev);
    x->stx_dev_major = (uint32_t)((d >> 8) & 0xfff);
    x->stx_dev_minor = (uint32_t)((d & 0xff) | ((d >> 12) & ~0xffull));
    x->stx_rdev_major = (uint32_t)((rd >> 8) & 0xfff);
    x->stx_rdev_minor = (uint32_t)((rd & 0xff) | ((rd >> 12) & ~0xffull));
}

void fs_stat_from_vattr(const struct fs_mount *m, const struct lkd_vattr *va, struct lx_stat *ls)
{
    memset(ls, 0, sizeof *ls);
    ls->st_dev = m ? m->lx_dev : 0;
    ls->st_ino = va->ino;
    ls->st_nlink = va->nlink ? va->nlink : 1;
    ls->st_mode = va->mode;
    ls->st_uid = va->uid;
    ls->st_gid = va->gid;
    ls->st_rdev = va->rdev;
    ls->st_size = (int64_t)va->size;
    ls->st_blksize = 1024; /* procfs and sysfs report 1024 (fs/proc/inode.c sb->s_blocksize) */
    ls->st_blocks = 0;
    ls->st_atime_sec = ls->st_mtime_sec = ls->st_ctime_sec = va->mtime_sec;
    ls->st_atime_nsec = ls->st_mtime_nsec = ls->st_ctime_nsec = va->mtime_nsec;
}

void fs_statx_from_lxstat(const struct lx_stat *s, struct lx_statx *x)
{
    memset(x, 0, sizeof *x);
    x->stx_mask = LX_STATX_BASIC_STATS;
    x->stx_blksize = (uint32_t)s->st_blksize;
    x->stx_nlink = (uint32_t)s->st_nlink;
    x->stx_uid = s->st_uid;
    x->stx_gid = s->st_gid;
    x->stx_mode = (uint16_t)s->st_mode;
    x->stx_ino = s->st_ino;
    x->stx_size = (uint64_t)s->st_size;
    x->stx_blocks = (uint64_t)s->st_blocks;
    x->stx_atime.tv_sec = s->st_atime_sec; x->stx_atime.tv_nsec = (uint32_t)s->st_atime_nsec;
    x->stx_mtime.tv_sec = s->st_mtime_sec; x->stx_mtime.tv_nsec = (uint32_t)s->st_mtime_nsec;
    x->stx_ctime.tv_sec = s->st_ctime_sec; x->stx_ctime.tv_nsec = (uint32_t)s->st_ctime_nsec;
    x->stx_dev_major = (uint32_t)((s->st_dev >> 8) & 0xfff);
    x->stx_dev_minor = (uint32_t)((s->st_dev & 0xff) | ((s->st_dev >> 12) & ~0xffull));
    x->stx_rdev_major = (uint32_t)((s->st_rdev >> 8) & 0xfff);
    x->stx_rdev_minor = (uint32_t)((s->st_rdev & 0xff) | ((s->st_rdev >> 12) & ~0xffull));
}

/* ---- Guest memory <-> host fd ------------------------------------------------------------------ */

int fs_get_path(uint64_t gva, char *buf)
{
    int64_t n = lkd_user_strn(gva, buf, LX_PATH_MAX);
    return (int)n;
}

/* Regular files: chunked direct I/O on lkd_g2h after validation; the first failing chunk ends the
 * transfer with the bytes done so far (Linux copy_to_user/copy_from_user partial semantics). */
int64_t fs_host_io(int host_fd, uint64_t gva, uint64_t count, const int64_t *off, int is_write)
{
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    int64_t done = 0;
    while ((uint64_t)done < count) {
        size_t chunk = (size_t)(count - (uint64_t)done);
        if (chunk > (1u << 20)) chunk = 1u << 20;
        if (lkd_mm_check_range(gva + (uint64_t)done, chunk, !is_write) < 0) return done ? done : -LX_EFAULT;
        void *p = lkd_g2h(gva + (uint64_t)done);
        ssize_t r;
        if (off) r = is_write ? pwrite(host_fd, p, chunk, *off + done) : pread(host_fd, p, chunk, *off + done);
        else r = is_write ? write(host_fd, p, chunk) : read(host_fd, p, chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            return done ? done : LKD_HOST_ERR();
        }
        done += r;
        if ((size_t)r < chunk) break;
    }
    return done;
}

/* ---- Status flags ------------------------------------------------------------------------------ */

#define FS_HOST_TRACKED (LX_O_NONBLOCK | LX_O_APPEND | LX_O_DSYNC | LX_O_SYNC | LX_O_ASYNC)

static int tracks_host_flags(lkd_fdobj *o)
{
    if (o->host_fd < 0) return 0;
    if (o->ops == &fs_pipe_ops || o->ops == &fs_path_ops) return 0;
    return o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_stream_ops || o->ops == &fs_memfd_ops;
}

uint32_t fs_obj_getfl(lkd_fdobj *o)
{
    uint32_t fl = atomic_load(&o->status_flags);
    if (tracks_host_flags(o)) {
        int h = fcntl(o->host_fd, F_GETFL);
        if (h >= 0) fl = (fl & ~(uint32_t)FS_HOST_TRACKED) | (lkd_open_flags_from_host(h) & FS_HOST_TRACKED);
    }
    return fl;
}

/* fcntl F_SETFL (fs/fcntl.c setfl): O_APPEND, O_NONBLOCK, O_DIRECT, O_NOATIME, O_ASYNC change. */
int fs_obj_setfl(lkd_task *t, lkd_fdobj *o, uint32_t arg)
{
    (void)t;
    const uint32_t mask = LX_O_APPEND | LX_O_NONBLOCK | LX_O_DIRECT | LX_O_NOATIME | LX_O_ASYNC;
    uint32_t cur = fs_obj_getfl(o);
    if ((arg & LX_O_ASYNC) && !(cur & LX_O_ASYNC) && o->kind != LKD_FD_FILE && o->kind != LKD_FD_DIR) {
        /* SIGIO delivery is not implemented: refuse visibly instead of accepting a flag that would
         * never produce a signal. Regular files have no fasync on Linux either (flag only). */
        lkd_log_once("fs: F_SETFL O_ASYNC on a %s: SIGIO not implemented (-ENOSYS)", o->ops ? o->ops->name : "?");
        return -LX_ENOSYS;
    }
    if (tracks_host_flags(o)) {
        int h = fcntl(o->host_fd, F_GETFL);
        if (h < 0) return LKD_HOST_ERR();
        h &= ~(O_APPEND | O_NONBLOCK);
        if (arg & LX_O_APPEND) h |= O_APPEND;
        if (arg & LX_O_NONBLOCK) h |= O_NONBLOCK;
        if (fcntl(o->host_fd, F_SETFL, h) < 0) return LKD_HOST_ERR();
        if (o->kind == LKD_FD_FILE && ((arg ^ cur) & LX_O_DIRECT))
            fcntl(o->host_fd, F_NOCACHE, (arg & LX_O_DIRECT) ? 1 : 0);
    }
    uint32_t fl = atomic_load(&o->status_flags);
    fl = (fl & ~mask) | (arg & mask);
    atomic_store(&o->status_flags, fl);
    return 0;
}

uint32_t fs_obj_access(lkd_fdobj *o) { return atomic_load(&o->status_flags) & LX_O_ACCMODE; }
int fs_obj_is_path(lkd_fdobj *o) { return o->ops == &fs_path_ops; }

/* ---- fs_file private state --------------------------------------------------------------------- */

static struct fs_file *file_new(const char *host_path, const struct fs_mount *m)
{
    struct fs_file *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    if (host_path && !(f->host_path = strdup(host_path))) { free(f); return NULL; }
    if (m) { f->ro_mount = m->ro; f->mount_id = m->id; f->lx_dev = m->lx_dev; f->mnt = m; }
    return f;
}

static void dir_snapshot_free(struct fs_file *f)
{
    for (size_t i = 0; i < f->nents; i++) free(f->ents[i].name);
    free(f->ents);
    f->ents = NULL;
    f->nents = 0;
    f->snap_valid = 0;
}

static void file_release(lkd_fdobj *o)
{
    struct fs_file *f = o->priv;
    if (!f) return;
    dir_snapshot_free(f);
    free(f->host_path);
    free(f);
    o->priv = NULL;
}

int fs_obj_hoststat(lkd_fdobj *o, struct stat *st)
{
    if (o->host_fd >= 0) return fstat(o->host_fd, st) < 0 ? LKD_HOST_ERR() : 0;
    struct fs_file *f = (o->ops == &fs_path_ops || o->ops == &fs_dir_ops) ? o->priv : NULL;
    if (f && f->host_path) return lstat(f->host_path, st) < 0 ? LKD_HOST_ERR() : 0;
    return -LX_EBADF;
}

/* ---- Regular files ----------------------------------------------------------------------------- */

static int64_t file_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    (void)t;
    struct fs_file *f = o->priv;
    if (f && f->own_offset && !off) {
        pthread_mutex_lock(&o->lock);
        int64_t r = fs_host_io(o->host_fd, gva, count, &f->pos, 0);
        if (r > 0) f->pos += r;
        pthread_mutex_unlock(&o->lock);
        return r;
    }
    return fs_host_io(o->host_fd, gva, count, off, 0);
}

static int64_t file_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    (void)t;
    struct fs_file *f = o->priv;
    int append = (fs_obj_getfl(o) & LX_O_APPEND) != 0;
    if (off && append) {
        /* Linux pwrite(2) BUGS: with O_APPEND the data is appended whatever the offset. The append is
         * done at the size seen now (not atomic against another appender, documented). */
        struct stat st;
        if (fstat(o->host_fd, &st) < 0) return LKD_HOST_ERR();
        int64_t at = st.st_size;
        return fs_host_io(o->host_fd, gva, count, &at, 1);
    }
    if (f && f->own_offset && !off) {
        pthread_mutex_lock(&o->lock);
        if (append) {
            struct stat st;
            if (fstat(o->host_fd, &st) == 0) f->pos = st.st_size;
        }
        int64_t r = fs_host_io(o->host_fd, gva, count, &f->pos, 1);
        if (r > 0) f->pos += r;
        pthread_mutex_unlock(&o->lock);
        return r;
    }
    return fs_host_io(o->host_fd, gva, count, off, 1);
}

static int host_whence(int lx_whence)
{
    switch (lx_whence) {
    case LX_SEEK_SET: return SEEK_SET;
    case LX_SEEK_CUR: return SEEK_CUR;
    case LX_SEEK_END: return SEEK_END;
    case LX_SEEK_DATA: return SEEK_DATA; /* Linux 3 = SEEK_DATA; Darwin SEEK_DATA is 4 */
    case LX_SEEK_HOLE: return SEEK_HOLE;
    default: return -1;
    }
}

static int64_t file_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    (void)t;
    int hw = host_whence(whence);
    if (hw < 0) return -LX_EINVAL;
    struct fs_file *f = o->priv;
    if (f && f->own_offset) {
        pthread_mutex_lock(&o->lock);
        int64_t base = 0, r;
        struct stat st;
        switch (whence) {
        case LX_SEEK_SET: base = 0; break;
        case LX_SEEK_CUR: base = f->pos; break;
        case LX_SEEK_END:
            if (fstat(o->host_fd, &st) < 0) { pthread_mutex_unlock(&o->lock); return LKD_HOST_ERR(); }
            base = st.st_size;
            break;
        default:
            /* SEEK_DATA/SEEK_HOLE without touching the host offset, which another description shares:
             * the whole file is data (generic_file_llseek_size semantics for files without holes). */
            if (fstat(o->host_fd, &st) < 0) { pthread_mutex_unlock(&o->lock); return LKD_HOST_ERR(); }
            if (off < 0 || off >= st.st_size) { pthread_mutex_unlock(&o->lock); return -LX_ENXIO; }
            f->pos = whence == LX_SEEK_DATA ? off : st.st_size;
            r = f->pos;
            pthread_mutex_unlock(&o->lock);
            return r;
        }
        if ((off > 0 && base > INT64_MAX - off) || base + off < 0) { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
        f->pos = base + off;
        r = f->pos;
        pthread_mutex_unlock(&o->lock);
        return r;
    }
    off_t r = lseek(o->host_fd, off, hw);
    return r < 0 ? LKD_HOST_ERR() : (int64_t)r;
}

static int file_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    (void)t;
    struct stat st;
    int rc = fs_obj_hoststat(o, &st);
    if (rc < 0) return rc;
    fs_stat_from_host(&st, out);
    return 0;
}

static int file_ftruncate(lkd_task *t, lkd_fdobj *o, int64_t len)
{
    (void)t;
    return ftruncate(o->host_fd, len) < 0 ? (int)LKD_HOST_ERR() : 0;
}

static int file_fsync(lkd_task *t, lkd_fdobj *o, int datasync)
{
    (void)t; (void)datasync;
    return fsync(o->host_fd) < 0 ? (int)LKD_HOST_ERR() : 0;
}

static int file_mmap_fd(lkd_fdobj *o) { return o->host_fd; }

/* readlink(/proc/<pid>/fd/N) of a path object: guest path, " (deleted)" once unlinked (d_path). */
int fs_describe_path(lkd_fdobj *o, char *buf, size_t cap)
{
    const char *g = o->gpath ? o->gpath : "";
    struct stat st;
    int deleted = 0;
    if (o->host_fd >= 0 && fstat(o->host_fd, &st) == 0 && st.st_nlink == 0 && !S_ISDIR(st.st_mode)) deleted = 1;
    int n = snprintf(buf, cap, "%s%s", g, deleted ? " (deleted)" : "");
    if (n < 0) return -LX_EINVAL;
    return (size_t)n >= cap ? (int)cap : n;
}

static int64_t file_ioctl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)cmd; (void)arg;
    return -LX_ENOTTY; /* FIONREAD on regular files is handled by the ioctl syscall itself */
}

const lkd_fd_ops fs_file_ops = {
    .name = "file", .read = file_read, .write = file_write, .lseek = file_lseek, .ioctl = file_ioctl,
    .fstat = file_fstat, .ftruncate = file_ftruncate, .fsync = file_fsync, .mmap_fd = file_mmap_fd,
    .describe = fs_describe_path, .release = file_release,
};

/* ---- Directories ------------------------------------------------------------------------------- */

static int64_t dir_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    (void)t;
    struct fs_file *f = o->priv;
    pthread_mutex_lock(&o->lock);
    int64_t np;
    if (whence == LX_SEEK_SET) np = off;
    else if (whence == LX_SEEK_CUR) np = (int64_t)f->dpos + off;
    else { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
    if (np < 0) { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
    f->dpos = (uint64_t)np;
    if (np == 0) dir_snapshot_free(f); /* rewinddir: a new listing sees later changes */
    pthread_mutex_unlock(&o->lock);
    return np;
}

static int dir_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct fs_file *f = o->priv;
    if (o->host_fd < 0 && f && (f->synthetic || (f->mnt && f->mnt->kind == FS_MNT_VIRT))) {
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, LX_AT_FDCWD, o->gpath ? o->gpath : "/", FS_R_FOLLOW | FS_R_GLOBAL, r);
        if (rc == 0) rc = fs_res_stat(t, r, out);
        free(r);
        return rc;
    }
    return file_fstat(t, o, out);
}

static int dir_fsync(lkd_task *t, lkd_fdobj *o, int datasync)
{
    (void)t; (void)datasync;
    if (o->host_fd < 0) return 0; /* virtual directory: nothing to write back (simple_sync_file) */
    return fsync(o->host_fd) < 0 ? (int)LKD_HOST_ERR() : 0;
}

static int dir_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "%s", o->gpath ? o->gpath : "/");
    return (n < 0) ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

const lkd_fd_ops fs_dir_ops = {
    .name = "dir", .lseek = dir_lseek, .fstat = dir_fstat, .getdents = fs_getdents, .fsync = dir_fsync,
    .describe = dir_describe, .release = file_release,
};

int fs_obj_is_dir(lkd_fdobj *o)
{
    if (o->ops == &fs_dir_ops) return 1;
    if (o->ops == &fs_path_ops) {
        struct fs_file *f = o->priv;
        struct stat st;
        if (f && !f->host_path) return f->synthetic;
        return fs_obj_hoststat(o, &st) == 0 && S_ISDIR(st.st_mode);
    }
    return 0;
}

int fs_dirobj_gpath(lkd_fdobj *o, char *out, size_t cap)
{
    if (!fs_obj_is_dir(o)) return -LX_ENOTDIR;
    if (!o->gpath) return -LX_ENOTDIR;
    int n = snprintf(out, cap, "%s", o->gpath);
    return (n < 0 || (size_t)n >= cap) ? -LX_ENAMETOOLONG : 0;
}

/* ---- O_PATH ------------------------------------------------------------------------------------ */

static int path_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct fs_file *f = o->priv;
    if (f && !f->host_path) {
        /* virtual object: stat it again through its guest path, without following a final link */
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, LX_AT_FDCWD, o->gpath ? o->gpath : "/", FS_R_NOMAGIC | FS_R_GLOBAL, r);
        if (rc == 0) rc = fs_res_stat(t, r, out);
        free(r);
        return rc;
    }
    return file_fstat(t, o, out);
}

const lkd_fd_ops fs_path_ops = {
    .name = "path", .fstat = path_fstat, .describe = dir_describe, .release = file_release,
};

/* ---- Streams: host ttys, character devices, FIFOs and sockets LKD did not create ---------------- */

/* Host descriptors shared with other host processes (terminal, launcher pipes): the host O_NONBLOCK
 * flag is the guest's, a blocking guest read first waits (interruptibly) for readiness, then reads. A
 * concurrent reader of the same host description may take the data between the two steps; the host
 * read then blocks uninterruptibly (documented gap). Data goes through a kernel buffer so that a
 * guest buffer fault never splits a host read. */
static int64_t stream_rw(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, int is_write)
{
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    size_t chunk = count > 65536 ? 65536 : (size_t)count;
    char small[4096];
    char *buf = chunk <= sizeof small ? small : malloc(chunk);
    if (!buf) return -LX_ENOMEM;
    int nonblock = (fs_obj_getfl(o) & LX_O_NONBLOCK) != 0;
    int64_t done = 0, ret = 0;
    while ((uint64_t)done < count) {
        size_t n = (size_t)(count - (uint64_t)done);
        if (n > chunk) n = chunk;
        if (is_write && lkd_user_read(gva + (uint64_t)done, buf, n) < 0) { ret = -LX_EFAULT; break; }
        if (!nonblock) {
            int w = lkd_block_wait_fd(t, o->host_fd, is_write ? POLLOUT : POLLIN, -1);
            if (w < 0) { ret = w; break; }
        }
        ssize_t r = is_write ? write(o->host_fd, buf, n) : read(o->host_fd, buf, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN && !nonblock) continue;
            ret = LKD_HOST_ERR();
            if (ret == -LX_EPIPE) fs_sigpipe(t);
            break;
        }
        if (!is_write && r > 0 && lkd_user_write(gva + (uint64_t)done, buf, (size_t)r) < 0) { ret = -LX_EFAULT; break; }
        done += r;
        if (!is_write) break;           /* a read returns what is available */
        if ((size_t)r < n && nonblock) break;
    }
    if (buf != small) free(buf);
    if (done > 0) return done;
    return ret;
}

static int64_t stream_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    return stream_rw(t, o, gva, count, 0);
}
static int64_t stream_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    return stream_rw(t, o, gva, count, 1);
}

static int64_t stream_ioctl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    if (cmd == LX_FIONREAD) {
        int avail = 0;
        if (ioctl(o->host_fd, FIONREAD, &avail) < 0) return LKD_HOST_ERR();
        int32_t v = avail;
        return lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
    }
    if (isatty(o->host_fd)) return fs_tty_ioctl(t, o->host_fd, cmd, arg);
    return -LX_ENOTTY;
}

static uint32_t stream_poll(lkd_fdobj *o, uint32_t want)
{
    struct pollfd p = { .fd = o->host_fd, .events = POLLIN | POLLOUT };
    if (poll(&p, 1, 0) < 0) return 0;
    uint32_t ev = 0;
    if (p.revents & POLLIN) ev |= LX_POLLIN | LX_POLLRDNORM;
    if (p.revents & POLLOUT) ev |= LX_POLLOUT | LX_POLLWRNORM;
    if (p.revents & POLLHUP) ev |= LX_POLLHUP;
    if (p.revents & POLLERR) ev |= LX_POLLERR;
    if (p.revents & POLLNVAL) ev |= LX_POLLNVAL;
    return ev & (want | LX_POLLHUP | LX_POLLERR | LX_POLLNVAL);
}

static int stream_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    struct stat st;
    int n;
    if (o->gpath) n = snprintf(buf, cap, "%s", o->gpath);
    else if (fstat(o->host_fd, &st) == 0 && S_ISFIFO(st.st_mode)) n = snprintf(buf, cap, "pipe:[%llu]", (unsigned long long)st.st_ino);
    else if (fstat(o->host_fd, &st) == 0 && S_ISSOCK(st.st_mode)) n = snprintf(buf, cap, "socket:[%llu]", (unsigned long long)st.st_ino);
    else n = snprintf(buf, cap, "/dev/tty");
    return (n < 0) ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

static int stream_fsync(lkd_task *t, lkd_fdobj *o, int datasync)
{
    (void)t; (void)o; (void)datasync;
    return -LX_EINVAL; /* ttys, pipes and sockets have no fsync (Linux: -EINVAL) */
}

const lkd_fd_ops fs_stream_ops = {
    .name = "stream", .read = stream_read, .write = stream_write, .ioctl = stream_ioctl, .poll = stream_poll,
    .fstat = file_fstat, .fsync = stream_fsync, .describe = stream_describe, .release = file_release,
};

/* ---- Construction ------------------------------------------------------------------------------ */

int fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *gpath, const char *host_path, lkd_fdobj **out)
{
    struct stat st;
    if (fstat(host_fd, &st) < 0) return LKD_HOST_ERR();
    const lkd_fd_ops *ops;
    lkd_fd_kind kind;
    if (S_ISREG(st.st_mode)) { ops = &fs_file_ops; kind = LKD_FD_FILE; }
    else if (S_ISDIR(st.st_mode)) { ops = &fs_dir_ops; kind = LKD_FD_DIR; }
    else if (S_ISFIFO(st.st_mode)) { ops = &fs_stream_ops; kind = LKD_FD_PIPE; }
    else if (S_ISSOCK(st.st_mode)) { ops = &fs_stream_ops; kind = LKD_FD_SOCKET; }
    else { ops = &fs_stream_ops; kind = LKD_FD_DEV; }
    /* never let a guest write raise SIGPIPE in the host runtime: EPIPE comes back and LKD sends the
     * guest SIGPIPE itself (stream_rw) */
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) fcntl(host_fd, F_SETNOSIGPIPE, 1);
    /* A file or directory that arrives without its guest path (SCM_RIGHTS from another host process,
     * request wine-3): find it again from the host path through the mount table, so that fchdir,
     * openat(dirfd, ...), getdents64 and /proc/self/fd/N behave as in the sender. */
    char hp[MAXPATHLEN], gp[LX_PATH_MAX];
    const struct fs_mount *m = NULL;
    if (!gpath && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) && fcntl(host_fd, F_GETPATH, hp) == 0 &&
        lkd_fs_host_to_guest(hp, gp, sizeof gp) == 0) {
        gpath = gp;
        if (!host_path) host_path = hp;
        m = fs_mount_find(gp);
    }
    if (S_ISDIR(st.st_mode)) lx_flags |= LX_O_DIRECTORY;
    struct fs_file *f = file_new(host_path, m);
    if (!f) return -LX_ENOMEM;
    lkd_fdobj *o = lkd_fdobj_new(kind, ops, host_fd, lx_flags, f);
    if (!o) { free(f->host_path); free(f); return -LX_ENOMEM; }
    if (gpath) o->gpath = strdup(gpath);
    o->ino = st.st_ino;
    o->dev = lkd_dev_to_linux(st.st_dev);
    *out = o;
    return 0;
}

int lkd_fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *guest_path, lkd_fdobj **out)
{
    if (host_fd < 0 || !out) return -LX_EBADF;
    return fs_wrap_host_fd(host_fd, lx_flags, guest_path, NULL, out);
}

int lkd_fs_open_host_file(const char *guest_path, int host_fd, lkd_fdobj **out)
{
    if (host_fd < 0 || !out) return -LX_EBADF;
    struct stat st;
    if (fstat(host_fd, &st) < 0) return LKD_HOST_ERR();
    if (!S_ISREG(st.st_mode)) return -LX_EACCES; /* execve: -EACCES for a non-regular file */
    int d = fcntl(host_fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0) return LKD_HOST_ERR();
    char hp[MAXPATHLEN];
    int rc = fs_wrap_host_fd(d, LX_O_RDONLY | LX_O_LARGEFILE, guest_path,
                             fcntl(d, F_GETPATH, hp) == 0 ? hp : NULL, out);
    if (rc < 0) close(d);
    return rc;
}

int lkd_fs_install_stdio(lkd_task *t)
{
    for (int i = 0; i < 3; i++) {
        int d = fcntl(i, F_DUPFD_CLOEXEC, 3);
        if (d < 0) return LKD_HOST_ERR();
        int h = fcntl(d, F_GETFL);
        uint32_t acc = h >= 0 ? (uint32_t)(h & O_ACCMODE) : (i ? LX_O_WRONLY : LX_O_RDONLY);
        lkd_fdobj *o;
        int rc = fs_wrap_host_fd(d, acc | LX_O_LARGEFILE, NULL, NULL, &o);
        if (rc < 0) { close(d); return rc; }
        rc = lkd_fd_install_at(t, i, o, 0);
        lkd_fdobj_put(o);
        if (rc < 0) return rc;
    }
    return 0;
}

int lkd_fs_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    if (!o || !buf || !cap) return -LX_EINVAL;
    if (o->ops && o->ops->describe) return o->ops->describe(o, buf, cap);
    int n = snprintf(buf, cap, "%s", o->gpath ? o->gpath : "anon_inode:[unknown]");
    return n < 0 ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

/* Open a resolved, existing host object (or one just created). */
int fs_open_host(lkd_task *t, struct fs_res *r, uint32_t fl, uint32_t mode, int created, lkd_fdobj **out)
{
    (void)mode;
    uint32_t acc = fl & LX_O_ACCMODE;
    if (fl & LX_O_PATH) {
        int hosted = r->mnt && r->mnt->kind == FS_MNT_HOST && !r->synthetic;
        struct fs_file *f = file_new(hosted ? r->host : NULL, r->mnt);
        if (!f) return -LX_ENOMEM;
        f->synthetic = !hosted && LX_S_ISDIR(fs_res_mode(r)); /* directory without a host object */
        lkd_fdobj *o = lkd_fdobj_new(LKD_FD_FILE, &fs_path_ops, -1, LX_O_PATH | (fl & LX_O_NOFOLLOW), f);
        if (!o) { free(f->host_path); free(f); return -LX_ENOMEM; }
        o->gpath = strdup(r->gpath);
        o->ino = r->synthetic ? 0 : (uint64_t)r->st.st_ino;
        o->dev = r->mnt ? r->mnt->lx_dev : 0;
        *out = o;
        return 0;
    }
    int hflags = 0;
    uint32_t untranslated = 0;
    int rc = lkd_open_flags_to_host(fl & ~(uint32_t)(LX_O_CREAT | LX_O_EXCL), &hflags, &untranslated);
    if (rc < 0) return rc;
    hflags |= O_NOFOLLOW | O_CLOEXEC;
    if (created) hflags &= ~O_TRUNC;
    int is_dir = S_ISDIR(r->st.st_mode);
    int is_fifo = S_ISFIFO(r->st.st_mode);
    /* A directory is opened for search only: macOS opens a directory for reading in about 7 us and for search in
     * 1.6 us, and Wine opens directories for every name it does not find. Listing it reopens it for reading
     * (snapshot in dir.c); stat, statfs, xattrs, chmod, utimes, fsync, flock and openat work on this descriptor. */
    if (is_dir) hflags = O_SEARCH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int hfd;
    if (is_fifo) {
        /* Linux fifo_open blocks a writer until a reader exists. Open non-blocking and retry
         * interruptibly; a blocking reader is not held back waiting for a writer (documented gap). */
        int want_nb = (fl & LX_O_NONBLOCK) != 0;
        for (;;) {
            hfd = open(r->host, hflags | O_NONBLOCK);
            if (hfd >= 0 || errno != ENXIO || want_nb || acc == LX_O_RDONLY) break;
            int w = lkd_block_sleep(t, lkd_host_mono_ns() + 10000000);
            if (w < 0) return w;
        }
        if (hfd >= 0 && !want_nb) fcntl(hfd, F_SETFL, fcntl(hfd, F_GETFL) & ~O_NONBLOCK);
    } else {
        do { hfd = open(r->host, hflags, 0); } while (hfd < 0 && errno == EINTR);
    }
    if (hfd < 0) return LKD_HOST_ERR();
    return fs_open_host_fd(t, r, fl, hfd, out);
}

int fs_open_host_fd(lkd_task *t, struct fs_res *r, uint32_t fl, int hfd, lkd_fdobj **out)
{
    (void)t;
    uint32_t keep = (fl & (LX_O_ACCMODE | LX_O_DIRECT | LX_O_NOATIME | LX_O_APPEND | LX_O_NONBLOCK | LX_O_DSYNC |
                           LX_O_SYNC | LX_O_ASYNC)) | LX_O_LARGEFILE;
    int rc;
    int is_dir = S_ISDIR(r->st.st_mode);
    if ((fl & LX_O_DIRECT) && !is_dir) fcntl(hfd, F_NOCACHE, 1);
    struct stat st;
    if (fstat(hfd, &st) < 0) { int e = (int)LKD_HOST_ERR(); close(hfd); return e; }
    if (is_dir) {
        struct fs_file *f = file_new(r->host, r->mnt);
        if (!f) { close(hfd); return -LX_ENOMEM; }
        lkd_fdobj *o = lkd_fdobj_new(LKD_FD_DIR, &fs_dir_ops, hfd, LX_O_RDONLY | (keep & ~LX_O_ACCMODE) | LX_O_DIRECTORY, f);
        if (!o) { close(hfd); free(f->host_path); free(f); return -LX_ENOMEM; }
        o->gpath = strdup(r->gpath);
        o->ino = st.st_ino;
        o->dev = lkd_dev_to_linux(st.st_dev);
        *out = o;
        return 0;
    }
    lkd_fdobj *o;
    rc = fs_wrap_host_fd(hfd, keep, r->gpath, r->host, &o);
    if (rc < 0) { close(hfd); return rc; }
    struct fs_file *f = o->priv;
    if (f && r->mnt) { f->ro_mount = r->mnt->ro; f->mount_id = r->mnt->id; f->lx_dev = r->mnt->lx_dev; f->mnt = r->mnt; }
    *out = o;
    return 0;
}

int fs_open_vdir(lkd_task *t, const struct fs_res *r, uint32_t fl, lkd_fdobj **out)
{
    (void)t;
    struct fs_file *f = file_new(NULL, r->mnt);
    if (!f) return -LX_ENOMEM;
    f->synthetic = r->synthetic || (r->mnt && r->mnt->kind == FS_MNT_VIRT);
    const lkd_fd_ops *ops = (fl & LX_O_PATH) ? &fs_path_ops : &fs_dir_ops;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_DIR, ops, -1, (fl & LX_O_PATH) ? LX_O_PATH : (LX_O_RDONLY | LX_O_DIRECTORY | LX_O_LARGEFILE), f);
    if (!o) { free(f); return -LX_ENOMEM; }
    o->gpath = strdup(r->gpath);
    o->dev = r->mnt ? r->mnt->lx_dev : 0;
    o->ino = r->va.ino;
    *out = o;
    return 0;
}

int lkd_fs_open_guest_file(lkd_task *t, const char *guest_path, lkd_fdobj **out)
{
    return lkd_vfs_open(t, LX_AT_FDCWD, guest_path, LX_O_RDONLY | LX_O_CLOEXEC, 0, out);
}

/* ---- Reopen (/proc/<pid>/fd/N) ----------------------------------------------------------------- */

/* A new, independent open file description of the same file (fs/proc/base.c proc_fd_link + do_open:
 * own offset and flags). Host files are opened again by their current host path when it still names
 * the same inode; a deleted file gets a dup of the host fd with a private offset. */
int lkd_fdobj_reopen(lkd_task *t, lkd_fdobj *o, uint32_t fl, lkd_fdobj **out)
{
    if (!o || !out) return -LX_EINVAL;
    if (o->ops == &fs_memfd_ops) {
        extern int fs_memfd_reopen(lkd_fdobj *o, uint32_t fl, lkd_fdobj **out);
        return fs_memfd_reopen(o, fl, out);
    }
    if (o->ops == &fs_pipe_ops || o->ops == &fs_stream_ops) {
        if (o->kind == LKD_FD_SOCKET) return -LX_ENXIO;
        uint32_t acc = fl & LX_O_ACCMODE;
        if (o->kind == LKD_FD_PIPE && acc != fs_obj_access(o)) return -LX_ENXIO; /* other end not held */
        int d = fcntl(o->host_fd, F_DUPFD_CLOEXEC, 0);
        if (d < 0) return LKD_HOST_ERR();
        lkd_fdobj *n = lkd_fdobj_new(o->kind, o->ops, d, (fl & (LX_O_ACCMODE | LX_O_NONBLOCK)), NULL);
        if (!n) { close(d); return -LX_ENOMEM; }
        if (o->ops == &fs_stream_ops) n->priv = calloc(1, sizeof(struct fs_file));
        n->ino = o->ino;
        n->dev = o->dev;
        if (o->gpath) n->gpath = strdup(o->gpath);
        *out = n;
        return 0;
    }
    if (o->ops != &fs_file_ops && o->ops != &fs_dir_ops && o->ops != &fs_path_ops) return -LX_ENXIO;
    struct fs_file *f = o->priv;
    /* do_open after the magic link: O_DIRECTORY needs a directory, a directory refuses write access */
    int odir = fs_obj_is_dir(o);
    if ((fl & LX_O_DIRECTORY) && !odir) return -LX_ENOTDIR;
    if (odir && !(fl & LX_O_PATH) && ((fl & LX_O_ACCMODE) != LX_O_RDONLY || (fl & LX_O_CREAT))) return -LX_EISDIR;
    if (!(fl & LX_O_PATH) && (fl & LX_O_ACCMODE) != LX_O_RDONLY && f && f->ro_mount) return -LX_EROFS;
    /* Through the guest path again when it still names the same object (handles O_PATH, dirs). */
    struct stat cur;
    int have_cur = fs_obj_hoststat(o, &cur) == 0;
    if (o->gpath) {
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, LX_AT_FDCWD, o->gpath, (o->ops == &fs_path_ops && (atomic_load(&o->status_flags) & LX_O_NOFOLLOW)) ? FS_R_NOMAGIC | FS_R_GLOBAL : FS_R_FOLLOW | FS_R_NOMAGIC | FS_R_GLOBAL, r);
        if (rc == 0 && r->exists && !r->synthetic && r->mnt->kind == FS_MNT_HOST && have_cur &&
            r->st.st_dev == cur.st_dev && r->st.st_ino == cur.st_ino) {
            if (S_ISDIR(r->st.st_mode) && (fl & LX_O_ACCMODE) != LX_O_RDONLY && !(fl & LX_O_PATH)) rc = -LX_EISDIR;
            else rc = fs_open_host(t, r, fl & ~(uint32_t)(LX_O_CREAT | LX_O_EXCL | LX_O_TRUNC), 0, 0, out);
            if (rc == 0 && (fl & LX_O_TRUNC) && (fl & LX_O_ACCMODE) != LX_O_RDONLY && (*out)->host_fd >= 0)
                ftruncate((*out)->host_fd, 0);
            free(r);
            return rc;
        }
        if (rc == 0 && r->exists && (r->synthetic || r->mnt->kind == FS_MNT_VIRT) && !have_cur) {
            rc = (r->synthetic || LX_S_ISDIR(fs_res_mode(r))) ? fs_open_vdir(t, r, fl, out)
                                                               : r->mnt->ops->open(t, r->mnt->priv, r->rel, fl, 0, out);
            free(r);
            return rc;
        }
        free(r);
    }
    /* Deleted or renamed away: dup with a private offset. The access mode cannot exceed the original
     * host description's (documented gap; Linux would check the inode permission instead). */
    if (o->host_fd < 0) return -LX_ENOENT;
    uint32_t want = fl & LX_O_ACCMODE, have = fs_obj_access(o);
    if (want != have && have != LX_O_RDWR) return -LX_EACCES;
    int d = fcntl(o->host_fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0) return LKD_HOST_ERR();
    struct fs_file *nf = file_new(f ? f->host_path : NULL, f ? f->mnt : NULL);
    if (!nf) { close(d); return -LX_ENOMEM; }
    nf->own_offset = 1;
    lkd_fdobj *n = lkd_fdobj_new(o->kind, o->ops, d, (fl & (LX_O_ACCMODE | LX_O_DIRECT | LX_O_NOATIME)) | LX_O_LARGEFILE, nf);
    if (!n) { close(d); free(nf->host_path); free(nf); return -LX_ENOMEM; }
    n->ino = o->ino;
    n->dev = o->dev;
    if (o->gpath) n->gpath = strdup(o->gpath);
    *out = n;
    return 0;
}

int lkd_fs_mmap_check(lkd_fdobj *o, int prot, uint32_t flags)
{
    if (!o) return -LX_EBADF;
    if (fs_obj_is_path(o)) return -LX_EBADF;
    if (!o->ops || !o->ops->mmap_fd || o->ops->mmap_fd(o) < 0) return -LX_ENODEV;
    uint32_t acc = fs_obj_access(o);
    if (acc == LX_O_WRONLY) return -LX_EACCES;
    int shared = (flags & LX_MAP_TYPE) == LX_MAP_SHARED || (flags & LX_MAP_TYPE) == LX_MAP_SHARED_VALIDATE;
    if (shared && (prot & LX_PROT_WRITE)) {
        if (acc != LX_O_RDWR) return -LX_EACCES;
        if (fs_obj_getfl(o) & LX_O_APPEND) return -LX_EACCES;
        int seals = fs_memfd_seals(o);
        if (seals > 0 && (seals & (LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE))) return -LX_EPERM;
    }
    return 0;
}

int lkd_fs_fdinfo_basic(lkd_task *t, lkd_fdobj *o, int64_t *pos, uint32_t *flags, int *mnt_id)
{
    if (!o) return -LX_EBADF;
    int64_t p = 0;
    if (o->ops && o->ops->lseek && !fs_obj_is_path(o)) {
        p = o->ops->lseek(t, o, 0, LX_SEEK_CUR);
        if (p < 0) p = 0;
    }
    if (pos) *pos = p;
    if (flags) *flags = fs_obj_is_path(o) ? (LX_O_PATH | LX_O_LARGEFILE) : fs_obj_getfl(o);
    if (mnt_id) {
        struct fs_file *f = (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_path_ops) ? o->priv : NULL;
        *mnt_id = f ? f->mount_id : 0;
    }
    return 0;
}

/* fstat semantics for any object. */
int fs_obj_stat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    if (o->ops && o->ops->fstat) return o->ops->fstat(t, o, out);
    return -LX_EBADF;
}
