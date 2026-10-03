/* sys_path.c: namespace changes and attributes by path: rename*, mkdir*, rmdir, unlink*, link*,
 * symlink*, chmod*, chown*, umask, truncate, utime*, mknod*. The legacy calls map onto the *at forms.
 * Linux references: fs/namei.c (do_mkdirat, do_rmdir, do_unlinkat, do_linkat, do_symlinkat,
 * do_renameat2, do_mknodat, may_mknod), fs/open.c (do_fchmodat, do_fchownat, do_sys_truncate),
 * fs/utimes.c.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "fs_priv.h"

uint32_t fs_swap_umask(lkd_task *t, uint32_t nu);
int fs_memfd_chmod(lkd_fdobj *o, uint32_t mode);

/* Last component of a raw path ("" when the path is only slashes). */
static void last_component(const char *p, char *out, size_t cap)
{
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == '/') n--;
    size_t s = n;
    while (s > 0 && p[s - 1] != '/') s--;
    size_t l = n - s < cap - 1 ? n - s : cap - 1;
    memcpy(out, p + s, l);
    out[l] = 0;
}

static int dotty(const char *p)
{
    char c[LX_NAME_MAX + 2];
    last_component(p, c, sizeof c);
    if (!c[0]) return 3;               /* "/" */
    if (strcmp(c, ".") == 0) return 1;
    if (strcmp(c, "..") == 0) return 2;
    return 0;
}

/* Where a namespace change may happen: a writable host mount. Virtual mounts refuse creation with
 * -ENOENT (procfs/sysfs have no ->create) and removal with -EPERM. */
static int ns_writable(const struct fs_res *r, int creating)
{
    if (!r->mnt || r->synthetic || r->mnt->kind != FS_MNT_HOST) return creating ? -LX_ENOENT : -LX_EPERM;
    return r->mnt->ro ? -LX_EROFS : 0;
}

static int get_path_arg(uint64_t gva, char *buf)
{
    int n = fs_get_path(gva, buf);
    if (n < 0) return n;
    return n == 0 ? -LX_ENOENT : n;
}

/* ---- mkdir, rmdir, unlink ---------------------------------------------------------------------- */

static int64_t do_mkdirat(lkd_task *t, int dirfd, uint64_t pg, uint32_t mode)
{
    char path[LX_PATH_MAX];
    int n = get_path_arg(pg, path);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, path, FS_R_CREATE, r);
    if (rc == 0 && (r->exists || dotty(path))) rc = -LX_EEXIST;
    if (rc == 0) rc = ns_writable(r, 1);
    if (rc == 0 && mkdir(r->host, (mode_t)(mode & 01777 & ~lkd_vfs_umask(t))) < 0) rc = (int)LKD_HOST_ERR();
    free(r);
    return rc;
}
LKD_SYSCALL(mkdir) { return do_mkdirat(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1)); }
LKD_SYSCALL(mkdirat) { return do_mkdirat(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2)); }

static int64_t do_rmdir(lkd_task *t, int dirfd, const char *path)
{
    int d = dotty(path);
    if (d == 1) return -LX_EINVAL;
    if (d == 2) return -LX_ENOTEMPTY;
    if (d == 3) return -LX_EBUSY;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, path, 0, r);
    if (rc == 0 && !LX_S_ISDIR(fs_res_mode(r))) rc = -LX_ENOTDIR;
    if (rc == 0 && (fs_is_mount_point(r->gpath) || r->synthetic)) rc = -LX_EBUSY;
    if (rc == 0) rc = ns_writable(r, 0);
    if (rc == 0 && rmdir(r->host) < 0) rc = (int)LKD_HOST_ERR();
    free(r);
    return rc;
}

static int64_t do_unlink(lkd_task *t, int dirfd, const char *path)
{
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, path, 0, r);
    if (rc == 0 && LX_S_ISDIR(fs_res_mode(r))) rc = -LX_EISDIR;
    if (rc == 0) rc = ns_writable(r, 0);
    if (rc == 0 && unlink(r->host) < 0) rc = (int)LKD_HOST_ERR();
    free(r);
    return rc;
}

LKD_SYSCALL(rmdir)
{
    char path[LX_PATH_MAX];
    int n = get_path_arg(LKD_ARGP(0), path);
    return n < 0 ? n : do_rmdir(t, LX_AT_FDCWD, path);
}
LKD_SYSCALL(unlink)
{
    char path[LX_PATH_MAX];
    int n = get_path_arg(LKD_ARGP(0), path);
    return n < 0 ? n : do_unlink(t, LX_AT_FDCWD, path);
}
LKD_SYSCALL(unlinkat)
{
    int flags = LKD_ARGI(2);
    if (flags & ~LX_AT_REMOVEDIR) return -LX_EINVAL;
    char path[LX_PATH_MAX];
    int n = get_path_arg(LKD_ARGP(1), path);
    if (n < 0) return n;
    return (flags & LX_AT_REMOVEDIR) ? do_rmdir(t, LKD_ARGI(0), path) : do_unlink(t, LKD_ARGI(0), path);
}

/* ---- rename ------------------------------------------------------------------------------------ */

#define RENAME_EXCL_HOST 0x00000004
#define RENAME_SWAP_HOST 0x00000002
int renamex_np(const char *, const char *, unsigned int);

static int64_t do_renameat2(lkd_task *t, int ofd, uint64_t opg, int nfd, uint64_t npg, uint32_t flags)
{
    if (flags & ~(uint32_t)(LX_AT_RENAME_NOREPLACE | LX_AT_RENAME_EXCHANGE | LX_AT_RENAME_WHITEOUT)) return -LX_EINVAL;
    if ((flags & LX_AT_RENAME_NOREPLACE) && (flags & (LX_AT_RENAME_EXCHANGE | LX_AT_RENAME_WHITEOUT))) return -LX_EINVAL;
    if ((flags & LX_AT_RENAME_EXCHANGE) && (flags & LX_AT_RENAME_WHITEOUT)) return -LX_EINVAL;
    char op[LX_PATH_MAX], np[LX_PATH_MAX];
    int n = get_path_arg(opg, op);
    if (n < 0) return n;
    n = get_path_arg(npg, np);
    if (n < 0) return n;
    if (flags & LX_AT_RENAME_WHITEOUT) return -LX_EPERM;   /* needs CAP_MKNOD */
    if (dotty(op) || dotty(np)) return -LX_EBUSY;
    struct fs_res *ro = malloc(sizeof *ro), *rn = malloc(sizeof *rn);
    int rc = (!ro || !rn) ? -LX_ENOMEM : 0;
    if (rc == 0) rc = fs_resolve(t, ofd, op, 0, ro);
    if (rc == 0) rc = fs_resolve(t, nfd, np, FS_R_CREATE, rn);
    if (rc == 0 && ro->mnt != rn->mnt) rc = -LX_EXDEV;
    if (rc == 0) rc = ns_writable(ro, 0);
    if (rc == 0 && (fs_is_mount_point(ro->gpath) || (rn->exists && fs_is_mount_point(rn->gpath)))) rc = -LX_EBUSY;
    if (rc == 0 && (flags & LX_AT_RENAME_EXCHANGE) && !rn->exists) rc = -LX_ENOENT;
    if (rc == 0 && (flags & LX_AT_RENAME_NOREPLACE) && rn->exists) rc = -LX_EEXIST;
    if (rc == 0 && rn->exists && !(flags & LX_AT_RENAME_EXCHANGE)) {
        int od = LX_S_ISDIR(fs_res_mode(ro)), nd = LX_S_ISDIR(fs_res_mode(rn));
        if (od && !nd) rc = -LX_ENOTDIR;
        else if (!od && nd) rc = -LX_EISDIR;
    }
    if (rc == 0) {
        /* a directory cannot move below itself */
        size_t ol = strlen(ro->gpath);
        if (LX_S_ISDIR(fs_res_mode(ro)) && strncmp(rn->gpath, ro->gpath, ol) == 0 && rn->gpath[ol] == '/') rc = -LX_EINVAL;
    }
    if (rc == 0) {
        unsigned hf = (flags & LX_AT_RENAME_NOREPLACE) ? RENAME_EXCL_HOST : (flags & LX_AT_RENAME_EXCHANGE) ? RENAME_SWAP_HOST : 0;
        int hr = hf ? renamex_np(ro->host, rn->host, hf) : rename(ro->host, rn->host);
        if (hr < 0) rc = (int)LKD_HOST_ERR();
    }
    free(ro);
    free(rn);
    return rc;
}
LKD_SYSCALL(rename) { return do_renameat2(t, LX_AT_FDCWD, LKD_ARGP(0), LX_AT_FDCWD, LKD_ARGP(1), 0); }
LKD_SYSCALL(renameat) { return do_renameat2(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), LKD_ARGP(3), 0); }
LKD_SYSCALL(renameat2) { return do_renameat2(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), LKD_ARGP(3), LKD_ARGUI(4)); }

/* ---- link, symlink ----------------------------------------------------------------------------- */

static int64_t do_linkat(lkd_task *t, int ofd, uint64_t opg, int nfd, uint64_t npg, int flags)
{
    if (flags & ~(LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    char op[LX_PATH_MAX], np[LX_PATH_MAX];
    int n = fs_get_path(opg, op);
    if (n < 0) return n;
    if (n == 0) {
        /* AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH; without it Linux answers -ENOENT. */
        if (!(flags & LX_AT_EMPTY_PATH) || !t->proc || t->proc->cred.euid != 0) return -LX_ENOENT;
        return -LX_ENOENT; /* root: linking an open description has no host equivalent (documented) */
    }
    n = get_path_arg(npg, np);
    if (n < 0) return n;
    struct fs_res *ro = malloc(sizeof *ro), *rn = malloc(sizeof *rn);
    int rc = (!ro || !rn) ? -LX_ENOMEM : 0;
    if (rc == 0) rc = fs_resolve(t, ofd, op, (flags & LX_AT_SYMLINK_FOLLOW) ? FS_R_FOLLOW | FS_R_NOMAGIC : 0, ro);
    if (rc == 0) rc = fs_resolve(t, nfd, np, FS_R_CREATE, rn);
    if (rc == 0 && rn->exists) rc = -LX_EEXIST;
    if (rc == 0 && rn->trailing_slash) rc = -LX_ENOENT;
    if (rc == 0 && ro->mnt != rn->mnt) rc = -LX_EXDEV;
    if (rc == 0 && LX_S_ISDIR(fs_res_mode(ro))) rc = -LX_EPERM;
    if (rc == 0) rc = ns_writable(rn, 1);
    if (rc == 0 && linkat(AT_FDCWD, ro->host, AT_FDCWD, rn->host, 0) < 0) rc = (int)LKD_HOST_ERR();
    free(ro);
    free(rn);
    return rc;
}
LKD_SYSCALL(link) { return do_linkat(t, LX_AT_FDCWD, LKD_ARGP(0), LX_AT_FDCWD, LKD_ARGP(1), 0); }
LKD_SYSCALL(linkat) { return do_linkat(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), LKD_ARGP(3), LKD_ARGI(4)); }

/* The target text is stored verbatim: an absolute target names a GUEST path, resolved by LKD. */
static int64_t do_symlinkat(lkd_task *t, uint64_t tg, int nfd, uint64_t npg)
{
    char target[LX_PATH_MAX], np[LX_PATH_MAX];
    int n = get_path_arg(tg, target);
    if (n < 0) return n;
    n = get_path_arg(npg, np);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, nfd, np, FS_R_CREATE, r);
    if (rc == 0 && (r->exists || dotty(np))) rc = -LX_EEXIST;
    if (rc == 0 && r->trailing_slash) rc = -LX_ENOENT;
    if (rc == 0) rc = ns_writable(r, 1);
    if (rc == 0 && symlink(target, r->host) < 0) rc = (int)LKD_HOST_ERR();
    free(r);
    return rc;
}
LKD_SYSCALL(symlink) { return do_symlinkat(t, LKD_ARGP(0), LX_AT_FDCWD, LKD_ARGP(1)); }
LKD_SYSCALL(symlinkat) { return do_symlinkat(t, LKD_ARGP(0), LKD_ARGI(1), LKD_ARGP(2)); }

/* ---- mknod ------------------------------------------------------------------------------------- */

static int64_t do_mknodat(lkd_task *t, int dirfd, uint64_t pg, uint32_t mode, uint64_t dev)
{
    uint32_t type = mode & LX_S_IFMT;
    switch (type) {
    case 0: case LX_S_IFREG: case LX_S_IFIFO: case LX_S_IFSOCK: break;
    case LX_S_IFCHR: case LX_S_IFBLK:
        if (!t->proc || t->proc->cred.euid != 0) return -LX_EPERM;   /* CAP_MKNOD */
        break;
    case LX_S_IFDIR: return -LX_EPERM;
    default: return -LX_EINVAL;
    }
    char path[LX_PATH_MAX];
    int n = get_path_arg(pg, path);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, path, FS_R_CREATE, r);
    if (rc == 0 && (r->exists || dotty(path))) rc = -LX_EEXIST;
    if (rc == 0) rc = ns_writable(r, 1);
    mode_t perm = (mode_t)(mode & 07777 & ~lkd_vfs_umask(t));
    if (rc == 0) {
        int hr;
        if (type == 0 || type == LX_S_IFREG) {
            hr = open(r->host, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, perm);
            if (hr >= 0) { close(hr); hr = 0; }
        } else if (type == LX_S_IFIFO) hr = mkfifo(r->host, perm);
        else hr = mknod(r->host, (mode_t)type | perm, lkd_dev_from_linux(dev));
        if (hr < 0) rc = (int)LKD_HOST_ERR();
    }
    free(r);
    return rc;
}
LKD_SYSCALL(mknod) { return do_mknodat(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1), LKD_ARGU(2)); }
LKD_SYSCALL(mknodat) { return do_mknodat(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), LKD_ARGU(3)); }

/* ---- Attributes by path or fd ------------------------------------------------------------------ */

/* Resolve for an attribute change: path, or the fd itself with AT_EMPTY_PATH (or a NULL path for
 * utimensat). On success either *obj is set (fd case) or r is filled. */
static int attr_target(lkd_task *t, int dirfd, uint64_t pg, int flags, int null_is_fd, struct fs_res *r, lkd_fdobj **obj)
{
    *obj = NULL;
    char path[LX_PATH_MAX];
    int n = 0;
    if (pg == 0 && null_is_fd) n = 0;
    else {
        n = fs_get_path(pg, path);
        if (n < 0) return n;
        if (n == 0 && !(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
    }
    if (n == 0) {
        if (dirfd == LX_AT_FDCWD) return fs_resolve(t, LX_AT_FDCWD, ".", FS_R_FOLLOW, r);
        *obj = lkd_fd_get(t, dirfd, NULL);
        return *obj ? 0 : -LX_EBADF;
    }
    return fs_resolve(t, dirfd, path, (flags & LX_AT_SYMLINK_NOFOLLOW) ? FS_R_NOMAGIC : FS_R_FOLLOW | FS_R_NOMAGIC, r);
}

static int attr_writable(const struct fs_res *r)
{
    if (!r->mnt || r->synthetic || r->mnt->kind != FS_MNT_HOST) return -LX_EPERM;
    return r->mnt->ro ? -LX_EROFS : 0;
}

static int obj_host(lkd_fdobj *o, const char **path)
{
    struct fs_file *f = (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_path_ops ||
                         o->ops == &fs_stream_ops) ? o->priv : NULL;
    *path = f ? f->host_path : NULL;
    if (f && f->ro_mount) return -LX_EROFS;
    return 0;
}

static int64_t do_chmod(lkd_task *t, int dirfd, uint64_t pg, uint32_t mode, int flags)
{
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    lkd_fdobj *o;
    int rc = attr_target(t, dirfd, pg, flags, 0, r, &o);
    mode_t m = (mode_t)(mode & 07777);
    if (rc == 0 && o) {
        const char *hp;
        rc = obj_host(o, &hp);
        if (rc == 0) {
            if (o->ops == &fs_memfd_ops) rc = fs_memfd_chmod(o, mode);
            else if (o->host_fd >= 0) rc = fchmod(o->host_fd, m) < 0 ? (int)LKD_HOST_ERR() : 0;
            else if (hp) rc = fchmodat(AT_FDCWD, hp, m, AT_SYMLINK_NOFOLLOW) < 0 ? (int)LKD_HOST_ERR() : 0;
            else rc = -LX_EPERM;
        }
        lkd_fdobj_put(o);
    } else if (rc == 0) {
        rc = attr_writable(r);
        if (rc == 0 && LX_S_ISLNK(fs_res_mode(r))) rc = -LX_EOPNOTSUPP;   /* fchmodat2 AT_SYMLINK_NOFOLLOW on a link */
        if (rc == 0 && fchmodat(AT_FDCWD, r->host, m, AT_SYMLINK_NOFOLLOW) < 0) rc = (int)LKD_HOST_ERR();
    }
    free(r);
    return rc;
}
LKD_SYSCALL(chmod) { return do_chmod(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1), 0); }
LKD_SYSCALL(fchmodat) { return do_chmod(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), 0); }
LKD_SYSCALL(fchmodat2) { return do_chmod(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), LKD_ARGI(3)); }
LKD_SYSCALL(fchmod)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int rc;
    const char *hp;
    if (fs_obj_is_path(o)) rc = -LX_EBADF;
    else if (o->ops == &fs_memfd_ops) rc = fs_memfd_chmod(o, LKD_ARGUI(1));
    else if ((rc = obj_host(o, &hp)) == 0)
        rc = o->host_fd >= 0 ? (fchmod(o->host_fd, (mode_t)(LKD_ARGUI(1) & 07777)) < 0 ? (int)LKD_HOST_ERR() : 0) : -LX_EPERM;
    lkd_fdobj_put(o);
    return rc;
}

static int64_t do_chown(lkd_task *t, int dirfd, uint64_t pg, uint32_t uid, uint32_t gid, int flags, int is_fd)
{
    uid_t hu;
    gid_t hg;
    if (!is_fd && (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH))) return -LX_EINVAL;
    int rc = fs_uid_to_host(uid, &hu);
    if (rc == 0) rc = fs_gid_to_host(gid, &hg);
    if (rc < 0) return rc;
    if (is_fd) {
        lkd_fdobj *o = lkd_fd_get(t, dirfd, NULL);
        if (!o) return -LX_EBADF;
        const char *hp;
        if (fs_obj_is_path(o)) rc = -LX_EBADF;
        else if ((rc = obj_host(o, &hp)) == 0)
            rc = o->host_fd >= 0 ? (fchown(o->host_fd, hu, hg) < 0 ? (int)LKD_HOST_ERR() : 0) : -LX_EPERM;
        lkd_fdobj_put(o);
        return rc;
    }
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    lkd_fdobj *o;
    rc = attr_target(t, dirfd, pg, flags, 0, r, &o);
    if (rc == 0 && o) {
        const char *hp;
        rc = obj_host(o, &hp);
        if (rc == 0) {
            if (o->host_fd >= 0) rc = fchown(o->host_fd, hu, hg) < 0 ? (int)LKD_HOST_ERR() : 0;
            else if (hp) rc = fchownat(AT_FDCWD, hp, hu, hg, AT_SYMLINK_NOFOLLOW) < 0 ? (int)LKD_HOST_ERR() : 0;
            else rc = -LX_EPERM;
        }
        lkd_fdobj_put(o);
    } else if (rc == 0) {
        rc = attr_writable(r);
        if (rc == 0 && fchownat(AT_FDCWD, r->host, hu, hg, AT_SYMLINK_NOFOLLOW) < 0) rc = (int)LKD_HOST_ERR();
    }
    free(r);
    return rc;
}
LKD_SYSCALL(chown) { return do_chown(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1), LKD_ARGUI(2), 0, 0); }
LKD_SYSCALL(lchown) { return do_chown(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGUI(1), LKD_ARGUI(2), LX_AT_SYMLINK_NOFOLLOW, 0); }
LKD_SYSCALL(fchownat) { return do_chown(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), LKD_ARGUI(3), LKD_ARGI(4), 0); }
LKD_SYSCALL(fchown) { return do_chown(t, LKD_ARGI(0), 0, LKD_ARGUI(1), LKD_ARGUI(2), 0, 1); }

LKD_SYSCALL(umask) { return fs_swap_umask(t, LKD_ARGUI(0)); }

LKD_SYSCALL(truncate)
{
    int64_t len = LKD_ARGL(1);
    if (len < 0) return -LX_EINVAL;
    char path[LX_PATH_MAX];
    int n = get_path_arg(LKD_ARGP(0), path);
    if (n < 0) return n;
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, LX_AT_FDCWD, path, FS_R_FOLLOW | FS_R_NOMAGIC, r);
    if (rc == 0 && LX_S_ISDIR(fs_res_mode(r))) rc = -LX_EISDIR;
    else if (rc == 0 && !LX_S_ISREG(fs_res_mode(r))) rc = -LX_EINVAL;
    if (rc == 0) rc = attr_writable(r);
    if (rc == 0 && truncate(r->host, len) < 0) rc = (int)LKD_HOST_ERR();
    free(r);
    return rc;
}

/* ---- utimes ------------------------------------------------------------------------------------ */

static int nsec_ok(int64_t ns) { return ns == LX_UTIME_NOW || ns == LX_UTIME_OMIT || (ns >= 0 && ns <= 999999999); }

static int64_t do_utimens(lkd_task *t, int dirfd, uint64_t pg, const struct lx_timespec *ts, int flags, int null_is_fd)
{
    if (flags & ~(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)) return -LX_EINVAL;
    struct timespec h[2], *hp = NULL;
    if (ts) {
        for (int i = 0; i < 2; i++) {
            if (!nsec_ok(ts[i].tv_nsec)) return -LX_EINVAL;
            h[i].tv_sec = ts[i].tv_sec;
            h[i].tv_nsec = ts[i].tv_nsec == LX_UTIME_NOW ? UTIME_NOW : ts[i].tv_nsec == LX_UTIME_OMIT ? UTIME_OMIT : ts[i].tv_nsec;
        }
        hp = h;
    }
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    lkd_fdobj *o;
    int rc = attr_target(t, dirfd, pg, flags, null_is_fd, r, &o);
    if (rc == 0 && o) {
        const char *hpth;
        rc = obj_host(o, &hpth);
        if (rc == 0) {
            if (o->host_fd >= 0) rc = futimens(o->host_fd, hp) < 0 ? (int)LKD_HOST_ERR() : 0;
            else if (hpth) rc = utimensat(AT_FDCWD, hpth, hp, AT_SYMLINK_NOFOLLOW) < 0 ? (int)LKD_HOST_ERR() : 0;
            else rc = -LX_EPERM;
        }
        lkd_fdobj_put(o);
    } else if (rc == 0) {
        rc = attr_writable(r);
        if (rc == 0 && utimensat(AT_FDCWD, r->host, hp, AT_SYMLINK_NOFOLLOW) < 0) rc = (int)LKD_HOST_ERR();
    }
    free(r);
    return rc;
}

LKD_SYSCALL(utimensat)
{
    struct lx_timespec ts[2];
    if (LKD_ARGP(2) && lkd_user_read(LKD_ARGP(2), ts, sizeof ts) < 0) return -LX_EFAULT;
    return do_utimens(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2) ? ts : NULL, LKD_ARGI(3), 1);
}

static int64_t do_utimes_tv(lkd_task *t, int dirfd, uint64_t pg, uint64_t tvp)
{
    struct lx_timespec ts[2];
    if (tvp) {
        struct lx_timeval tv[2];
        if (lkd_user_read(tvp, tv, sizeof tv) < 0) return -LX_EFAULT;
        for (int i = 0; i < 2; i++) {
            if (tv[i].tv_usec < 0 || tv[i].tv_usec >= 1000000) return -LX_EINVAL;
            ts[i].tv_sec = tv[i].tv_sec;
            ts[i].tv_nsec = tv[i].tv_usec * 1000;
        }
    }
    return do_utimens(t, dirfd, pg, tvp ? ts : NULL, 0, 0);
}
LKD_SYSCALL(utimes) { return do_utimes_tv(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1)); }
LKD_SYSCALL(futimesat) { return do_utimes_tv(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2)); }
LKD_SYSCALL(utime)
{
    struct lx_timespec ts[2];
    if (LKD_ARGP(1)) {
        int64_t ub[2];
        if (lkd_user_read(LKD_ARGP(1), ub, sizeof ub) < 0) return -LX_EFAULT;
        ts[0].tv_sec = ub[0]; ts[0].tv_nsec = 0;
        ts[1].tv_sec = ub[1]; ts[1].tv_nsec = 0;
    }
    return do_utimens(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1) ? ts : NULL, 0, 0);
}

void fs_register_path(void)
{
    LKD_REGISTER(mkdir); LKD_REGISTER(mkdirat); LKD_REGISTER(rmdir);
    LKD_REGISTER(unlink); LKD_REGISTER(unlinkat);
    LKD_REGISTER(rename); LKD_REGISTER(renameat); LKD_REGISTER(renameat2);
    LKD_REGISTER(link); LKD_REGISTER(linkat); LKD_REGISTER(symlink); LKD_REGISTER(symlinkat);
    LKD_REGISTER(mknod); LKD_REGISTER(mknodat);
    LKD_REGISTER(chmod); LKD_REGISTER(fchmod); LKD_REGISTER(fchmodat); LKD_REGISTER(fchmodat2);
    LKD_REGISTER(chown); LKD_REGISTER(fchown); LKD_REGISTER(lchown); LKD_REGISTER(fchownat);
    LKD_REGISTER(umask); LKD_REGISTER(truncate);
    LKD_REGISTER(utime); LKD_REGISTER(utimes); LKD_REGISTER(futimesat); LKD_REGISTER(utimensat);
}
