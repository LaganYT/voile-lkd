/* sys_misc.c: dup*, fcntl, flock, ioctl, ftruncate, fallocate, fsync, fdatasync, sync, syncfs,
 * fadvise64, readahead, sync_file_range, getdents*, name_to_handle_at, open_by_handle_at.
 * Linux references: fs/file.c (dup2/dup3), fs/fcntl.c (do_fcntl), fs/locks.c (fcntl_getlk, flock),
 * fs/ioctl.c (do_vfs_ioctl), fs/open.c (do_sys_ftruncate, vfs_fallocate), fs/sync.c,
 * mm/fadvise.c, fs/readdir.c, fs/fhandle.c.
 */
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "fs_priv.h"

int fs_memfd_fallocate(lkd_fdobj *o, int mode, int64_t off, int64_t len);

/* ---- dup --------------------------------------------------------------------------------------- */

LKD_SYSCALL(dup) { return lkd_fd_dup(t, LKD_ARGI(0), -1, 0, 0); }

LKD_SYSCALL(dup2)
{
    int oldfd = LKD_ARGI(0), newfd = LKD_ARGI(1);
    if (oldfd == newfd) {
        lkd_fdobj *o = lkd_fd_get(t, oldfd, NULL);
        if (!o) return -LX_EBADF;
        lkd_fdobj_put(o);
        return newfd;
    }
    if (newfd < 0) return -LX_EBADF;
    return lkd_fd_dup(t, oldfd, newfd, 0, 0);
}

LKD_SYSCALL(dup3)
{
    int oldfd = LKD_ARGI(0), newfd = LKD_ARGI(1);
    uint32_t flags = LKD_ARGUI(2);
    if (flags & ~(uint32_t)LX_O_CLOEXEC) return -LX_EINVAL;
    if (oldfd == newfd) return -LX_EINVAL;
    if (newfd < 0) return -LX_EBADF;
    return lkd_fd_dup(t, oldfd, newfd, 0, (flags & LX_O_CLOEXEC) != 0);
}

/* ---- fcntl side state: owner, signal, write-life hint (no SIGIO is ever sent, see F_SETFL) -------- */

struct side { const lkd_fdobj *o; int32_t owner; int32_t owner_type; int32_t sig; uint64_t hint; struct side *next; };
static struct side *g_side;
static pthread_mutex_t g_side_lock = PTHREAD_MUTEX_INITIALIZER; /* leaf */

static struct side *side_get(const lkd_fdobj *o, int create)
{
    struct side *s;
    for (s = g_side; s; s = s->next) if (s->o == o) return s;
    if (!create) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = o;
    s->next = g_side;
    g_side = s;
    return s;
}

/* Called by lkd_fdobj_put when the object dies, so a new object at the same address starts clean. */
void fs_side_forget(const lkd_fdobj *o)
{
    pthread_mutex_lock(&g_side_lock);
    for (struct side **pp = &g_side; *pp; pp = &(*pp)->next)
        if ((*pp)->o == o) { struct side *d = *pp; *pp = d->next; free(d); break; }
    pthread_mutex_unlock(&g_side_lock);
}

static int64_t side_rw(lkd_fdobj *o, int which, int set, int64_t val)
{
    pthread_mutex_lock(&g_side_lock);
    struct side *s = side_get(o, set);
    int64_t r = 0;
    if (s) {
        if (which == 0) { if (set) s->owner = (int32_t)val; r = s->owner; }
        else if (which == 1) { if (set) s->sig = (int32_t)val; r = s->sig; }
        else if (which == 2) { if (set) s->owner_type = (int32_t)val; r = s->owner_type; }
        else { if (set) s->hint = (uint64_t)val; r = (int64_t)s->hint; }
    } else if (set) r = -LX_ENOMEM;
    pthread_mutex_unlock(&g_side_lock);
    return r;
}

/* ---- Record locks ------------------------------------------------------------------------------ */

static int lx_to_host_ltype(int16_t lt)
{
    switch (lt) { case LX_F_RDLCK: return F_RDLCK; case LX_F_WRLCK: return F_WRLCK; case LX_F_UNLCK: return F_UNLCK; }
    return -1;
}
static int16_t host_to_lx_ltype(short ht)
{
    switch (ht) { case F_RDLCK: return LX_F_RDLCK; case F_WRLCK: return LX_F_WRLCK; default: return LX_F_UNLCK; }
}

static int32_t host_pid_to_guest(pid_t hp)
{
    if (hp <= 0) return hp;
    lkd_proc_info *list = malloc(256 * sizeof *list);
    if (!list) return 0;
    int n = lkd_proc_list(list, 256);
    int32_t g = 0;   /* a holder outside the family is not visible: 0, like pid_vnr */
    for (int i = 0; i < n; i++) if (list[i].host_pid == hp) { g = list[i].pid; break; }
    free(list);
    return g;
}

static int64_t do_lock(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    struct lx_flock lf;
    if (lkd_user_read(arg, &lf, sizeof lf) < 0) return -LX_EFAULT;
    if (o->host_fd < 0) return -LX_EBADF;
    int ofd = cmd == LX_F_OFD_GETLK || cmd == LX_F_OFD_SETLK || cmd == LX_F_OFD_SETLKW;
    if (ofd && lf.l_pid != 0) return -LX_EINVAL;
    int ht = lx_to_host_ltype(lf.l_type);
    if (ht < 0) return -LX_EINVAL;
    if (lf.l_whence < 0 || lf.l_whence > 2) return -LX_EINVAL;
    uint32_t acc = fs_obj_access(o);
    int is_get = cmd == LX_F_GETLK || cmd == LX_F_OFD_GETLK;
    if (!is_get) {
        if (ht == F_RDLCK && acc == LX_O_WRONLY) return -LX_EBADF;
        if (ht == F_WRLCK && acc == LX_O_RDONLY) return -LX_EBADF;
    }
    struct flock hf;
    memset(&hf, 0, sizeof hf);
    hf.l_type = (short)ht;
    hf.l_whence = lf.l_whence;
    hf.l_start = lf.l_start;
    hf.l_len = lf.l_len;
    int hcmd;
    if (is_get) hcmd = ofd ? F_OFD_GETLK : F_GETLK;
    else hcmd = ofd ? F_OFD_SETLK : F_SETLK;
    int wait = cmd == LX_F_SETLKW || cmd == LX_F_OFD_SETLKW;
    for (;;) {
        if (fcntl(o->host_fd, hcmd, &hf) == 0) break;
        if (errno == EINTR) continue;
        if (wait && (errno == EAGAIN || errno == EACCES)) {
            /* blocking lock: retry interruptibly (the host F_SETLKW would sleep outside LKD) */
            int w = lkd_block_sleep(t, lkd_host_mono_ns() + 5000000);
            if (w < 0) return w;
            continue;
        }
        return LKD_HOST_ERR();
    }
    if (is_get) {
        lf.l_type = host_to_lx_ltype(hf.l_type);
        if (hf.l_type != F_UNLCK) {
            lf.l_whence = LX_SEEK_SET;
            lf.l_start = hf.l_start;
            lf.l_len = hf.l_len;
            lf.l_pid = ofd ? -1 : host_pid_to_guest(hf.l_pid);
        }
        if (lkd_user_write(arg, &lf, sizeof lf) < 0) return -LX_EFAULT;
    }
    return 0;
}

/* ---- fcntl ------------------------------------------------------------------------------------- */

#define LX_F_DUPFD_QUERY 1027

LKD_SYSCALL(fcntl)
{
    int fd = LKD_ARGI(0);
    uint32_t cmd = LKD_ARGUI(1);
    uint64_t arg = LKD_ARGU(2);
    int cloexec = 0;
    lkd_fdobj *o = lkd_fd_get(t, fd, &cloexec);
    if (!o) return -LX_EBADF;
    int64_t r;
    int is_path = fs_obj_is_path(o);
    if (is_path && cmd != LX_F_DUPFD && cmd != LX_F_DUPFD_CLOEXEC && cmd != LX_F_GETFD && cmd != LX_F_SETFD &&
        cmd != LX_F_GETFL && cmd != LX_F_DUPFD_QUERY) {
        lkd_fdobj_put(o);
        return -LX_EBADF;   /* O_PATH: fs/fcntl.c check_fcntl_cmd */
    }
    switch (cmd) {
    case LX_F_DUPFD: case LX_F_DUPFD_CLOEXEC: {
        int minfd = (int)(uint32_t)arg;
        if ((uint32_t)arg >= (uint32_t)fs_fd_limit(t)) r = -LX_EINVAL;
        else r = lkd_fd_alloc(t, o, minfd, cmd == LX_F_DUPFD_CLOEXEC);
        break;
    }
    case LX_F_GETFD: r = cloexec ? LX_FD_CLOEXEC : 0; break;
    case LX_F_SETFD: r = lkd_fd_set_cloexec(t, fd, (arg & LX_FD_CLOEXEC) != 0); break;
    case LX_F_GETFL: r = is_path ? (int64_t)(LX_O_PATH | (atomic_load(&o->status_flags) & LX_O_NOFOLLOW) | LX_O_LARGEFILE)
                                 : (int64_t)fs_obj_getfl(o); break;
    case LX_F_SETFL:
        if (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_stream_ops || o->ops == &fs_pipe_ops ||
            o->ops == &fs_memfd_ops) {
            r = fs_obj_setfl(t, o, (uint32_t)arg);
        } else {
            /* objects of other modules: record the status flags, then let the owner apply them */
            const uint32_t mask = LX_O_APPEND | LX_O_NONBLOCK | LX_O_DIRECT | LX_O_NOATIME;
            if (arg & LX_O_ASYNC) { r = -LX_ENOSYS; lkd_log_once("fs: O_ASYNC on %s: SIGIO not implemented", o->ops ? o->ops->name : "?"); break; }
            uint32_t fl = atomic_load(&o->status_flags);
            atomic_store(&o->status_flags, (fl & ~mask) | ((uint32_t)arg & mask));
            r = 0;
            if (o->ops && o->ops->fcntl) {
                int64_t x = o->ops->fcntl(t, o, LX_F_SETFL, arg);
                if (x < 0 && x != -LX_EINVAL) r = x;
            }
        }
        break;
    case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:
    case LX_F_OFD_GETLK: case LX_F_OFD_SETLK: case LX_F_OFD_SETLKW:
        r = do_lock(t, o, cmd, arg);
        break;
    case LX_F_GETOWN: r = side_rw(o, 0, 0, 0); break;
    case LX_F_SETOWN: r = side_rw(o, 0, 1, (int32_t)arg); if (r >= 0) { side_rw(o, 2, 1, 0); r = 0; } break;
    case LX_F_GETOWN_EX: {
        struct { int32_t type; int32_t pid; } ex = { (int32_t)side_rw(o, 2, 0, 0), (int32_t)side_rw(o, 0, 0, 0) };
        if (ex.type == 0) ex.type = ex.pid < 0 ? 2 /* F_OWNER_PGRP */ : 1 /* F_OWNER_PID */;
        if (ex.pid < 0) ex.pid = -ex.pid;
        r = lkd_user_write(arg, &ex, sizeof ex) < 0 ? -LX_EFAULT : 0;
        break;
    }
    case LX_F_SETOWN_EX: {
        struct { int32_t type; int32_t pid; } ex;
        if (lkd_user_read(arg, &ex, sizeof ex) < 0) { r = -LX_EFAULT; break; }
        if (ex.type < 0 || ex.type > 2) { r = -LX_EINVAL; break; }
        side_rw(o, 0, 1, ex.type == 2 ? -ex.pid : ex.pid);
        side_rw(o, 2, 1, ex.type);
        r = 0;
        break;
    }
    case LX_F_GETSIG: r = side_rw(o, 1, 0, 0); break;
    case LX_F_SETSIG:
        if (arg > LX_NSIG) r = -LX_EINVAL;
        else { r = side_rw(o, 1, 1, (int64_t)arg); if (r >= 0) r = 0; }
        break;
    case LX_F_GETPIPE_SZ: case LX_F_SETPIPE_SZ:
        r = o->ops == &fs_pipe_ops ? o->ops->fcntl(t, o, cmd, arg) : -LX_EBADF;
        break;
    case LX_F_ADD_SEALS: case LX_F_GET_SEALS:
        r = fs_memfd_fcntl_seals(t, o, cmd, arg);
        break;
    case LX_F_GETLEASE: r = LX_F_UNLCK; break;   /* no lease is ever held */
    case LX_F_SETLEASE:
        /* Leases need lease-break signals; refused like a kernel with fs.leases-enable=0 (-EINVAL). */
        r = arg == LX_F_UNLCK ? 0 : -LX_EINVAL;
        break;
    case LX_F_NOTIFY: r = -LX_EINVAL; break;      /* dnotify disabled (fs.dir-notify-enable=0); inotify exists */
    case LX_F_GET_RW_HINT: {
        uint64_t h = (uint64_t)side_rw(o, 3, 0, 0);
        r = lkd_user_write(arg, &h, sizeof h) < 0 ? -LX_EFAULT : 0;
        break;
    }
    case LX_F_SET_RW_HINT: {
        uint64_t h;
        if (lkd_user_read(arg, &h, sizeof h) < 0) { r = -LX_EFAULT; break; }
        if (h > 5) { r = -LX_EINVAL; break; }
        side_rw(o, 3, 1, (int64_t)h);
        r = 0;
        break;
    }
    case LX_F_DUPFD_QUERY: {
        lkd_fdobj *q = lkd_fd_get(t, (int)(uint32_t)arg, NULL);
        r = q ? (q == o) : -LX_EBADF;
        lkd_fdobj_put(q);
        break;
    }
    default:
        r = (o->ops && o->ops->fcntl) ? o->ops->fcntl(t, o, cmd, arg) : -LX_EINVAL;
        break;
    }
    lkd_fdobj_put(o);
    return r;
}

/* ---- flock ------------------------------------------------------------------------------------- */

LKD_SYSCALL(flock)
{
    int op = LKD_ARGI(1);
    int kind = op & ~LX_LOCK_NB;
    if (kind != LX_LOCK_SH && kind != LX_LOCK_EX && kind != LX_LOCK_UN) return -LX_EINVAL;
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r = 0;
    if (fs_obj_is_path(o) || o->host_fd < 0) { r = -LX_EBADF; goto out; }
    int hop = kind == LX_LOCK_SH ? LOCK_SH : kind == LX_LOCK_EX ? LOCK_EX : LOCK_UN;
    for (;;) {
        if (flock(o->host_fd, hop | LOCK_NB) == 0) break;
        if (errno == EINTR) continue;
        if (errno == EWOULDBLOCK && !(op & LX_LOCK_NB) && kind != LX_LOCK_UN) {
            int w = lkd_block_sleep(t, lkd_host_mono_ns() + 5000000);
            if (w < 0) { r = w; break; }
            continue;
        }
        r = LKD_HOST_ERR();
        break;
    }
out:
    lkd_fdobj_put(o);
    return r;
}

/* ---- ioctl ------------------------------------------------------------------------------------- */

#define LX_FIOQSIZE 0x5460
#define LX_FS_IOC_GETFLAGS 0x80086601 /* _IOR('f', 1, long); the kernel reads and writes an int */
#define LX_FS_CASEFOLD_FL 0x40000000

/* FS_IOC_GETFLAGS (ioctl_getflags): a host file or directory has none of the ext2 attribute flags, except casefold:
 * a directory of a case-insensitive host volume (APFS by default) is a casefolded directory, and its lookups are
 * already case-insensitive. Wine reads this flag before listing a directory for each name it does not find as
 * written. Virtual directories (procfs, mount points) answer ENOTTY, as procfs does. */
static int64_t getflags(lkd_fdobj *o, uint64_t arg)
{
    if (o->ops != &fs_file_ops && o->ops != &fs_dir_ops) return -LX_ENOTTY;
    if (o->host_fd < 0) return -LX_ENOTTY;
    uint32_t fl = 0;
    if (o->ops == &fs_dir_ops && fpathconf(o->host_fd, _PC_CASE_SENSITIVE) == 0) fl |= LX_FS_CASEFOLD_FL;
    return lkd_user_write(arg, &fl, sizeof fl) < 0 ? -LX_EFAULT : 0;
}

LKD_SYSCALL(ioctl)
{
    int fd = LKD_ARGI(0);
    uint32_t cmd = LKD_ARGUI(1);
    uint64_t arg = LKD_ARGU(2);
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int64_t r;
    if (fs_obj_is_path(o)) { lkd_fdobj_put(o); return -LX_EBADF; }
    switch (cmd) {
    case LX_FIOCLEX: r = lkd_fd_set_cloexec(t, fd, 1); break;
    case LX_FIONCLEX: r = lkd_fd_set_cloexec(t, fd, 0); break;
    case LX_FIONBIO: case LX_FIOASYNC: {
        int32_t on;
        if (lkd_user_read(arg, &on, sizeof on) < 0) { r = -LX_EFAULT; break; }
        uint32_t bit = cmd == LX_FIONBIO ? LX_O_NONBLOCK : LX_O_ASYNC;
        uint32_t fl = fs_obj_getfl(o);
        fl = on ? (fl | bit) : (fl & ~bit);
        if (o->ops == &fs_file_ops || o->ops == &fs_dir_ops || o->ops == &fs_stream_ops || o->ops == &fs_pipe_ops ||
            o->ops == &fs_memfd_ops) r = fs_obj_setfl(t, o, fl);
        else if (bit == LX_O_ASYNC && on) r = -LX_ENOSYS;
        else {
            uint32_t sf = atomic_load(&o->status_flags);
            atomic_store(&o->status_flags, on ? (sf | bit) : (sf & ~bit));
            r = 0;
            if (o->ops && o->ops->fcntl) { int64_t x = o->ops->fcntl(t, o, LX_F_SETFL, fl); if (x < 0 && x != -LX_EINVAL) r = x; }
        }
        break;
    }
    case LX_FIONREAD:
        if (o->ops == &fs_file_ops || o->ops == &fs_memfd_ops) {
            /* regular file: bytes between the position and the end (do_vfs_ioctl) */
            struct stat st;
            int64_t pos = o->ops->lseek(t, o, 0, LX_SEEK_CUR);
            if (fstat(o->host_fd, &st) < 0) { r = LKD_HOST_ERR(); break; }
            int32_t v = (int32_t)(st.st_size - (pos < 0 ? 0 : pos));
            r = lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
            break;
        }
        r = (o->ops && o->ops->ioctl) ? o->ops->ioctl(t, o, cmd, arg) : -LX_ENOTTY;
        break;
    case LX_FS_IOC_GETFLAGS:
        r = getflags(o, arg);
        if (r == -LX_ENOTTY && o->ops && o->ops->ioctl) r = o->ops->ioctl(t, o, cmd, arg);
        break;
    case LX_FIOQSIZE:
        if (o->ops == &fs_file_ops || o->ops == &fs_memfd_ops || o->ops == &fs_dir_ops) {
            struct lx_stat st;
            r = fs_obj_stat(t, o, &st);
            if (r == 0) { int64_t v = st.st_size; r = lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0; }
            break;
        }
        r = -LX_ENOTTY;
        break;
    default:
        r = (o->ops && o->ops->ioctl) ? o->ops->ioctl(t, o, cmd, arg) : -LX_ENOTTY;
        break;
    }
    if (r == -LX_ENOIOCTLCMD) r = -LX_ENOTTY;
    lkd_fdobj_put(o);
    return r;
}

/* ---- Size, allocation, sync -------------------------------------------------------------------- */

LKD_SYSCALL(ftruncate)
{
    int64_t len = LKD_ARGL(1);
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r;
    uint32_t acc = fs_obj_access(o);
    if (fs_obj_is_path(o)) r = -LX_EBADF;
    else if (len < 0) r = -LX_EINVAL;
    else if (!(o->kind == LKD_FD_FILE || o->kind == LKD_FD_MEMFD) || !o->ops->ftruncate) r = -LX_EINVAL;
    else if (acc != LX_O_WRONLY && acc != LX_O_RDWR) r = -LX_EINVAL;
    else r = o->ops->ftruncate(t, o, len);
    lkd_fdobj_put(o);
    return r;
}

#define LX_FALLOC_FL_KEEP_SIZE 0x01
#define LX_FALLOC_FL_PUNCH_HOLE 0x02

static int host_punch(int fd, int64_t off, int64_t len)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return (int)LKD_HOST_ERR();
    int64_t end = off + len > st.st_size ? st.st_size : off + len;
    if (end <= off) return 0;
    struct fpunchhole ph = { 0, 0, (off_t)off, (off_t)(end - off) };
    if (fcntl(fd, F_PUNCHHOLE, &ph) == 0) return 0;
    /* unaligned range or no support: write zeros (reads return zeros, as after a punch) */
    static const char zeros[16384];
    for (int64_t p = off; p < end;) {
        size_t n = (size_t)(end - p > (int64_t)sizeof zeros ? (int64_t)sizeof zeros : end - p);
        ssize_t w = pwrite(fd, zeros, n, p);
        if (w < 0) return (int)LKD_HOST_ERR();
        p += w;
    }
    return 0;
}

LKD_SYSCALL(fallocate)
{
    int mode = LKD_ARGI(1);
    int64_t off = LKD_ARGL(2), len = LKD_ARGL(3);
    if (off < 0 || len <= 0) return -LX_EINVAL;
    if (off > INT64_MAX - len) return -LX_EFBIG;
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r = 0;
    uint32_t acc = fs_obj_access(o);
    if (fs_obj_is_path(o) || (acc != LX_O_WRONLY && acc != LX_O_RDWR)) { r = -LX_EBADF; goto out; }
    if (o->kind == LKD_FD_DIR) { r = -LX_EISDIR; goto out; }
    if (o->kind == LKD_FD_PIPE) { r = -LX_ESPIPE; goto out; }
    if ((mode & LX_FALLOC_FL_PUNCH_HOLE) && !(mode & LX_FALLOC_FL_KEEP_SIZE)) { r = -LX_EOPNOTSUPP; goto out; }
    if (o->kind == LKD_FD_MEMFD) { r = fs_memfd_fallocate(o, mode, off, len); goto out; }
    if (o->kind != LKD_FD_FILE || o->host_fd < 0) { r = -LX_ENODEV; goto out; }
    if (mode & ~(LX_FALLOC_FL_KEEP_SIZE | LX_FALLOC_FL_PUNCH_HOLE)) { r = -LX_EOPNOTSUPP; goto out; }
    if (mode & LX_FALLOC_FL_PUNCH_HOLE) { r = host_punch(o->host_fd, off, len); goto out; }
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) { r = LKD_HOST_ERR(); goto out; }
    if (off + len > st.st_size) {
        /* reserve space past EOF (best effort: APFS may still report ENOSPC later, documented) */
        fstore_t fs = { F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t)(off + len - st.st_size), 0 };
        if (fcntl(o->host_fd, F_PREALLOCATE, &fs) < 0 && errno == ENOSPC) { r = -LX_ENOSPC; goto out; }
        if (!(mode & LX_FALLOC_FL_KEEP_SIZE) && ftruncate(o->host_fd, off + len) < 0) r = LKD_HOST_ERR();
    }
out:
    lkd_fdobj_put(o);
    return r;
}

static int64_t do_fsync(lkd_task *t, int fd, int datasync)
{
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int64_t r = fs_obj_is_path(o) ? -LX_EBADF : (o->ops && o->ops->fsync) ? o->ops->fsync(t, o, datasync) : -LX_EINVAL;
    lkd_fdobj_put(o);
    return r;
}
LKD_SYSCALL(fsync) { return do_fsync(t, LKD_ARGI(0), 0); }
LKD_SYSCALL(fdatasync) { return do_fsync(t, LKD_ARGI(0), 1); }
LKD_SYSCALL(sync) { (void)t; (void)rq; sync(); return 0; }
LKD_SYSCALL(syncfs)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int path = fs_obj_is_path(o);
    lkd_fdobj_put(o);
    if (path) return -LX_EBADF;
    sync();   /* the host syncs every file system: a superset of the one asked for */
    return 0;
}

/* fadvise64(2): advisory; WILLNEED becomes a host read-ahead (F_RDADVISE). Pipes -ESPIPE. */
LKD_SYSCALL(fadvise64)
{
    int64_t off = LKD_ARGL(1), len = LKD_ARGL(2);
    int adv = LKD_ARGI(3);
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r = 0;
    if (fs_obj_is_path(o)) r = -LX_EBADF;
    else if (o->kind == LKD_FD_PIPE) r = -LX_ESPIPE;
    else if (adv < 0 || adv > 5 || len < 0) r = -LX_EINVAL;
    else if (adv == 3 && o->kind == LKD_FD_FILE && o->host_fd >= 0) {
        struct radvisory ra = { (off_t)off, (int)(len > 0x7fffffff || len == 0 ? 0x7fffffff : len) };
        (void)fcntl(o->host_fd, F_RDADVISE, &ra);
    }
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(readahead)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r = 0;
    uint32_t acc = fs_obj_access(o);
    if (fs_obj_is_path(o) || acc == LX_O_WRONLY) r = -LX_EBADF;
    else if (o->kind != LKD_FD_FILE || o->host_fd < 0) r = -LX_EINVAL;
    else {
        uint64_t cnt = LKD_ARGU(2);
        struct radvisory ra = { (off_t)LKD_ARGL(1), (int)(cnt > 0x7fffffff ? 0x7fffffff : cnt) };
        (void)fcntl(o->host_fd, F_RDADVISE, &ra);
    }
    lkd_fdobj_put(o);
    return r;
}

LKD_SYSCALL(sync_file_range)
{
    int64_t off = LKD_ARGL(1), n = LKD_ARGL(2);
    uint32_t flags = LKD_ARGUI(3);
    if (flags & ~7u) return -LX_EINVAL;
    if (off < 0 || n < 0 || (n > 0 && off > INT64_MAX - n)) return -LX_EINVAL;
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    int64_t r = 0;
    if (fs_obj_is_path(o)) r = -LX_EBADF;
    else if (o->kind == LKD_FD_PIPE || o->kind == LKD_FD_SOCKET) r = -LX_ESPIPE;
    else if ((flags & 2u) && o->host_fd >= 0 && fsync(o->host_fd) < 0) r = LKD_HOST_ERR();
    lkd_fdobj_put(o);
    return r;
}

/* ---- getdents ---------------------------------------------------------------------------------- */

static int64_t do_getdents(lkd_task *t, int fd, uint64_t buf, uint64_t count, int legacy)
{
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    int64_t r;
    if (fs_obj_is_path(o)) r = -LX_EBADF;
    else if (!o->ops || !o->ops->getdents) r = -LX_ENOTDIR;
    else r = o->ops->getdents(t, o, buf, count, legacy);
    lkd_fdobj_put(o);
    return r;
}
LKD_SYSCALL(getdents) { return do_getdents(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), 1); }
LKD_SYSCALL(getdents64) { return do_getdents(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGUI(2), 0); }

/* ---- File handles ------------------------------------------------------------------------------ */

/* Host directories export no file handles: -EOPNOTSUPP after the path lookup (fs/fhandle.c). */
LKD_SYSCALL(name_to_handle_at)
{
    int flags = LKD_ARGI(4);
    if (flags & ~(LX_AT_SYMLINK_FOLLOW | LX_AT_EMPTY_PATH | 0x200 /* AT_HANDLE_FID */)) return -LX_EINVAL;
    char path[LX_PATH_MAX];
    int n = fs_get_path(LKD_ARGP(1), path);
    if (n < 0) return n;
    if (n == 0 && !(flags & LX_AT_EMPTY_PATH)) return -LX_ENOENT;
    if (n > 0) {
        struct fs_res *r = malloc(sizeof *r);
        if (!r) return -LX_ENOMEM;
        int rc = fs_resolve(t, LKD_ARGI(0), path, (flags & LX_AT_SYMLINK_FOLLOW) ? FS_R_FOLLOW | FS_R_NOMAGIC : 0, r);
        free(r);
        if (rc < 0) return rc;
    }
    return -LX_EOPNOTSUPP;
}

/* Needs CAP_DAC_READ_SEARCH (-EPERM); no handle was ever issued, so a root caller gets -ESTALE. */
LKD_SYSCALL(open_by_handle_at)
{
    (void)rq;
    return (t->proc && t->proc->cred.euid == 0) ? -LX_ESTALE : -LX_EPERM;
}

void fs_register_misc(void)
{
    LKD_REGISTER(dup); LKD_REGISTER(dup2); LKD_REGISTER(dup3);
    LKD_REGISTER(fcntl); LKD_REGISTER(flock); LKD_REGISTER(ioctl);
    LKD_REGISTER(ftruncate); LKD_REGISTER(fallocate);
    LKD_REGISTER(fsync); LKD_REGISTER(fdatasync); LKD_REGISTER(sync); LKD_REGISTER(syncfs);
    LKD_REGISTER(fadvise64); LKD_REGISTER(readahead); LKD_REGISTER(sync_file_range);
    LKD_REGISTER(getdents); LKD_REGISTER(getdents64);
    LKD_REGISTER(name_to_handle_at); LKD_REGISTER(open_by_handle_at);
}

/* fork child: the leaf lock may have been held by another thread of the parent (lkd_fork_reinit_mutex). */
void fs_side_lock_fork_child(void) { lkd_fork_reinit_mutex(&g_side_lock, 0); }
