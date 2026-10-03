/* xattr.c: extended attributes (fs/xattr.c, xattr_permission). Only the "user." namespace is stored, as
 * host attributes of the same name. "trusted." needs CAP_SYS_ADMIN (-EPERM to set, -ENODATA to get),
 * "security." attributes are not settable without capabilities here (-EPERM) and none exist (-ENODATA),
 * "system." (POSIX ACLs) is not supported by the host file system (-EOPNOTSUPP). Host attributes
 * outside "user." (com.apple.*) are never listed. user.* on anything but regular files and directories:
 * -EPERM to set, -ENODATA to get (Linux xattr_permission).
 */
#include <fcntl.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "fs_priv.h"

#define LX_XATTR_CREATE 1
#define LX_XATTR_REPLACE 2
#define LX_XATTR_NAME_MAX 255
#define LX_XATTR_SIZE_MAX 65536
#define LX_XATTR_LIST_MAX 65536

enum { NS_USER, NS_TRUSTED, NS_SECURITY, NS_SYSTEM, NS_BAD };

static int ns_of(const char *n)
{
    if (!strncmp(n, "user.", 5)) return NS_USER;
    if (!strncmp(n, "trusted.", 8)) return NS_TRUSTED;
    if (!strncmp(n, "security.", 9)) return NS_SECURITY;
    if (!strncmp(n, "system.", 7)) return NS_SYSTEM;
    return NS_BAD;
}

static int64_t host_err(void) { return errno == ENOATTR ? -LX_ENODATA : LKD_HOST_ERR(); }

/* Target: either a host path (with its mode) or a host fd. */
struct xt { const char *path; int fd; mode_t mode; int ro; int virt; lkd_fdobj *obj; struct fs_res *r; };

static int xt_path(lkd_task *t, uint64_t pg, int follow, struct xt *x)
{
    char path[LX_PATH_MAX];
    int n = fs_get_path(pg, path);
    if (n < 0) return n;
    if (n == 0) return -LX_ENOENT;
    x->r = malloc(sizeof *x->r);
    if (!x->r) return -LX_ENOMEM;
    int rc = fs_resolve(t, LX_AT_FDCWD, path, follow ? FS_R_FOLLOW | FS_R_NOMAGIC : 0, x->r);
    if (rc < 0) return rc;
    x->fd = -1;
    if (x->r->synthetic || x->r->mnt->kind != FS_MNT_HOST) { x->virt = 1; return 0; }
    x->path = x->r->host;
    x->mode = x->r->st.st_mode;
    x->ro = x->r->mnt->ro;
    return 0;
}

static int xt_fd(lkd_task *t, int fd, struct xt *x)
{
    x->obj = lkd_fd_get(t, fd, NULL);
    if (!x->obj) return -LX_EBADF;
    if (fs_obj_is_path(x->obj)) return -LX_EBADF;
    struct stat st;
    if (x->obj->ops == &fs_memfd_ops) { x->virt = 1; return 0; }   /* tmpfs user xattrs: not kept (gap) */
    if (x->obj->host_fd < 0 || fstat(x->obj->host_fd, &st) < 0) { x->virt = 1; return 0; }
    x->fd = x->obj->host_fd;
    x->mode = st.st_mode;
    struct fs_file *f = (x->obj->ops == &fs_file_ops || x->obj->ops == &fs_dir_ops) ? x->obj->priv : NULL;
    x->ro = f ? f->ro_mount : 0;
    if (x->obj->kind == LKD_FD_PIPE || x->obj->kind == LKD_FD_SOCKET) x->virt = 1;
    return 0;
}

static void xt_done(struct xt *x)
{
    free(x->r);
    if (x->obj) lkd_fdobj_put(x->obj);
}

static int read_name(uint64_t g, char *name)
{
    int64_t n = lkd_user_strn(g, name, LX_XATTR_NAME_MAX + 1);
    if (n == -LX_EFAULT) return -LX_EFAULT;
    if (n < 0 || n == 0 || n > LX_XATTR_NAME_MAX) return -LX_ERANGE;
    return 0;
}

static int user_ok(mode_t m) { return S_ISREG(m) || S_ISDIR(m); }

static int64_t x_set(lkd_task *t, struct xt *x, uint64_t name_g, uint64_t val_g, uint64_t size, int flags)
{
    (void)t;
    char name[LX_XATTR_NAME_MAX + 2];
    if (flags & ~(LX_XATTR_CREATE | LX_XATTR_REPLACE)) return -LX_EINVAL;
    int rc = read_name(name_g, name);
    if (rc < 0) return rc;
    if (size > LX_XATTR_SIZE_MAX) return -LX_E2BIG;
    int ns = ns_of(name);
    if (ns == NS_BAD || ns == NS_SYSTEM) return -LX_EOPNOTSUPP;
    if (ns != NS_USER) return -LX_EPERM;
    if (x->virt) return -LX_EOPNOTSUPP;
    if (!user_ok(x->mode)) return -LX_EPERM;
    if (x->ro) return -LX_EROFS;
    char *val = malloc(size ? size : 1);
    if (!val) return -LX_ENOMEM;
    if (size && lkd_user_read(val_g, val, size) < 0) { free(val); return -LX_EFAULT; }
    int hf = (flags & LX_XATTR_CREATE ? XATTR_CREATE : 0) | (flags & LX_XATTR_REPLACE ? XATTR_REPLACE : 0);
    int r = x->fd >= 0 ? fsetxattr(x->fd, name, val, size, 0, hf) : setxattr(x->path, name, val, size, 0, hf | XATTR_NOFOLLOW);
    free(val);
    return r < 0 ? host_err() : 0;
}

static int64_t x_get(lkd_task *t, struct xt *x, uint64_t name_g, uint64_t val_g, uint64_t size)
{
    (void)t;
    char name[LX_XATTR_NAME_MAX + 2];
    int rc = read_name(name_g, name);
    if (rc < 0) return rc;
    int ns = ns_of(name);
    if (ns == NS_BAD || ns == NS_SYSTEM) return -LX_EOPNOTSUPP;
    if (ns != NS_USER) return -LX_ENODATA;
    if (x->virt) return -LX_EOPNOTSUPP;
    if (!user_ok(x->mode)) return -LX_ENODATA;
    ssize_t need = x->fd >= 0 ? fgetxattr(x->fd, name, NULL, 0, 0, 0) : getxattr(x->path, name, NULL, 0, 0, XATTR_NOFOLLOW);
    if (need < 0) return host_err();
    if (size == 0) return need;
    if (size > LX_XATTR_SIZE_MAX) size = LX_XATTR_SIZE_MAX;
    if ((uint64_t)need > size) return -LX_ERANGE;
    char *val = malloc(need ? (size_t)need : 1);
    if (!val) return -LX_ENOMEM;
    ssize_t got = x->fd >= 0 ? fgetxattr(x->fd, name, val, (size_t)need, 0, 0) : getxattr(x->path, name, val, (size_t)need, 0, XATTR_NOFOLLOW);
    int64_t ret = got < 0 ? host_err() : (lkd_user_write(val_g, val, (size_t)got) < 0 ? -LX_EFAULT : got);
    free(val);
    return ret;
}

static int64_t x_list(lkd_task *t, struct xt *x, uint64_t list_g, uint64_t size)
{
    (void)t;
    if (x->virt) return 0;   /* no attributes */
    ssize_t n = x->fd >= 0 ? flistxattr(x->fd, NULL, 0, 0) : listxattr(x->path, NULL, 0, XATTR_NOFOLLOW);
    if (n < 0) return host_err();
    char *all = malloc(n ? (size_t)n : 1), *out = malloc(n ? (size_t)n : 1);
    if (!all || !out) { free(all); free(out); return -LX_ENOMEM; }
    n = x->fd >= 0 ? flistxattr(x->fd, all, (size_t)n, 0) : listxattr(x->path, all, (size_t)n, XATTR_NOFOLLOW);
    if (n < 0) { free(all); free(out); return host_err(); }
    size_t used = 0;
    for (ssize_t p = 0; p < n;) {
        size_t l = strnlen(all + p, (size_t)(n - p));
        if (l && ns_of(all + p) == NS_USER) { memcpy(out + used, all + p, l + 1); used += l + 1; }
        p += (ssize_t)l + 1;
    }
    free(all);
    int64_t ret;
    if (size == 0) ret = (int64_t)used;
    else if (used > size) ret = -LX_ERANGE;
    else ret = (used && lkd_user_write(list_g, out, used) < 0) ? -LX_EFAULT : (int64_t)used;
    free(out);
    return ret;
}

static int64_t x_remove(lkd_task *t, struct xt *x, uint64_t name_g)
{
    (void)t;
    char name[LX_XATTR_NAME_MAX + 2];
    int rc = read_name(name_g, name);
    if (rc < 0) return rc;
    int ns = ns_of(name);
    if (ns == NS_BAD || ns == NS_SYSTEM) return -LX_EOPNOTSUPP;
    if (ns != NS_USER) return -LX_EPERM;
    if (x->virt) return -LX_EOPNOTSUPP;
    if (!user_ok(x->mode)) return -LX_EPERM;
    if (x->ro) return -LX_EROFS;
    int r = x->fd >= 0 ? fremovexattr(x->fd, name, 0) : removexattr(x->path, name, XATTR_NOFOLLOW);
    return r < 0 ? host_err() : 0;
}

#define WITH_PATH(follow, call) do { struct xt x = { .fd = -1 }; int rc_ = xt_path(t, LKD_ARGP(0), follow, &x); \
    int64_t r_ = rc_ < 0 ? rc_ : (call); xt_done(&x); return r_; } while (0)
#define WITH_FD(call) do { struct xt x = { .fd = -1 }; int rc_ = xt_fd(t, LKD_ARGI(0), &x); \
    int64_t r_ = rc_ < 0 ? rc_ : (call); xt_done(&x); return r_; } while (0)

LKD_SYSCALL(setxattr) { WITH_PATH(1, x_set(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3), LKD_ARGI(4))); }
LKD_SYSCALL(lsetxattr) { WITH_PATH(0, x_set(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3), LKD_ARGI(4))); }
LKD_SYSCALL(fsetxattr) { WITH_FD(x_set(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3), LKD_ARGI(4))); }
LKD_SYSCALL(getxattr) { WITH_PATH(1, x_get(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3))); }
LKD_SYSCALL(lgetxattr) { WITH_PATH(0, x_get(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3))); }
LKD_SYSCALL(fgetxattr) { WITH_FD(x_get(t, &x, LKD_ARGP(1), LKD_ARGP(2), LKD_ARGU(3))); }
LKD_SYSCALL(listxattr) { WITH_PATH(1, x_list(t, &x, LKD_ARGP(1), LKD_ARGU(2))); }
LKD_SYSCALL(llistxattr) { WITH_PATH(0, x_list(t, &x, LKD_ARGP(1), LKD_ARGU(2))); }
LKD_SYSCALL(flistxattr) { WITH_FD(x_list(t, &x, LKD_ARGP(1), LKD_ARGU(2))); }
LKD_SYSCALL(removexattr) { WITH_PATH(1, x_remove(t, &x, LKD_ARGP(1))); }
LKD_SYSCALL(lremovexattr) { WITH_PATH(0, x_remove(t, &x, LKD_ARGP(1))); }
LKD_SYSCALL(fremovexattr) { WITH_FD(x_remove(t, &x, LKD_ARGP(1))); }

void fs_register_xattr(void)
{
    LKD_REGISTER(setxattr); LKD_REGISTER(lsetxattr); LKD_REGISTER(fsetxattr);
    LKD_REGISTER(getxattr); LKD_REGISTER(lgetxattr); LKD_REGISTER(fgetxattr);
    LKD_REGISTER(listxattr); LKD_REGISTER(llistxattr); LKD_REGISTER(flistxattr);
    LKD_REGISTER(removexattr); LKD_REGISTER(lremovexattr); LKD_REGISTER(fremovexattr);
}
