/* vfs.h: rooted path resolution against the guest root, binds, and virtual mounts. OWNER: fs.
 * Core provides weak placeholders (src/core/stubs_fs.c) returning -ENOSYS.
 *
 * The guest sees a root file system that is a host directory (the guest root, lkd_config.guest_root)
 * with extra host directories bound at guest paths, and virtual mounts (/proc, /dev, /sys) served by
 * other modules through struct lkd_vmount_ops. Resolution is done by LKD itself (component by
 * component, so ".." after a symlink, ELOOP at 40 links and chroot escape rules follow Linux), never by
 * handing a guest path to the host.
 *
 * Virtual mount contract (stable, request fs-1 point 3): readdir never returns "." or ".." (the vfs adds
 * them and ignores them if returned) and the vfs adds the mount points directly below the listed
 * directory. getattr is called with nofollow = 1 during the walk; an ordinary link is read by readlink and
 * resolved in guest space (absolute targets from the task's root). A final magic_link node that is
 * followed is opened by open(rel); stat that follows one does open(rel, O_PATH) then fstat. An inner magic
 * link is followed only when its text is an absolute path, else -ENOTDIR. open is called for non-directory
 * nodes and final magic links; a virtual directory is opened by the vfs itself (LKD_FD_DIR without host fd,
 * getdents through readdir). Mounts are never removed; a host mount at the same point replaces the old
 * one, a second virtual mount at the same point is -EBUSY.
 */
#ifndef LKD_VFS_H
#define LKD_VFS_H

#include "fd.h"

struct lkd_fsctx; /* cwd, root, umask; refcounted; shared by CLONE_FS */
typedef struct lkd_fsctx lkd_fsctx;

lkd_fsctx *lkd_fsctx_new(void);                                   /* cwd = root = "/" , umask 022 */
lkd_fsctx *lkd_fsctx_clone(lkd_fsctx *parent, uint64_t clone_flags); /* CLONE_FS shares */
void lkd_fsctx_put(lkd_fsctx *ctx);
/* Guest cwd as a canonical absolute path ("/" if deleted), copied into out. Returns length or -errno. */
int lkd_vfs_getcwd(lkd_task *t, char *out, size_t cap);
uint32_t lkd_vfs_umask(lkd_task *t);

/* ---- Resolution and file operations on guest paths -------------------------------------------- */

/* dirfd is LX_AT_FDCWD or a directory fd. flags: LX_AT_SYMLINK_NOFOLLOW, LX_AT_EMPTY_PATH. */
int lkd_vfs_open(lkd_task *t, int dirfd, const char *gpath, uint32_t lx_flags, uint32_t mode, lkd_fdobj **out);
int lkd_vfs_stat(lkd_task *t, int dirfd, const char *gpath, int flags, struct lx_stat *out);
int64_t lkd_vfs_readlink(lkd_task *t, int dirfd, const char *gpath, char *buf, size_t cap); /* length, not NUL-terminated */
/* Canonical guest path with every symlink resolved (like realpath), into out. Returns length or -errno. */
int lkd_vfs_realpath(lkd_task *t, int dirfd, const char *gpath, char *out, size_t cap);
/* Host path of a guest path that lives on a host-backed mount (exec, mmap by path, tools). Returns 0 or
 * -LX_ENOENT / -LX_EXDEV when the path is virtual. */
int lkd_vfs_to_host(lkd_task *t, int dirfd, const char *gpath, int flags, char *host_out, size_t cap);

/* ---- Mounts ----------------------------------------------------------------------------------- */

/* Set the guest root: a host directory. Called once by lkd_fs_init from lkd_config. */
int lkd_vfs_set_root(const char *host_dir);
/* Bind a host directory at a guest path ("/tmp" -> host tmp, "/dev/shm"). ro != 0 for read-only. */
int lkd_vfs_mount_bind(const char *guest_path, const char *host_dir, int ro);

/* Attributes of a node of a virtual mount. */
struct lkd_vattr {
    uint32_t mode;           /* file type and permission bits (LX_S_IF*) */
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t ino;
    uint64_t size;           /* 0 for generated files, like /proc */
    uint64_t rdev;
    int64_t mtime_sec, mtime_nsec;
    uint8_t magic_link;      /* symlink whose OPEN follows the object (proc/<pid>/fd/N, exe, cwd, root) */
};

/* A virtual mount is a tree of generated nodes. Paths are relative to the mount point, canonical (no
 * "." or ".." or empty components), no leading '/'; "" is the mount root. `priv` is the pointer given
 * at mount time. `t` is the calling task (permission checks, /proc/self). Every function returns 0 /
 * a count or -LX_E*. The vfs follows symlinks returned by readlink for ordinary links; for a
 * magic_link node it calls open() on the link itself, and the mount returns the object (usually through
 * lkd_fdobj_reopen). */
typedef struct lkd_vmount_ops {
    const char *fstype;                                                      /* "proc", "sysfs", "devtmpfs" */
    int (*getattr)(lkd_task *t, void *priv, const char *rel, int nofollow, struct lkd_vattr *out);
    int64_t (*readlink)(lkd_task *t, void *priv, const char *rel, char *buf, size_t cap); /* length, no NUL */
    int (*open)(lkd_task *t, void *priv, const char *rel, uint32_t lx_flags, uint32_t mode, lkd_fdobj **out);
    /* Directory listing for an open directory: entry i (0-based) of the directory at rel. Returns 1 and
     * fills name/d_type/ino, 0 at the end, -errno. Called with increasing i; the mount may snapshot. */
    int (*readdir)(lkd_task *t, void *priv, const char *rel, uint64_t i, char name[256], uint8_t *d_type, uint64_t *ino);
} lkd_vmount_ops;

/* Mount a virtual tree at guest_path (procfs does it for /proc, /sys; devices for /dev). Returns 0 or
 * -errno (-LX_EBUSY if something is already mounted exactly there). Called from lkd_<module>_init(). */
int lkd_vfs_mount_virtual(const char *guest_path, const lkd_vmount_ops *ops, void *priv);

/* /proc/mounts and mountinfo text of the current mount table (malloc, caller frees). */
char *lkd_vfs_render_mounts(lkd_task *t, int mountinfo, size_t *len_out);

#endif /* LKD_VFS_H */
