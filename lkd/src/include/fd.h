/* fd.h: the per-process fd table and the open file description objects. OWNER: fs.
 * Core provides weak placeholders (src/core/stubs_fs.c): a small working table so that other
 * modules can unit-test with fake descriptors before the fs module exists.
 *
 * Open file description sharing. An open file description is one struct lkd_fdobj, reference counted:
 * dup/dup2/F_DUPFD and CLONE_FILES share it inside a process. Across a guest fork the host fork copies
 * the process memory, so each process has its own copy of the object; what stays shared is the HOST
 * fd, which the host kernel shares across fork exactly like Linux (file offset and O_NONBLOCK/O_APPEND
 * status live in the host open file). Therefore:
 *   - for host-backed objects the file offset and the O_NONBLOCK/O_APPEND flags are read from and
 *     written to the host fd (lseek(fd, 0, SEEK_CUR), fcntl(F_GETFL/F_SETFL)), never cached;
 *   - objects without a host fd (virtual files, signalfd) keep their state in process memory and do not
 *     share it across fork. This is a documented gap, not something to fake.
 * Across SCM_RIGHTS to another host process an object travels by its host fd, or by what ops->export_fds sends
 * (a host twin, a record: see the end of this file); the receiver rebuilds it (net_scm_import).
 * The close-on-exec flag lives in the fd table entry (per descriptor, not per description) and is
 * mirrored on the host fd (FD_CLOEXEC) so that a host re-exec closes exactly what Linux would.
 */
#ifndef LKD_FD_H
#define LKD_FD_H

#include "task.h"

typedef enum lkd_fd_kind {
    LKD_FD_NONE = 0,
    LKD_FD_FILE, LKD_FD_DIR, LKD_FD_PIPE, LKD_FD_SOCKET, LKD_FD_EVENTFD, LKD_FD_TIMERFD,
    LKD_FD_SIGNALFD, LKD_FD_EPOLL, LKD_FD_INOTIFY, LKD_FD_MEMFD, LKD_FD_PROCFD, LKD_FD_DEV,
    LKD_FD_KIND_COUNT
} lkd_fd_kind;

typedef struct lkd_fdobj lkd_fdobj;

/* Operations of an object kind. Every pointer is optional; a NULL entry gives the Linux error shown.
 * `t` is the calling task. Buffers are GUEST addresses: use lkd_user_read/write on them (or, after
 * lkd_mm_check_range, hand lkd_g2h(gva) to a host call). Operations may block interruptibly through
 * lkd_block_wait_* and return -LX_ERESTARTSYS when interrupted (the caller turns it into -EINTR when
 * the syscall is not restartable). They return a count or value >= 0, or -LX_E*. */
typedef struct lkd_fd_ops {
    const char *name;                                   /* "file", "pipe", "eventfd"... for traces */
    int64_t (*read)(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset); /* NULL: -EINVAL; offset NULL = file position */
    int64_t (*write)(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset); /* NULL: -EINVAL */
    int64_t (*lseek)(lkd_task *t, lkd_fdobj *o, int64_t off, int whence);        /* NULL: -ESPIPE */
    int64_t (*ioctl)(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg);     /* NULL: -ENOTTY */
    int64_t (*fcntl)(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg);     /* object-specific commands (seals, locks, pipe size); NULL: -EINVAL */
    uint32_t (*poll)(lkd_fdobj *o, uint32_t want);       /* LX_POLL* events true now, never blocks; NULL: IN|OUT always */
    int (*fstat)(lkd_task *t, lkd_fdobj *o, struct lx_stat *out);                /* NULL: -EBADF */
    int64_t (*getdents)(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, int legacy); /* NULL: -ENOTDIR */
    int (*ftruncate)(lkd_task *t, lkd_fdobj *o, int64_t length);                 /* NULL: -EINVAL */
    int (*fsync)(lkd_task *t, lkd_fdobj *o, int datasync);                       /* NULL: -EINVAL */
    int (*mmap_fd)(lkd_fdobj *o);                        /* host fd the mm module may mmap, or -1 (-ENODEV) */
    int (*describe)(lkd_fdobj *o, char *buf, size_t cap);/* text of readlink(/proc/<pid>/fd/N): "/a/b", "pipe:[12]", "socket:[9]", "anon_inode:[eventfd]", "/memfd:x (deleted)"; returns length or -errno; NULL: use o->gpath */
    int (*fdinfo)(lkd_fdobj *o, char *buf, size_t cap);  /* extra lines of /proc/<pid>/fdinfo/N (after pos/flags/mnt_id); returns length; NULL: none */
    void (*release)(lkd_fdobj *o);                       /* last reference dropped: close host fds, free priv */
    /* SCM_RIGHTS to another host process: fills hfds with new host descriptors (close-on-exec, owned by the caller)
     * that carry this open file description, returns their count (at most cap) or -errno. NULL: a dup of host_fd, or
     * -EOPNOTSUPP without one. Objects with no host descriptor of their own send a host twin with the same behaviour
     * (devices) or a record (lkd_fdrec_export below). */
    int (*export_fds)(lkd_fdobj *o, int *hfds, int cap);
} lkd_fd_ops;

struct lkd_fdobj {
    _Atomic int refs;
    lkd_fd_kind kind;
    const lkd_fd_ops *ops;
    int host_fd;                      /* host descriptor carrying the data and the readiness, or -1 */
    int notify_fd;                    /* -1, or a host fd (pipe read end) that becomes readable whenever
                                         poll() may have changed; set by objects with no kernel-visible
                                         readiness (signalfd, emulated eventfd/timerfd). epoll registers it
                                         with kqueue and calls ops->poll for the truth. */
    _Atomic uint32_t status_flags;    /* Linux O_* status flags (access mode, O_APPEND, O_NONBLOCK...).
                                         For host-backed objects F_GETFL re-reads the host flags. */
    pthread_mutex_t lock;             /* LKD_RANK_FDOBJ: object-private state, never held across a wait */
    uint64_t ino;                     /* identity for pipe:[ino], socket:[ino], locks, procfs */
    uint64_t dev;
    char *gpath;                      /* guest path if known (malloc), else NULL */
    void *priv;                       /* kind-specific state, freed by ops->release */
};

/* ---- Objects ---------------------------------------------------------------------------------- */

/* New object with one reference. host_fd/notify_fd may be -1. gpath is copied. Never fails except
 * ENOMEM: returns NULL then. */
lkd_fdobj *lkd_fdobj_new(lkd_fd_kind kind, const lkd_fd_ops *ops, int host_fd, uint32_t status_flags, void *priv);
lkd_fdobj *lkd_fdobj_ref(lkd_fdobj *o);
void lkd_fdobj_put(lkd_fdobj *o); /* last put: net hook lkd_net_object_released(), ops->release, closes host_fd/notify_fd, frees */
/* A new independent open file description on the same file (open("/proc/self/fd/N") semantics,
 * syscall_reopen in HL): same inode, own offset and flags. A pipe reopens the end the object holds (Linux
 * allows both ends through /proc; the other end answers -LX_ENXIO here, request fs-1 point 4); sockets
 * answer -LX_ENXIO like Linux. Implemented by fs. */
int lkd_fdobj_reopen(lkd_task *t, lkd_fdobj *o, uint32_t lx_flags, lkd_fdobj **out);

/* ---- Table ------------------------------------------------------------------------------------ */

typedef struct lkd_fdtable lkd_fdtable; /* private to fs; fixed 65536 entries minimum, grows to RLIMIT_NOFILE */

/* Create an empty table. fs gives each new process one (std fds are set by the runtime). */
lkd_fdtable *lkd_fdtable_new(void);
/* CLONE_FILES: returns `parent` with one more reference. Otherwise a copy sharing the descriptions
 * (each object gets a reference, host fds are not duplicated: the fork child inherits them). */
lkd_fdtable *lkd_fdtable_clone(lkd_fdtable *parent, uint64_t clone_flags);
void lkd_fdtable_put(lkd_fdtable *tab);

/* Install `o` (takes its own reference; the caller keeps its own) at the lowest free fd >= min_fd, with
 * FD_CLOEXEC if cloexec. Returns the fd or -LX_EMFILE (RLIMIT_NOFILE) / -LX_EINVAL. */
int lkd_fd_alloc(lkd_task *t, lkd_fdobj *o, int min_fd, int cloexec);
/* Install at exactly `fd` (dup2/dup3 semantics: closes what is there first). */
int lkd_fd_install_at(lkd_task *t, int fd, lkd_fdobj *o, int cloexec);
/* Object behind fd, with a new reference to put with lkd_fdobj_put, or NULL (errno semantics: the caller
 * returns -LX_EBADF). If cloexec_out is not NULL it receives the descriptor flag. */
lkd_fdobj *lkd_fd_get(lkd_task *t, int fd, int *cloexec_out);
/* Close fd. Returns 0 or -LX_EBADF. The host fd is closed when the last reference to the object goes. */
int lkd_fd_close(lkd_task *t, int fd);
/* dup/dup2/dup3/F_DUPFD(_CLOEXEC): newfd < 0 means "lowest free >= min_fd". Returns the new fd or -errno. */
int lkd_fd_dup(lkd_task *t, int oldfd, int newfd, int min_fd, int cloexec);
int lkd_fd_set_cloexec(lkd_task *t, int fd, int cloexec); /* F_SETFD; 0 or -EBADF */
/* Exec: close every descriptor with FD_CLOEXEC. */
void lkd_fd_close_on_exec(lkd_task *t);
/* Number of open descriptors and the highest in use (for /proc/<pid>/status FDSize, limits). */
int lkd_fd_count(lkd_task *t);
/* Iterate for /proc/<pid>/fd of THIS process: fills fds[] with open numbers ascending; returns count. */
int lkd_fd_list(lkd_task *t, int *fds, int max);

/* ---- Hooks implemented by other modules, declared here because fd code calls them ------------- */

/* net: an object is being destroyed; remove it from every epoll set that references it (Linux removes
 * a description from epoll when its last reference goes). Weak no-op default in core. */
void lkd_net_object_released(lkd_fdobj *o);

/* ---- Records: open file descriptions that travel as a request to open the same thing again ------- */

/* An object with no host descriptor and no host twin (a synthetic or virtual directory, an O_PATH descriptor, a
 * generated /proc file, /dev/full, a signalfd) crosses a host process boundary as a record: a 16 KiB POSIX shm
 * object, unlinked at once, that starts with a magic and holds the record type, the Linux status flags and the
 * owning module's data. The receiving process asks that module to rebuild the object from the data
 * (net_scm_import). The rebuilt object is a new open file description of the same thing, opened with the
 * receiver's rights; offsets and flags changed afterwards are not shared (the rule above for objects without a
 * host fd). The data never holds host paths or pointers, only guest-level names. */
enum {
    LKD_FDREC_FS = 1,          /* fs: guest path opened again (synthetic and virtual directories, O_PATH) */
    LKD_FDREC_DEV = 2,         /* procfs: an emulated character device without a host twin (/dev/full, O_PATH) */
    LKD_FDREC_PROCFS = 3,      /* procfs: a generated file of /proc or /sys, or /proc/<pid>/mem, with its position */
    LKD_FDREC_SIGNALFD = 4,    /* sig: a signalfd with its mask (reads the receiver's own signals, as on Linux) */
    LKD_FDREC_PIPE = 5,        /* fs: the guest status flags of a pipe end, sent right before its host pipe */
    LKD_FDREC_TYPES
};
#define LKD_FDREC_MAX_DATA (16384u - 32u)

/* New record carrying data (at most LKD_FDREC_MAX_DATA bytes): *hfd receives a close-on-exec host descriptor.
 * Returns 1 (one descriptor, the count export_fds returns) or -errno. */
int lkd_fdrec_export(uint32_t type, uint32_t status_flags, const void *data, size_t len, int *hfd);
/* Rebuild an object from a record's data, in the receiving task t. */
typedef int (*lkd_fdrec_import_fn)(lkd_task *t, uint32_t status_flags, const void *data, size_t len, lkd_fdobj **out);
/* A module registers the importer of its record type at init. */
void lkd_fdrec_register(uint32_t type, lkd_fdrec_import_fn fn);
/* 0: host_fd is not a record (left untouched); 1: *out holds the rebuilt object; -errno: a record that could not
 * be rebuilt. host_fd stays owned by the caller in every case. */
int lkd_fdrec_import(lkd_task *t, int host_fd, lkd_fdobj **out);
/* 1 with its type and status flags when host_fd is a record, else 0. Reads only. */
int lkd_fdrec_peek(int host_fd, uint32_t *type, uint32_t *status_flags);

#endif /* LKD_FD_H */
