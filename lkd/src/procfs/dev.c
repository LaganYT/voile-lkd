/* dev.c: the character devices of /dev and their file objects.
 *
 * Linux references: drivers/char/mem.c (null, zero, full, random, urandom: read_null, write_null,
 * read_zero, write_full, mmap_zero), drivers/char/random.c (urandom_read_iter, random_write_iter),
 * Documentation/admin-guide/devices.txt (numbers).
 *
 * Supported: null, zero, full, random, urandom (generated here from the host's getentropy), tty and
 * console (the host controlling terminal, when there is one). NOT supported: pseudo terminals.
 * /dev/ptmx exists with the Linux mode and numbers but opening it answers -ENODEV, the answer of a
 * kernel without a devpts driver; /dev/pts is an empty directory. The fs module's tty support covers host
 * terminals only (docs/status/procfs.md). /dev/shm is mounted by the fs module. /dev/ntsync (misc 10:126,
 * drivers/misc/ntsync.c) is the sig module's (ntsync.h); it exists while lkd_ntsync_available().
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntsync.h"
#include "procfs_priv.h"

enum { D_NULL, D_ZERO, D_FULL, D_RANDOM, D_URANDOM, D_TTY, D_CONSOLE, D_PTMX, D_NTSYNC, D_N };
struct dev { const char *name; uint16_t perm; uint8_t major, minor; };
static const struct dev g_dev[D_N] = {
    [D_NULL] = { "null", 0666, 1, 3 }, [D_ZERO] = { "zero", 0666, 1, 5 }, [D_FULL] = { "full", 0666, 1, 7 },
    [D_RANDOM] = { "random", 0666, 1, 8 }, [D_URANDOM] = { "urandom", 0666, 1, 9 },
    [D_TTY] = { "tty", 0666, 5, 0 }, [D_CONSOLE] = { "console", 0600, 5, 1 }, [D_PTMX] = { "ptmx", 0666, 5, 2 },
    [D_NTSYNC] = { "ntsync", 0666, 10, 126 },
};
struct dlnk { const char *name, *target; };
static const struct dlnk g_lnk[] = {
    { "fd", "/proc/self/fd" }, { "stdin", "/proc/self/fd/0" }, { "stdout", "/proc/self/fd/1" },
    { "stderr", "/proc/self/fd/2" },
};
#define N_LNK (int)(sizeof g_lnk / sizeof g_lnk[0])
static const struct pf_tent g_pts = { "pts", 'd', 0755, 0, NULL, NULL, NULL };

/* Entries in listing order: devices, links, pts. */
#define N_ENT (D_N + N_LNK + 1)

static uint64_t dev_ino(int idx) { return 0x100 + (uint64_t)idx; }

int pf_dev_lookup(const char *name, struct pf_node *n)
{
    int64_t boot = pf_boot_realtime_sec();
    n->mtime = boot;
    n->tent = NULL;
    n->magic = 0;
    n->size = 0;
    for (int i = 0; i < D_N; i++) {
        if (strcmp(g_dev[i].name, name)) continue;
        if (i == D_NTSYNC && !lkd_ntsync_available()) return -LX_ENOENT;
        n->kind = PK_DEVNODE;
        n->sub = i;
        n->mode = LX_S_IFCHR | g_dev[i].perm;
        n->nlink = 1;
        n->rdev = ((uint64_t)g_dev[i].major << 8) | g_dev[i].minor;
        n->ino = dev_ino(i);
        return 0;
    }
    for (int i = 0; i < N_LNK; i++) {
        if (strcmp(g_lnk[i].name, name)) continue;
        n->kind = PK_DEVLNK;
        n->sub = i;
        n->mode = LX_S_IFLNK | 0777;
        n->nlink = 1;
        n->ino = dev_ino(D_N + i);
        return 0;
    }
    if (!strcmp(name, "pts")) {
        n->kind = PK_SDIR;
        n->tent = &g_pts;
        n->mode = LX_S_IFDIR | 0755;
        n->nlink = 2;
        n->ino = dev_ino(D_N + N_LNK);
        return 0;
    }
    return -LX_ENOENT;
}

int pf_dev_readdir(uint64_t i, char name[256], uint8_t *dt, uint64_t *ino)
{
    if (i >= D_NTSYNC && !lkd_ntsync_available()) i++; /* no /dev/ntsync: the entries after it move up */
    if (i >= N_ENT) return 0;
    if (i < D_N) { snprintf(name, 256, "%s", g_dev[i].name); *dt = LX_DT_CHR; }
    else if (i < (uint64_t)(D_N + N_LNK)) { snprintf(name, 256, "%s", g_lnk[i - D_N].name); *dt = LX_DT_LNK; }
    else { snprintf(name, 256, "pts"); *dt = LX_DT_DIR; }
    *ino = dev_ino((int)i);
    return 1;
}

int64_t pf_dev_readlink(const struct pf_node *n, char *buf, size_t cap)
{
    if (n->sub < 0 || n->sub >= N_LNK) return -LX_EINVAL;
    size_t l = strlen(g_lnk[n->sub].target);
    if (l > cap) l = cap;
    memcpy(buf, g_lnk[n->sub].target, l);
    return (int64_t)l;
}

/* ---- Objects ----------------------------------------------------------------------------------- */

/* Host twins: the macOS device that behaves like the Linux one. An object may hold a descriptor on its twin, one
 * that arrived from another host process or one opened the first time the object is sent. The twin carries the
 * open file description through SCM_RIGHTS: another LKD process turns it back into this device
 * (lkd_procfs_dev_adopt), any other host process gets a working device. Reads and writes stay in LKD: macOS
 * refuses writes to /dev/urandom (MESURE: EPERM), its devices cannot be polled (MESURE: poll gives POLLNVAL, kevent
 * EINVAL), and the Linux rules (MAX_RW_COUNT, a signal ending a long urandom read) are kept here. /dev/full has no
 * macOS twin and an O_PATH descriptor has no host form: both travel as a record (LKD_FDREC_DEV). */
static const char *const g_twin[D_N] = {
    [D_NULL] = "/dev/null", [D_ZERO] = "/dev/zero", [D_RANDOM] = "/dev/random", [D_URANDOM] = "/dev/urandom",
};
static dev_t g_twin_rdev[D_N];
static pthread_once_t g_twin_once = PTHREAD_ONCE_INIT;

static void twin_init(void)
{
    for (int i = 0; i < D_N; i++) {
        struct stat st;
        g_twin_rdev[i] = (dev_t)-1;
        if (g_twin[i] && stat(g_twin[i], &st) == 0 && S_ISCHR(st.st_mode)) g_twin_rdev[i] = st.st_rdev;
    }
}

struct devobj { int sub; int twin; };   /* twin: host descriptor on the host twin, or -1 */

static int dev_of(lkd_fdobj *o) { return ((const struct devobj *)o->priv)->sub; }

#define CHUNK 65536u

static int64_t zero_fill(uint64_t gva, uint64_t count)
{
    static const char z[1] = { 0 };
    (void)z;
    if (count > 0x7ffff000u) count = 0x7ffff000u;   /* MAX_RW_COUNT */
    uint64_t done = 0;
    while (done < count) {
        uint64_t c = count - done > CHUNK ? CHUNK : count - done;
        if (lkd_user_zero(gva + done, c) < 0) return done ? (int64_t)done : -LX_EFAULT;
        done += c;
    }
    return (int64_t)done;
}

static int64_t dev_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{
    switch (dev_of(o)) {
    case D_NULL: return 0;
    case D_ZERO: case D_FULL: return zero_fill(gva, count);
    case D_RANDOM: case D_URANDOM: {
        if (count > 0x7ffff000u) count = 0x7ffff000u;
        uint8_t buf[256];
        uint64_t done = 0;
        while (done < count) {
            size_t c = count - done > sizeof buf ? sizeof buf : (size_t)(count - done);
            if (getentropy(buf, c) != 0) return done ? (int64_t)done : LKD_HOST_ERR();
            if (lkd_user_write(gva + done, buf, c) < 0) return done ? (int64_t)done : -LX_EFAULT;
            done += c;
            if (done >= 4096 && lkd_sig_interrupted(t)) break;   /* urandom_read_iter: stops for a pending signal */
        }
        return (int64_t)done;
    }
    }
    (void)offset;
    return -LX_EINVAL;
}

static int64_t dev_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{
    (void)t; (void)offset;
    switch (dev_of(o)) {
    case D_NULL: return (int64_t)count;
    case D_FULL: return -LX_ENOSPC;
    case D_ZERO: return (int64_t)count;   /* write_null */
    case D_RANDOM: case D_URANDOM: {
        /* random_write_iter mixes the data into the pool; the host kernel owns its pool, the bytes are
         * validated (EFAULT) and dropped. Linux reports them as consumed. */
        uint8_t buf[256];
        uint64_t done = 0;
        while (done < count) {
            size_t c = count - done > sizeof buf ? sizeof buf : (size_t)(count - done);
            if (lkd_user_read(gva + done, buf, c) < 0) return done ? (int64_t)done : -LX_EFAULT;
            done += c;
        }
        return (int64_t)done;
    }
    }
    return -LX_EINVAL;
}

/* null_lseek: the position is forced to 0 */
static int64_t dev_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    (void)t; (void)o; (void)off; (void)whence;
    return 0;
}

static uint32_t dev_poll(lkd_fdobj *o, uint32_t want) { (void)o; return want & (LX_POLLIN | LX_POLLOUT); }

static int dev_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct pf_node n;
    memset(&n, 0, sizeof n);
    n.mnt = PF_DEV;
    if (pf_dev_lookup(g_dev[dev_of(o)].name, &n) < 0) return -LX_EBADF;
    pf_stat_node(t, &n, out);
    return 0;
}

static int dev_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    int n = snprintf(buf, cap, "/dev/%s", g_dev[dev_of(o)].name);
    return n < 0 ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

static void dev_release(lkd_fdobj *o)
{
    struct devobj *d = o->priv;
    if (!d) return;
    if (d->twin >= 0) close(d->twin);
    free(d);
    o->priv = NULL;
}

/* SCM_RIGHTS: a descriptor on the host twin, opened with the guest's access mode and carrying its O_NONBLOCK and
 * O_APPEND (the receiver reads them from the host description); a record when there is no twin. */
static int dev_export(lkd_fdobj *o, int *hfds, int cap)
{
    (void)cap;
    struct devobj *d = o->priv;
    uint32_t fl = atomic_load(&o->status_flags);
    if (g_twin[d->sub] && !(fl & LX_O_PATH)) {
        int rc = 0;
        pthread_mutex_lock(&o->lock);
        if (d->twin < 0) {
            int hflags = 0;
            uint32_t un = 0;
            if (lkd_open_flags_to_host(fl & LX_O_ACCMODE, &hflags, &un) == 0) d->twin = open(g_twin[d->sub], hflags | O_CLOEXEC);
        }
        if (d->twin >= 0) {
            int h = fcntl(d->twin, F_GETFL);
            if (h >= 0) {
                h &= ~(O_NONBLOCK | O_APPEND);
                if (fl & LX_O_NONBLOCK) h |= O_NONBLOCK;
                if (fl & LX_O_APPEND) h |= O_APPEND;
                fcntl(d->twin, F_SETFL, h);
            }
            hfds[0] = fcntl(d->twin, F_DUPFD_CLOEXEC, 0);
            rc = hfds[0] < 0 ? (int)LKD_HOST_ERR() : 1;
        }
        pthread_mutex_unlock(&o->lock);
        if (rc) return rc;
        /* the host refused its own device: the record still carries it to an LKD process */
    }
    int32_t sub = d->sub;
    return lkd_fdrec_export(LKD_FDREC_DEV, fl, &sub, sizeof sub, &hfds[0]);
}

static const lkd_fd_ops g_zero_ops = {
    .name = "dev", .read = dev_read, .write = dev_write, .lseek = dev_lseek, .poll = dev_poll, .fstat = dev_fstat,
    .describe = dev_describe, .release = dev_release, .export_fds = dev_export,
};
static const lkd_fd_ops g_dev_ops = {
    .name = "dev", .read = dev_read, .write = dev_write, .lseek = dev_lseek, .poll = dev_poll, .fstat = dev_fstat,
    .describe = dev_describe, .release = dev_release, .export_fds = dev_export,
};

static int dev_new(int sub, uint32_t flags, int twin, lkd_fdobj **out)
{
    struct devobj *d = malloc(sizeof *d);
    if (!d) return -LX_ENOMEM;
    d->sub = sub;
    d->twin = twin;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_DEV, sub == D_ZERO ? &g_zero_ops : &g_dev_ops, -1, flags, d);
    if (!o) { free(d); return -LX_ENOMEM; }
    char gp[32];
    snprintf(gp, sizeof gp, "/dev/%s", g_dev[sub].name);
    o->gpath = strdup(gp);
    o->ino = dev_ino(sub);
    *out = o;
    return 0;
}

/* A host descriptor that arrives without a guest path (SCM_RIGHTS from another host process, the runtime's
 * standard descriptors): a host twin becomes the Linux device, with its numbers (1:3 for /dev/null), its Linux
 * behaviour and the status flags of the host description. Takes host_fd when it returns 1. */
int lkd_procfs_dev_adopt(int host_fd, const struct stat *st, uint32_t lx_flags, lkd_fdobj **out)
{
    if (!S_ISCHR(st->st_mode)) return 0;
    pthread_once(&g_twin_once, twin_init);
    for (int i = 0; i < D_N; i++) {
        if (!g_twin[i] || g_twin_rdev[i] == (dev_t)-1 || st->st_rdev != g_twin_rdev[i]) continue;
        uint32_t fl = (lx_flags & ~(uint32_t)(LX_O_NONBLOCK | LX_O_APPEND)) | LX_O_LARGEFILE;
        int h = fcntl(host_fd, F_GETFL);
        if (h >= 0 && (h & O_NONBLOCK)) fl |= LX_O_NONBLOCK;
        if (h >= 0 && (h & O_APPEND)) fl |= LX_O_APPEND;
        int rc = dev_new(i, fl, host_fd, out);
        return rc < 0 ? rc : 1;
    }
    return 0;
}

/* LKD_FDREC_DEV: one of the memory devices (mem.c), in the sender's status flags. */
static int dev_rec_import(lkd_task *t, uint32_t fl, const void *data, size_t len, lkd_fdobj **out)
{
    (void)t;
    int32_t sub;
    if (len != sizeof sub) return -LX_EINVAL;
    memcpy(&sub, data, sizeof sub);
    if (sub < D_NULL || sub > D_URANDOM) return -LX_EINVAL;
    return dev_new(sub, (fl & (LX_O_ACCMODE | LX_O_NONBLOCK | LX_O_APPEND | LX_O_PATH)) | LX_O_LARGEFILE, -1, out);
}

void pf_dev_register(void) { lkd_fdrec_register(LKD_FDREC_DEV, dev_rec_import); }

/* The mm module maps /dev/zero as anonymous memory (drivers/char/mem.c mmap_zero). */
int lkd_procfs_dev_is_zero(lkd_fdobj *o) { return o && o->ops == &g_zero_ops; }

static int open_host_tty(const struct pf_node *n, uint32_t fl, lkd_fdobj **out)
{
    int hflags = 0;
    uint32_t un = 0;
    int rc = lkd_open_flags_to_host(fl & (LX_O_ACCMODE | LX_O_NONBLOCK | LX_O_NOCTTY), &hflags, &un);
    if (rc < 0) return rc;
    int fd = open("/dev/tty", hflags | O_CLOEXEC);   /* ENXIO when the host process has no controlling terminal */
    if (fd < 0) return LKD_HOST_ERR();
    char gp[32];
    snprintf(gp, sizeof gp, "/dev/%s", g_dev[n->sub].name);
    rc = lkd_fs_wrap_host_fd(fd, fl, gp, out);
    if (rc < 0) close(fd);
    return rc;
}

int pf_dev_open(lkd_task *t, const struct pf_node *n, uint32_t fl, lkd_fdobj **out)
{
    if (fl & LX_O_DIRECTORY) return -LX_ENOTDIR;
    switch (n->sub) {
    case D_TTY: case D_CONSOLE: return open_host_tty(n, fl, out);
    case D_PTMX: return -LX_ENODEV;   /* no pseudo terminal support */
    case D_NTSYNC: {
        int rc = lkd_ntsync_open_device(t, fl, out);
        if (rc == 0) (*out)->ino = n->ino;
        return rc;
    }
    }
    return dev_new(n->sub, (fl & (LX_O_ACCMODE | LX_O_NONBLOCK | LX_O_APPEND | LX_O_PATH)) | LX_O_LARGEFILE, -1, out);
}
