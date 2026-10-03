/* fs_priv.h: internal declarations of the fs module (vfs, fd table, file objects, file syscalls).
 * Not for other modules: they use fd.h, vfs.h and the lkd_fs_* functions of lkd_fs.h. */
#ifndef LKD_FS_PRIV_H
#define LKD_FS_PRIV_H

#include <sys/stat.h>

#include "lkd_internal.h"
#include "fd.h"
#include "vfs.h"
#include "task.h"
#include "lkd_fs.h"

#define FS_MAX_MOUNTS 64
#define FS_MAXSYMLINKS 40          /* Linux MAXSYMLINKS, include/linux/namei.h */
#define FS_FD_HARD_MAX (1 << 20)   /* Linux fs.nr_open default (1048576) */
#define FS_MAX_RW 0x7ffff000ll     /* Linux MAX_RW_COUNT: INT_MAX & PAGE_MASK */

/* Effective configuration from core (src/core/core_priv.h; request docs/requests/fs-1.md). */
const lkd_config *lkd_config_get(void);

/* ---- Mounts ------------------------------------------------------------------------------------ */

enum { FS_MNT_HOST = 1, FS_MNT_VIRT = 2 };

struct fs_mount {
    char *gpath;                 /* canonical guest mount point, "/" for the root */
    size_t glen;
    int kind;
    char *host;                  /* FS_MNT_HOST: canonical host directory (realpath) */
    int ro;
    const lkd_vmount_ops *ops;   /* FS_MNT_VIRT */
    void *priv;
    int id;                      /* mount id for mountinfo and statx */
    uint64_t lx_dev;             /* Linux st_dev of the mount root */
    char fstype[32];
};

/* Mounts are only added (never removed); readers need no lock. */
int fs_mount_count(void);
const struct fs_mount *fs_mount_at(int i);
const struct fs_mount *fs_mount_find(const char *gpath);       /* longest prefix, NULL if none */
int fs_is_mount_point(const char *gpath);                        /* exact mount point */
int fs_is_mount_ancestor(const char *gpath);                     /* a mount lies strictly below */

/* ---- Path resolution --------------------------------------------------------------------------- */

#define FS_R_FOLLOW   0x1u   /* follow a final symlink */
#define FS_R_CREATE   0x2u   /* the final component may be missing (its parent must exist) */
#define FS_R_NOMAGIC  0x4u   /* treat a final magic link as an ordinary link (readlink-like ops) */
#define FS_R_GLOBAL   0x8u   /* the path is a stored global guest path: resolve from "/", not the task root */

struct fs_res {
    char gpath[LX_PATH_MAX];     /* canonical guest path (global namespace) */
    char host[LX_PATH_MAX];      /* host path when mnt->kind == FS_MNT_HOST */
    const struct fs_mount *mnt;
    const char *rel;             /* FS_MNT_VIRT: path relative to the mount (into gpath) */
    int exists;
    int synthetic;               /* directory that exists only because a mount lies below */
    int magic;                   /* final component is a magic link and FS_R_FOLLOW was given */
    int trailing_slash;          /* the path ended with '/' */
    struct stat st;              /* host lstat of the final object (host mounts, exists) */
    struct lkd_vattr va;         /* attributes (virtual mounts, exists) */
};

/* Resolve gpath relative to dirfd (LX_AT_FDCWD or a directory fd) against the task's root.
 * Returns 0 or -errno. Empty path: -ENOENT (callers handle AT_EMPTY_PATH first). */
int fs_resolve(lkd_task *t, int dirfd, const char *gpath, unsigned flags, struct fs_res *r);
/* openat2 RESOLVE_* restrictions applied by fs_resolve on the calling thread while set. */
#define FS_RESOLVE_NO_XDEV 0x01u
#define FS_RESOLVE_NO_MAGICLINKS 0x02u
#define FS_RESOLVE_NO_SYMLINKS 0x04u
#define FS_RESOLVE_BENEATH 0x08u
#define FS_RESOLVE_IN_ROOT 0x10u
struct fs_restrict { uint64_t resolve; };
void fs_restrict_set(const struct fs_restrict *rs);
/* Mode bits (LX_S_IF* | perms) of a resolved existing object. */
uint32_t fs_res_mode(const struct fs_res *r);
/* Fill a Linux stat for a resolved existing object (follows nothing). */
int fs_res_stat(lkd_task *t, const struct fs_res *r, struct lx_stat *out);
/* The res is on a writable host mount (0) or -EROFS / -EACCES (virtual) */
int fs_res_writable_host(const struct fs_res *r);

/* fsctx accessors (copy under the fsctx lock). */
void fs_get_root(lkd_task *t, char *out, size_t cap);
void fs_get_cwd(lkd_task *t, char *out, size_t cap);
void fs_set_cwd(lkd_task *t, const char *gpath);
void fs_set_root(lkd_task *t, const char *gpath);

/* ---- Stat helpers ------------------------------------------------------------------------------ */

uint32_t fs_uid_from_host(uid_t u);
uint32_t fs_gid_from_host(gid_t g);
int fs_uid_to_host(uint32_t u, uid_t *out);   /* 0, -EPERM */
int fs_gid_to_host(uint32_t g, gid_t *out);
void fs_stat_from_host(const struct stat *hs, struct lx_stat *ls);
void fs_statx_from_host(const struct stat *hs, uint32_t mask, struct lx_statx *x);
void fs_stat_from_vattr(const struct fs_mount *m, const struct lkd_vattr *va, struct lx_stat *ls);
void fs_statx_from_lxstat(const struct lx_stat *s, struct lx_statx *x);

/* ---- File objects ------------------------------------------------------------------------------ */

/* Private state of host-backed file, directory and O_PATH objects. */
struct fs_file {
    char *host_path;             /* host path at open time (O_PATH objects use it for every op) */
    int ro_mount;
    int own_offset;              /* 1: offset lives in `pos` (reopened deleted file), not in the host fd */
    int64_t pos;
    int mount_id;
    uint64_t lx_dev;
    /* directories */
    const struct fs_mount *mnt;  /* the mount of the directory */
    int synthetic;
    struct fs_dirent *ents;      /* snapshot taken by the first getdents after open/rewind */
    size_t nents;
    int snap_valid;
    uint64_t dpos;               /* index of the next entry */
};

struct fs_dirent { char *name; uint64_t ino; uint8_t type; };

uint64_t fs_memfd_bias(lkd_fdobj *o);   /* host phase of a memfd, 0 otherwise (memfd.c) */
extern const lkd_fd_ops fs_file_ops, fs_dir_ops, fs_path_ops, fs_stream_ops, fs_pipe_ops, fs_memfd_ops;

/* Open a resolved existing host object as an fd object (flags are Linux open flags). */
int fs_open_host(lkd_task *t, struct fs_res *r, uint32_t lx_flags, uint32_t mode, int created, lkd_fdobj **out);
/* Same with a host fd already opened by the caller (O_CREAT path); takes ownership of hfd. */
int fs_open_host_fd(lkd_task *t, struct fs_res *r, uint32_t lx_flags, int hfd, lkd_fdobj **out);
/* Directory object for a virtual or synthetic directory (no host fd). */
int fs_open_vdir(lkd_task *t, const struct fs_res *r, uint32_t lx_flags, lkd_fdobj **out);
/* Wrap a host fd that LKD owns; type chosen from fstat. Takes ownership of host_fd. */
int fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *gpath, const char *host_path, lkd_fdobj **out);
/* Guest path of a directory object (for *at calls), or -ENOTDIR / -EBADF. */
int fs_dirobj_gpath(lkd_fdobj *o, char *out, size_t cap);
int fs_obj_is_dir(lkd_fdobj *o);
uint32_t fs_obj_access(lkd_fdobj *o);       /* LX_O_ACCMODE bits of the description */
int fs_obj_is_path(lkd_fdobj *o);            /* O_PATH description */
/* stat of any object (fstat semantics, O_PATH allowed). */
int fs_obj_stat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out);
int fs_obj_hoststat(lkd_fdobj *o, struct stat *st);  /* host-backed only: 0 or -errno */

/* Read/write helper between a host fd and guest memory in chunks (partial results at the first fault,
 * like Linux copy_to_user). off NULL: file position. Returns bytes or -errno. */
int64_t fs_host_io(int host_fd, uint64_t gva, uint64_t count, const int64_t *off, int is_write);

/* memfd */
int fs_memfd_seals(lkd_fdobj *o);                       /* -1 if not a memfd */
int64_t fs_memfd_fcntl_seals(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg);

/* pipes */
int fs_pipe_create(int nonblock, lkd_fdobj **rd, lkd_fdobj **wr);
void fs_sigpipe(lkd_task *t);

/* ioctl on terminals (imported termios translation). */
int64_t fs_tty_ioctl(lkd_task *t, int host_fd, uint32_t cmd, uint64_t arg);

/* Directory snapshot reading (host and virtual). */
int64_t fs_getdents(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, int legacy);

/* Text for describe of a file object. */
int fs_describe_path(lkd_fdobj *o, char *buf, size_t cap);

/* Read a guest path argument. Returns length, -EFAULT, -ENAMETOOLONG. */
int fs_get_path(uint64_t gva, char *buf);

/* fd table internals used by fcntl and close_range. */
int fs_fd_limit(lkd_task *t);
int fs_fdtable_close_range(lkd_task *t, unsigned first, unsigned last, int cloexec_only);
int fs_fd_get_cloexec(lkd_task *t, int fd);
void fs_fdtables_fork_prepare(void);
void fs_fdtables_fork_parent(void);
void fs_fdtables_fork_child(void);

/* Registration of the syscalls of each file. */
void fs_register_open(void);
void fs_register_rw(void);
void fs_register_stat(void);
void fs_register_path(void);
void fs_register_misc(void);
void fs_register_pipe(void);
void fs_register_memfd(void);
void fs_register_xattr(void);

void fs_side_forget(const lkd_fdobj *o);

/* Status flag helpers. */
uint32_t fs_obj_getfl(lkd_fdobj *o);
int fs_obj_setfl(lkd_task *t, lkd_fdobj *o, uint32_t lx_flags);

#endif
