/* sys_stat.c: stat family, statx, statfs, ustat, access, readlink, getcwd, chdir, fchdir, chroot.
 * Linux references: fs/stat.c (vfs_fstatat, do_statx, cp_new_stat), fs/statfs.c, fs/open.c
 * (do_faccessat, ksys_chdir, ksys_chroot), fs/d_path.c (getcwd).
 */
#include <fcntl.h>
#include <sys/mount.h>
#include <unistd.h>

#include "fs_priv.h"

#define LX_STATX__RESERVED 0x80000000u
#define LX_ST_RDONLY 0x0001
#define LX_ST_NOSUID 0x0002
#define LX_ST_NODEV 0x0004
#define LX_ST_NOEXEC 0x0008
#define LX_ST_RELATIME 0x1000

/* Stat a final magic link (follow semantics): ask the mount for the object and fstat it. */
static int stat_magic(lkd_task *t, struct fs_res *r, struct lx_stat *out, lkd_fdobj **keep)
{
    if (!r->mnt->ops->open) return -LX_ENXIO;
    lkd_fdobj *o = NULL;
    int rc = r->mnt->ops->open(t, r->mnt->priv, r->rel, LX_O_PATH, 0, &o);
    if (rc < 0) return rc;
    rc = fs_obj_stat(t, o, out);
    if (keep) *keep = o; else lkd_fdobj_put(o);
    return rc;
}

static int stat_fd(lkd_task *t, int fd, struct lx_stat *out)
{
    if (fd == LX_AT_FDCWD) {
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, LX_AT_FDCWD, ".", FS_R_FOLLOW, r);
        if (rc == 0) rc = fs_res_stat(t, r, out);
        free(r);
        return rc;
    }
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int rc = fs_obj_stat(t, o, out);
    lkd_fdobj_put(o);
    return rc;
}

int lkd_vfs_stat(lkd_task *t, int dirfd, const char *gpath, int flags, struct lx_stat *out)
{
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH | LX_AT_NO_AUTOMOUNT)) return -LX_EINVAL;
    if (!gpath || !gpath[0]) {
        if (!(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
        return stat_fd(t, dirfd, out);
    }
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, gpath, (flags & LX_AT_SYMLINK_NOFOLLOW) ? 0 : FS_R_FOLLOW, r);
    if (rc == 0) rc = r->magic ? stat_magic(t, r, out, NULL) : fs_res_stat(t, r, out);
    free(r);
    return rc;
}

static int64_t do_stat(lkd_task *t, int dirfd, uint64_t path_gva, uint64_t buf, int flags)
{
    char path[LX_PATH_MAX];
    struct lx_stat st;
    int rc;
    if (path_gva == 0 && (flags & LX_AT_EMPTY_PATH)) {   /* NULL path with AT_EMPTY_PATH (Linux >= 6.11) */
        if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH | LX_AT_NO_AUTOMOUNT)) return -LX_EINVAL;
        rc = stat_fd(t, dirfd, &st);
    } else {
        int n = fs_get_path(path_gva, path);
        if (n < 0) return n;
        rc = lkd_vfs_stat(t, dirfd, path, flags, &st);
    }
    if (rc < 0) return rc;
    return lkd_user_write(buf, &st, sizeof st) < 0 ? -LX_EFAULT : 0;
}

LKD_SYSCALL(stat) { return do_stat(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1), 0); }
LKD_SYSCALL(lstat) { return do_stat(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1), LX_AT_SYMLINK_NOFOLLOW); }
LKD_SYSCALL(newfstatat) { return do_stat(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGI(3)); }
LKD_SYSCALL(fstat)
{
    struct lx_stat st;
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int rc = fs_obj_stat(t, o, &st);
    lkd_fdobj_put(o);
    if (rc < 0) return rc;
    return lkd_user_write(LKD_ARGP(1), &st, sizeof st) < 0 ? -LX_EFAULT : 0;
}

/* statx(2): host objects give the birth time (STATX_BTIME) and the mount id when asked. */
LKD_SYSCALL(statx)
{
    int dirfd = LKD_ARGI(0), flags = LKD_ARGI(2);
    uint32_t mask = LKD_ARGUI(3);
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_NO_AUTOMOUNT | LX_AT_EMPTY_PATH | LX_AT_STATX_SYNC_TYPE)) return -LX_EINVAL;
    if ((flags & LX_AT_STATX_SYNC_TYPE) == LX_AT_STATX_SYNC_TYPE) return -LX_EINVAL;
    if (mask & LX_STATX__RESERVED) return -LX_EINVAL;
    struct lx_statx x;
    struct stat hs;
    int have_host = 0, mnt_id = 0, rc = 0;
    struct lx_stat ls;
    char path[LX_PATH_MAX];
    path[0] = 0;
    if (LKD_ARGP(1)) {
        int n = fs_get_path(LKD_ARGP(1), path);
        if (n < 0) return n;
    } else if (!(flags & LX_AT_EMPTY_PATH)) {
        return -LX_EFAULT;
    }
    if (!path[0]) {
        if (!(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
        if (dirfd == LX_AT_FDCWD) { snprintf(path, sizeof path, "."); }
        else {
            lkd_fdobj *o = lkd_fd_get(t, dirfd, NULL);
            if (!o) return -LX_EBADF;
            if (o->host_fd >= 0 && (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_stream_ops) &&
                fstat(o->host_fd, &hs) == 0) {
                have_host = 1;
                struct fs_file *f = o->priv;
                mnt_id = f ? f->mount_id : 0;
            } else if (o->ops == &fs_path_ops && fs_obj_hoststat(o, &hs) == 0) {
                have_host = 1;
                mnt_id = ((struct fs_file *)o->priv)->mount_id;
            } else {
                rc = fs_obj_stat(t, o, &ls);
            }
            lkd_fdobj_put(o);
            if (rc < 0) return rc;
            goto fill;
        }
    }
    {
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        rc = fs_resolve(t, dirfd, path, (flags & LX_AT_SYMLINK_NOFOLLOW) ? 0 : FS_R_FOLLOW, r);
        if (rc == 0) {
            if (r->magic) rc = stat_magic(t, r, &ls, NULL);
            else if (!r->synthetic && r->mnt->kind == FS_MNT_HOST) { hs = r->st; have_host = 1; mnt_id = r->mnt->id; }
            else { rc = fs_res_stat(t, r, &ls); mnt_id = r->mnt ? r->mnt->id : 0; }
        }
        free(r);
        if (rc < 0) return rc;
    }
fill:
    if (have_host) fs_statx_from_host(&hs, mask, &x);
    else fs_statx_from_lxstat(&ls, &x);
    if (mask & LX_STATX_MNT_ID) { x.stx_mnt_id = (uint64_t)mnt_id; x.stx_mask |= LX_STATX_MNT_ID; }
    return lkd_user_write(LKD_ARGP(4), &x, sizeof x) < 0 ? -LX_EFAULT : 0;
}

/* ---- statfs ------------------------------------------------------------------------------------ */

static void virt_statfs(const struct fs_mount *m, struct lx_statfs *s)
{
    memset(s, 0, sizeof *s);
    if (m && strcmp(m->fstype, "proc") == 0) { s->f_type = 0x9fa0; s->f_flags = LX_ST_NOSUID | LX_ST_NODEV | LX_ST_NOEXEC | LX_ST_RELATIME; }
    else if (m && strcmp(m->fstype, "sysfs") == 0) { s->f_type = 0x62656572; s->f_flags = LX_ST_NOSUID | LX_ST_NODEV | LX_ST_NOEXEC | LX_ST_RELATIME; }
    else if (m && (strcmp(m->fstype, "devtmpfs") == 0 || strcmp(m->fstype, "tmpfs") == 0)) { s->f_type = 0x01021994; s->f_flags = LX_ST_NOSUID | LX_ST_RELATIME; }
    s->f_bsize = 4096;
    s->f_frsize = 4096;
    s->f_namelen = 255;
    if (m) { s->f_fsid[0] = (int32_t)(m->lx_dev & 0xffffffff); }
}

static int host_statfs(const char *host, const struct fs_mount *m, struct lx_statfs *out)
{
    struct statfs hs;
    if (statfs(host, &hs) < 0) return (int)LKD_HOST_ERR();
    lkd_statfs_from_host(&hs, out);
    if (m && m->ro) out->f_flags |= LX_ST_RDONLY;
    return 0;
}

LKD_SYSCALL(statfs)
{
    char path[LX_PATH_MAX];
    int n = fs_get_path(LKD_ARGP(0), path);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    struct lx_statfs s;
    int rc = fs_resolve(t, LX_AT_FDCWD, path, FS_R_FOLLOW | FS_R_NOMAGIC, r);
    if (rc == 0) {
        if (!r->synthetic && r->mnt->kind == FS_MNT_HOST) rc = host_statfs(r->host, r->mnt, &s);
        else virt_statfs(r->mnt, &s);
    }
    free(r);
    if (rc < 0) return rc;
    return lkd_user_write(LKD_ARGP(1), &s, sizeof s) < 0 ? -LX_EFAULT : 0;
}

LKD_SYSCALL(fstatfs)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    struct lx_statfs s;
    int rc = 0;
    struct fs_file *f = (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_path_ops) ? o->priv : NULL;
    if (o->ops == &fs_memfd_ops) { virt_statfs(NULL, &s); s.f_type = 0x01021994; }
    else if (o->ops == &fs_pipe_ops) { virt_statfs(NULL, &s); s.f_type = 0x50495045; }
    else if (f && f->host_path && f->mnt && f->mnt->kind == FS_MNT_HOST) {
        struct statfs hs;
        if ((o->host_fd >= 0 ? fstatfs(o->host_fd, &hs) : statfs(f->host_path, &hs)) < 0) rc = (int)LKD_HOST_ERR();
        else { lkd_statfs_from_host(&hs, &s); if (f->ro_mount) s.f_flags |= LX_ST_RDONLY; }
    } else if (f && f->mnt) virt_statfs(f->mnt, &s);
    else if (o->host_fd >= 0) {
        struct statfs hs;
        if (fstatfs(o->host_fd, &hs) < 0) rc = (int)LKD_HOST_ERR(); else lkd_statfs_from_host(&hs, &s);
    } else virt_statfs(NULL, &s);  /* anon inode objects of other modules */
    lkd_fdobj_put(o);
    if (rc < 0) return rc;
    return lkd_user_write(LKD_ARGP(1), &s, sizeof s) < 0 ? -LX_EFAULT : 0;
}

/* ustat(2): the file system whose device is dev (fs/statfs.c: -EINVAL when none). */
LKD_SYSCALL(ustat)
{
    uint64_t dev = LKD_ARGU(0) & 0xffffffffu;
    int n = fs_mount_count();
    for (int i = 0; i < n; i++) {
        const struct fs_mount *m = fs_mount_at(i);
        if (m->kind != FS_MNT_HOST || m->lx_dev != dev) continue;
        struct statfs hs;
        if (statfs(m->host, &hs) < 0) return LKD_HOST_ERR();
        struct { int32_t f_tfree; int32_t pad; uint64_t f_tinode; char f_fname[6]; char f_fpack[6]; int32_t pad2; } u;
        memset(&u, 0, sizeof u);
        u.f_tfree = (int32_t)hs.f_bfree;
        u.f_tinode = hs.f_ffree;
        return lkd_user_write(LKD_ARGP(1), &u, 32) < 0 ? -LX_EFAULT : 0;
    }
    return -LX_EINVAL;
}

/* ---- access ------------------------------------------------------------------------------------ */

/* generic_permission on a virtual node: owner, group or other bits for the caller's fsuid/fsgid. */
static int virt_access(lkd_task *t, const struct fs_res *r, int amode)
{
    uint32_t mode = fs_res_mode(r);
    uint32_t uid = t && t->proc ? t->proc->cred.fsuid : 1000, gid = t && t->proc ? t->proc->cred.fsgid : 1000;
    if (uid == 0) return (amode & LX_X_OK) && !(mode & 0111) && !LX_S_ISDIR(mode) ? -LX_EACCES : 0;
    uint32_t vu = r->synthetic ? 0 : r->va.uid, vg = r->synthetic ? 0 : r->va.gid;
    uint32_t have = vu == uid ? (mode >> 6) & 7 : vg == gid ? (mode >> 3) & 7 : mode & 7;
    if ((amode & LX_R_OK) && !(have & 4)) return -LX_EACCES;
    if ((amode & LX_W_OK) && !(have & 2)) return -LX_EACCES;
    if ((amode & LX_X_OK) && !(have & 1)) return -LX_EACCES;
    return 0;
}

static int64_t do_access(lkd_task *t, int dirfd, uint64_t path_gva, int amode, int flags)
{
    if (amode & ~(LX_R_OK | LX_W_OK | LX_X_OK)) return -LX_EINVAL;
    if (flags & ~(LX_AT_EACCESS | LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    char path[LX_PATH_MAX];
    int n = fs_get_path(path_gva, path);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc;
    if (n == 0) {
        if (!(flags & LX_AT_EMPTY_PATH)) { free(r); return -LX_ENOENT; }
        if (dirfd == LX_AT_FDCWD) rc = fs_resolve(t, LX_AT_FDCWD, ".", FS_R_FOLLOW, r);
        else {
            lkd_fdobj *o = lkd_fd_get(t, dirfd, NULL);
            if (!o) { free(r); return -LX_EBADF; }
            struct fs_file *f = (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_path_ops) ? o->priv : NULL;
            if (f && f->host_path && f->mnt) {
                memset(r, 0, sizeof *r);
                snprintf(r->host, sizeof r->host, "%s", f->host_path);
                r->mnt = f->mnt;
                r->exists = fs_obj_hoststat(o, &r->st) == 0;
                rc = r->exists ? 0 : -LX_ENOENT;
            } else rc = -LX_ENOENT;   /* objects without a path (pipes, anon inodes): nothing to check */
            lkd_fdobj_put(o);
        }
    } else {
        rc = fs_resolve(t, dirfd, path, ((flags & LX_AT_SYMLINK_NOFOLLOW) ? 0 : FS_R_FOLLOW) | FS_R_NOMAGIC, r);
    }
    if (rc == 0) {
        if (amode == LX_F_OK) rc = 0;
        else if (r->synthetic || r->mnt->kind == FS_MNT_VIRT) rc = virt_access(t, r, amode);
        else {
            uint32_t ty = (uint32_t)r->st.st_mode & LX_S_IFMT;
            if ((amode & LX_W_OK) && r->mnt->ro && (ty == LX_S_IFREG || ty == LX_S_IFDIR || ty == LX_S_IFLNK)) rc = -LX_EROFS;
            else {
                int hf = AT_SYMLINK_NOFOLLOW | ((flags & LX_AT_EACCESS) ? AT_EACCESS : 0);
                int hm = ((amode & LX_R_OK) ? R_OK : 0) | ((amode & LX_W_OK) ? W_OK : 0) | ((amode & LX_X_OK) ? X_OK : 0);
                if (faccessat(AT_FDCWD, r->host, hm, hf) < 0) rc = (int)LKD_HOST_ERR();
            }
        }
    }
    free(r);
    return rc;
}

LKD_SYSCALL(access) { return do_access(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGI(1), 0); }
LKD_SYSCALL(faccessat) { return do_access(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), 0); }
LKD_SYSCALL(faccessat2) { return do_access(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), LKD_ARGI(3)); }

/* ---- readlink ---------------------------------------------------------------------------------- */

static int64_t do_readlink(lkd_task *t, int dirfd, uint64_t path_gva, uint64_t buf, int64_t bufsiz)
{
    if (bufsiz <= 0) return -LX_EINVAL;
    char path[LX_PATH_MAX], target[LX_PATH_MAX];
    int n = fs_get_path(path_gva, path);
    if (n < 0) return n;
    int64_t len;
    if (n == 0) {
        /* readlinkat(fd, "") on an O_PATH|O_NOFOLLOW descriptor of a symlink (fs/stat.c do_readlinkat) */
        lkd_fdobj *o = dirfd == LX_AT_FDCWD ? NULL : lkd_fd_get(t, dirfd, NULL);
        if (!o) return dirfd == LX_AT_FDCWD ? -LX_ENOENT : -LX_EBADF;
        struct fs_file *f = o->ops == &fs_path_ops ? o->priv : NULL;
        struct stat st;
        if (f && f->host_path && lstat(f->host_path, &st) == 0 && S_ISLNK(st.st_mode)) {
            ssize_t k = readlink(f->host_path, target, sizeof target);
            len = k < 0 ? LKD_HOST_ERR() : k;
        } else len = -LX_ENOENT;
        lkd_fdobj_put(o);
    } else {
        len = lkd_vfs_readlink(t, dirfd, path, target, sizeof target);
    }
    if (len < 0) return len;
    if (len > bufsiz) len = bufsiz;
    return lkd_user_write(buf, target, (size_t)len) < 0 ? -LX_EFAULT : len;
}

LKD_SYSCALL(readlink) { return do_readlink(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1), LKD_ARGL(2)); }
LKD_SYSCALL(readlinkat) { return do_readlink(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGL(3)); }

/* ---- cwd and root ------------------------------------------------------------------------------ */

LKD_SYSCALL(getcwd)
{
    char buf[LX_PATH_MAX + 16];
    uint64_t size = LKD_ARGU(1);
    int n = lkd_vfs_getcwd(t, buf, sizeof buf);
    if (n < 0) return n;
    if ((uint64_t)n + 1 > size) return -LX_ERANGE;
    if (lkd_user_write(LKD_ARGP(0), buf, (size_t)n + 1) < 0) return -LX_EFAULT;
    return n + 1;
}

static int dir_search_ok(lkd_task *t, const struct fs_res *r)
{
    if (!LX_S_ISDIR(fs_res_mode(r))) return -LX_ENOTDIR;
    if (r->synthetic || r->mnt->kind == FS_MNT_VIRT) return virt_access(t, r, LX_X_OK);
    return access(r->host, X_OK) < 0 ? (int)LKD_HOST_ERR() : 0;
}

LKD_SYSCALL(chdir)
{
    char path[LX_PATH_MAX];
    int n = fs_get_path(LKD_ARGP(0), path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, LX_AT_FDCWD, path, FS_R_FOLLOW | FS_R_NOMAGIC, r);
    if (rc == 0) rc = dir_search_ok(t, r);
    if (rc == 0) fs_set_cwd(t, r->gpath);
    free(r);
    return rc;
}

LKD_SYSCALL(fchdir)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    char g[LX_PATH_MAX];
    int rc = fs_dirobj_gpath(o, g, sizeof g);
    lkd_fdobj_put(o);
    if (rc < 0) return rc;
    fs_set_cwd(t, g);
    return 0;
}

/* chroot(2) needs CAP_SYS_CHROOT: -EPERM for a non-root caller. */
LKD_SYSCALL(chroot)
{
    char path[LX_PATH_MAX];
    int n = fs_get_path(LKD_ARGP(0), path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, LX_AT_FDCWD, path, FS_R_FOLLOW | FS_R_NOMAGIC, r);
    if (rc == 0) rc = dir_search_ok(t, r);
    if (rc == 0 && t->proc && t->proc->cred.euid != 0) rc = -LX_EPERM;
    if (rc == 0) fs_set_root(t, r->gpath);
    free(r);
    return rc;
}

void fs_register_stat(void)
{
    LKD_REGISTER(stat);
    LKD_REGISTER(fstat);
    LKD_REGISTER(lstat);
    LKD_REGISTER(newfstatat);
    LKD_REGISTER(statx);
    LKD_REGISTER(statfs);
    LKD_REGISTER(fstatfs);
    LKD_REGISTER(ustat);
    LKD_REGISTER(access);
    LKD_REGISTER(faccessat);
    LKD_REGISTER(faccessat2);
    LKD_REGISTER(readlink);
    LKD_REGISTER(readlinkat);
    LKD_REGISTER(getcwd);
    LKD_REGISTER(chdir);
    LKD_REGISTER(fchdir);
    LKD_REGISTER(chroot);
}
