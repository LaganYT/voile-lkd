/* sys_open.c: open, openat, openat2, creat, close, close_range and lkd_vfs_open.
 * Linux references: fs/open.c (build_open_flags, do_sys_openat2), fs/namei.c (open_last_lookups,
 * do_open, may_open), fs/file.c (close_range).
 */
#include <fcntl.h>
#include <unistd.h>

#include "fs_priv.h"

#define LX_VALID_OPEN_FLAGS (LX_O_ACCMODE | LX_O_CREAT | LX_O_EXCL | LX_O_NOCTTY | LX_O_TRUNC | LX_O_APPEND | \
    LX_O_NONBLOCK | LX_O_DSYNC | LX_O_ASYNC | LX_O_DIRECT | LX_O_LARGEFILE | LX_O_DIRECTORY | LX_O_NOFOLLOW | \
    LX_O_NOATIME | LX_O_CLOEXEC | LX_O_SYNC | LX_O_PATH | LX_O_TMPFILE)
#define LX_O_PATH_FLAGS (LX_O_DIRECTORY | LX_O_NOFOLLOW | LX_O_PATH | LX_O_CLOEXEC)

static int open_resolved(lkd_task *t, struct fs_res *r, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{
    uint32_t acc = fl & LX_O_ACCMODE;
    int wants_write = acc == LX_O_WRONLY || acc == LX_O_RDWR || (fl & LX_O_TRUNC);
    if (r->magic) {
        if (!r->mnt->ops->open) return -LX_ENXIO;
        return r->mnt->ops->open(t, r->mnt->priv, r->rel, fl, mode, out);
    }
    if (!r->exists) {
        /* O_CREAT of a missing final component (open_last_lookups / lookup_open). */
        if (r->trailing_slash) return -LX_EISDIR;
        int w = fs_res_writable_host(r);
        if (w == -LX_EACCES && r->mnt && r->mnt->kind == FS_MNT_VIRT) return -LX_EACCES;
        if (w < 0) return w;
        int hflags = 0;
        uint32_t un;
        int rc = lkd_open_flags_to_host(fl & ~(uint32_t)LX_O_TRUNC, &hflags, &un);
        if (rc < 0) return rc;
        hflags |= O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC;
        mode_t m = (mode_t)(mode & 07777 & ~lkd_vfs_umask(t));
        int hfd = open(r->host, hflags, m);
        if (hfd < 0) return LKD_HOST_ERR();
        if (fstat(hfd, &r->st) < 0) { int e = (int)LKD_HOST_ERR(); close(hfd); return e; }
        r->exists = 1;
        /* keep the descriptor of the creation: a file created with mode 0444 is still writable
         * through it (Linux may_open is skipped for a newly created file) */
        return fs_open_host_fd(t, r, fl, hfd, out);
    }
    if ((fl & (LX_O_CREAT | LX_O_EXCL)) == (LX_O_CREAT | LX_O_EXCL)) return -LX_EEXIST;
    uint32_t type = fs_res_mode(r) & LX_S_IFMT;
    if (type == LX_S_IFLNK) {
        if (fl & LX_O_PATH) return fs_open_host(t, r, fl, mode, 0, out);
        return -LX_ELOOP;                               /* O_NOFOLLOW on a symlink */
    }
    if ((fl & LX_O_DIRECTORY) && type != LX_S_IFDIR) return -LX_ENOTDIR;
    if (type == LX_S_IFDIR && !(fl & LX_O_PATH) && ((fl & LX_O_CREAT) || acc != LX_O_RDONLY)) return -LX_EISDIR;
    if (r->synthetic || (r->mnt && r->mnt->kind == FS_MNT_VIRT && type == LX_S_IFDIR))
        return fs_open_vdir(t, r, fl, out);
    if (r->mnt->kind == FS_MNT_VIRT) {
        if (fl & LX_O_PATH) return fs_open_host(t, r, fl, mode, 0, out);
        if (!r->mnt->ops->open) return -LX_ENXIO;
        return r->mnt->ops->open(t, r->mnt->priv, r->rel, fl, mode, out);
    }
    if (!(fl & LX_O_PATH) && wants_write && r->mnt->ro && type != LX_S_IFCHR && type != LX_S_IFIFO &&
        type != LX_S_IFSOCK)
        return -LX_EROFS;
    return fs_open_host(t, r, fl, mode, 0, out);
}

int lkd_vfs_open(lkd_task *t, int dirfd, const char *gpath, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{
    if (fl & LX_O_PATH) fl &= LX_O_PATH_FLAGS;
    if ((fl & LX_O_TMPFILE) == LX_O_TMPFILE) {
        uint32_t acc = fl & LX_O_ACCMODE;
        if (acc != LX_O_WRONLY && acc != LX_O_RDWR) return -LX_EINVAL;
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, dirfd, gpath, FS_R_FOLLOW, r);
        if (rc == 0 && !LX_S_ISDIR(fs_res_mode(r))) rc = -LX_ENOTDIR;
        free(r);
        /* Unnamed temporary files need file system support (Linux: -EOPNOTSUPP when the fs lacks
         * ->tmpfile). Host directories cannot create one; glibc tmpfile() then falls back. */
        return rc < 0 ? rc : -LX_EOPNOTSUPP;
    }
    if ((fl & LX_O_CREAT) && (fl & LX_O_DIRECTORY)) return -LX_EINVAL;   /* Linux >= 6.4 */
    unsigned rf = 0;
    if (!(fl & LX_O_NOFOLLOW) && !((fl & LX_O_CREAT) && (fl & LX_O_EXCL))) rf |= FS_R_FOLLOW;
    if (fl & LX_O_CREAT) rf |= FS_R_CREATE;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, gpath, rf, r);
    if (rc == 0) rc = open_resolved(t, r, fl, mode, out);
    free(r);
    return rc;
}

/* LKD_FDREC_FS (fd.h): a synthetic or virtual directory, or an O_PATH descriptor, that another host process sent.
 * Its global guest path is opened again with the sender's flags, as a request of the receiver (net/core/scm.c
 * would hand over the same description; here offsets and flags changed later are not shared, fd.h). */
static int fs_rec_import(lkd_task *t, uint32_t fl, const void *data, size_t len, lkd_fdobj **out)
{
    const char *gp = data;
    if (!len || memchr(gp, 0, len) != gp + len - 1 || gp[0] != '/') return -LX_EINVAL;
    uint32_t keep = (fl & LX_O_PATH) ? (fl & (LX_O_PATH | LX_O_NOFOLLOW))
                                     : (fl & (LX_O_ACCMODE | LX_O_NONBLOCK | LX_O_APPEND | LX_O_DIRECTORY | LX_O_NOATIME));
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    unsigned rf = FS_R_GLOBAL | FS_R_NOMAGIC | ((keep & LX_O_NOFOLLOW) ? 0 : FS_R_FOLLOW);
    int rc = fs_resolve(t, LX_AT_FDCWD, gp, rf, r);
    if (rc == 0 && !r->exists) rc = -LX_ENOENT;
    if (rc == 0) rc = open_resolved(t, r, keep, 0, out);
    free(r);
    return rc;
}

static int64_t do_open(lkd_task *t, int dirfd, uint64_t path_gva, uint32_t fl, uint32_t mode)
{
    char path[LX_PATH_MAX];
    int n = fs_get_path(path_gva, path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    lkd_fdobj *o;
    int rc = lkd_vfs_open(t, dirfd, path, fl, mode, &o);
    if (rc < 0) return rc;
    int fd = lkd_fd_alloc(t, o, 0, (fl & LX_O_CLOEXEC) != 0);
    lkd_fdobj_put(o);
    return fd;
}

/* open/openat ignore unknown flag bits (historical, fs/open.c build_open_flags masks them). */
LKD_SYSCALL(open) { return do_open(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1) & LX_VALID_OPEN_FLAGS, LKD_ARGUI(2)); }
LKD_SYSCALL(openat) { return do_open(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2) & LX_VALID_OPEN_FLAGS, LKD_ARGUI(3)); }
LKD_SYSCALL(creat) { return do_open(t, LX_AT_FDCWD, LKD_ARGP(0), LX_O_CREAT | LX_O_WRONLY | LX_O_TRUNC, LKD_ARGUI(1)); }

/* openat2(2): strict validation (include/uapi/linux/openat2.h). RESOLVE_* restrictions are applied
 * after resolution on the canonical path (see docs/status/fs.md for the exact coverage). */
#define LX_RESOLVE_NO_XDEV 0x01
#define LX_RESOLVE_NO_MAGICLINKS 0x02
#define LX_RESOLVE_NO_SYMLINKS 0x04
#define LX_RESOLVE_BENEATH 0x08
#define LX_RESOLVE_IN_ROOT 0x10
#define LX_RESOLVE_CACHED 0x20

LKD_SYSCALL(openat2)
{
    uint64_t size = LKD_ARGU(3);
    struct { uint64_t flags, mode, resolve; } how;
    if (size < 24) return -LX_EINVAL;
    if (size > 4096) return -LX_E2BIG;
    if (lkd_user_read(LKD_ARGP(2), &how, sizeof how) < 0) return -LX_EFAULT;
    for (uint64_t off = 24; off < size; off++) {
        uint8_t b;
        if (lkd_user_read(LKD_ARGP(2) + off, &b, 1) < 0) return -LX_EFAULT;
        if (b) return -LX_E2BIG;
    }
    if (how.flags & ~(uint64_t)LX_VALID_OPEN_FLAGS) return -LX_EINVAL;
    if (how.resolve & ~(uint64_t)0x3f) return -LX_EINVAL;
    if ((how.resolve & LX_RESOLVE_BENEATH) && (how.resolve & LX_RESOLVE_IN_ROOT)) return -LX_EINVAL;
    if (how.mode & ~(uint64_t)07777) return -LX_EINVAL;
    if (!(how.flags & LX_O_CREAT) && (how.flags & LX_O_TMPFILE) != LX_O_TMPFILE && how.mode) return -LX_EINVAL;
    if ((how.flags & LX_O_PATH) && (how.flags & ~(uint64_t)LX_O_PATH_FLAGS)) return -LX_EINVAL;
    if (how.resolve & LX_RESOLVE_CACHED) {
        /* Only cached lookups allowed: LKD cannot promise a lookup without host I/O. -EAGAIN is the
         * documented answer when the cache cannot serve it; callers retry without the flag. */
        return -LX_EAGAIN;
    }
    char path[LX_PATH_MAX];
    int n = fs_get_path(LKD_ARGP(1), path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    int dirfd = LKD_ARGI(0);
    struct fs_restrict rs = { how.resolve };
    lkd_fdobj *o;
    if (how.resolve) fs_restrict_set(&rs);
    int rc = lkd_vfs_open(t, dirfd, path, (uint32_t)how.flags, (uint32_t)how.mode, &o);
    fs_restrict_set(NULL);
    if (rc < 0) return rc;
    int fd = lkd_fd_alloc(t, o, 0, (how.flags & LX_O_CLOEXEC) != 0);
    lkd_fdobj_put(o);
    return fd;
}

LKD_SYSCALL(close) { return lkd_fd_close(t, LKD_ARGI(0)); }

/* close_range(2), fs/file.c: flags CLOSE_RANGE_UNSHARE (2) and CLOSE_RANGE_CLOEXEC (4). UNSHARE gives
 * the calling thread a private copy of a shared table first. */
LKD_SYSCALL(close_range)
{
    unsigned first = LKD_ARGUI(0), last = LKD_ARGUI(1), flags = LKD_ARGUI(2);
    if (flags & ~6u) return -LX_EINVAL;
    if (last < first) return -LX_EINVAL;
    if ((flags & 2u) && t->fdt) {
        lkd_fdtable *copy = lkd_fdtable_clone(t->fdt, 0);
        if (!copy) return -LX_ENOMEM;
        lkd_fdtable *old = t->fdt;
        t->fdt = copy;
        lkd_fdtable_put(old);
    }
    return fs_fdtable_close_range(t, first, last, (flags & 4u) ? 1 : 0);
}

void fs_register_open(void)
{
    LKD_REGISTER(open);
    LKD_REGISTER(openat);
    LKD_REGISTER(openat2);
    LKD_REGISTER(creat);
    LKD_REGISTER(close);
    LKD_REGISTER(close_range);
    lkd_fdrec_register(LKD_FDREC_FS, fs_rec_import);
}
