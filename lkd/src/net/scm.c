/* scm.c: LKD objects across processes through SCM_RIGHTS (net/core/scm.c semantics: the receiver gets
 * a new descriptor on the same open file description).
 *
 * Sending: an object is carried by its host descriptor (files, directories, pipes, sockets, memfd, host
 * shm). An eventfd is carried by three host descriptors (its shared counter region and both ends of
 * its readiness pipe). kqueue-backed objects (epoll, timerfd, inotify) cannot cross processes (MESURE:
 * sendmsg of a kqueue descriptor gives EINVAL) and objects without a host descriptor (signalfd, virtual
 * files) have nothing to carry: sendmsg answers -EOPNOTSUPP for them, visibly.
 *
 * Receiving: each host descriptor is rebuilt from what fstat says. A socket becomes a socket object
 * (domain from getsockname, type from SO_TYPE); a POSIX shm object (no file type bits, MESURE) becomes a
 * mappable shared-memory file object, or an eventfd when it carries the eventfd magic; anything else
 * goes to the fs module (regular file, directory, FIFO, character device). The host O_NONBLOCK of a
 * received description is shared with the sender and left untouched.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "net_priv.h"
#include "lkd_fs.h"

/* ---- host shared memory object ----------------------------------------------------------------- */

/* A POSIX shm descriptor (wsd and vtest send them): mappable MAP_SHARED at 16 KiB granularity; read and
 * write are refused by the host (MESURE: ENXIO), and so they are for the guest. */
static int shm_mmap_fd(lkd_fdobj *o) { return o->host_fd; }

static int shm_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) return (int)LKD_HOST_ERR();
    memset(out, 0, sizeof *out);
    out->st_dev = o->dev;
    out->st_ino = o->ino;
    out->st_nlink = 0;                           /* unlinked shmem file */
    out->st_mode = LX_S_IFREG | (st.st_mode & 07777);
    out->st_uid = t && t->proc ? t->proc->cred.euid : 0;
    out->st_gid = t && t->proc ? t->proc->cred.egid : 0;
    out->st_size = st.st_size;
    out->st_blksize = 4096;
    out->st_blocks = (st.st_size + 511) / 512;
    return 0;
}

static int64_t shm_rw(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    (void)t; (void)o; (void)gva; (void)count; (void)off;
    return -LX_ENXIO;
}

static int shm_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "/dev/shm/host-shm.%llu (deleted)", (unsigned long long)o->ino);
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static int64_t shm_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)arg;
    return cmd == LX_F_SETFL ? 0 : -LX_EINVAL;
}

static const lkd_fd_ops net_hostshm_ops = {
    .name = "host-shm",
    .read = shm_rw,
    .write = shm_rw,
    .fcntl = shm_fcntl,
    .fstat = shm_fstat,
    .mmap_fd = shm_mmap_fd,
    .describe = shm_describe,
};

static int wrap_shm(int fd, lkd_fdobj **out)
{
    int fl = fcntl(fd, F_GETFL);
    uint32_t acc = fl < 0 ? LX_O_RDWR : ((fl & O_ACCMODE) == O_RDONLY ? LX_O_RDONLY : (fl & O_ACCMODE) == O_WRONLY ? LX_O_WRONLY : LX_O_RDWR);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_FILE, &net_hostshm_ops, fd, acc, NULL);
    if (!o) return -LX_ENOMEM;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    o->dev = 0x17;                               /* tmpfs-like device number for /dev/shm */
    *out = o;
    return 0;
}

/* ---- export ------------------------------------------------------------------------------------ */

int net_scm_export(lkd_fdobj *o, int *hfds, int cap)
{
    if (o->ops == &net_eventfd_ops) {
        if (cap < 3) return -LX_ETOOMANYREFS;
        return net_eventfd_export(o, hfds);
    }
    if (o->ops == &net_epoll_ops || o->ops == &net_timerfd_ops || o->ops == &net_inotify_ops || o->host_fd < 0) {
        lkd_log_once("net: SCM_RIGHTS of a %s object cannot cross host processes (-EOPNOTSUPP)", o->ops ? o->ops->name : "?");
        return -LX_EOPNOTSUPP;
    }
    if (cap < 1) return -LX_ETOOMANYREFS;
    int pin = lkd_fs_pin_raw(o);   /* the receiver sees the raw host file (request wine-7) */
    if (pin < 0) return pin;
    hfds[0] = fcntl(o->host_fd, F_DUPFD_CLOEXEC, 0);
    if (hfds[0] < 0) return (int)LKD_HOST_ERR();
    return 1;
}

/* ---- import ------------------------------------------------------------------------------------ */

static int import_socket(int fd, lkd_fdobj **out)
{
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    memset(&ss, 0, sizeof ss);
    int dom = LX_AF_UNIX;
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) == 0 && ss.ss_family) {
        int d = lkd_af_from_host(ss.ss_family);
        if (d >= 0) dom = d;
    }
    int ty = SOCK_STREAM;
    socklen_t tl = sizeof ty;
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &ty, &tl);
    int lxty = ty == SOCK_DGRAM ? LX_SOCK_DGRAM : ty == SOCK_SEQPACKET ? LX_SOCK_SEQPACKET : ty == SOCK_RAW ? LX_SOCK_RAW : LX_SOCK_STREAM;
    int proto = dom == LX_AF_UNIX ? 0 : (lxty == LX_SOCK_STREAM ? LX_IPPROTO_TCP : LX_IPPROTO_UDP);
    int fl = fcntl(fd, F_GETFL);
    uint32_t nb = (fl >= 0 && (fl & O_NONBLOCK)) ? LX_O_NONBLOCK : 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int e = net_sock_wrap(fd, dom, lxty, proto, nb, 0, out);
    if (e == 0) {
        int acc = 0;
        socklen_t al = sizeof acc;
        if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &acc, &al) == 0 && acc) atomic_store(&net_sk(*out)->listening, 1);
    }
    return e;
}

int net_scm_import(const int *hfds, int n, lkd_fdobj **objs, int cap)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        int fd = hfds[i];
        lkd_fdobj *o = NULL;
        int e = -LX_EBADF;
        struct stat st;
        if (k >= cap || fstat(fd, &st) < 0) { close(fd); continue; }
        if (S_ISSOCK(st.st_mode)) {
            e = import_socket(fd, &o);
        } else if ((st.st_mode & S_IFMT) == 0) {
            if (i + 2 < n && net_eventfd_is_region(fd)) {
                struct stat a, b;
                if (fstat(hfds[i + 1], &a) == 0 && fstat(hfds[i + 2], &b) == 0 && S_ISFIFO(a.st_mode) && S_ISFIFO(b.st_mode)) {
                    e = net_eventfd_from_host(fd, hfds[i + 1], hfds[i + 2], &o);
                    if (e == 0) { i += 2; objs[k++] = o; continue; }
                }
            }
            e = wrap_shm(fd, &o);
        } else {
            int fl = fcntl(fd, F_GETFL);
            uint32_t acc = fl < 0 ? LX_O_RDWR : ((fl & O_ACCMODE) == O_RDONLY ? LX_O_RDONLY : (fl & O_ACCMODE) == O_WRONLY ? LX_O_WRONLY : LX_O_RDWR);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            e = lkd_fs_wrap_host_fd(fd, acc | LX_O_LARGEFILE, NULL, &o);
        }
        if (e < 0) { close(fd); continue; }
        objs[k++] = o;
    }
    return k;
}
