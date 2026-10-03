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

static int dev_of(lkd_fdobj *o) { return (int)(uintptr_t)o->priv - 1; }

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

static const lkd_fd_ops g_zero_ops = {
    .name = "dev", .read = dev_read, .write = dev_write, .lseek = dev_lseek, .poll = dev_poll, .fstat = dev_fstat,
    .describe = dev_describe,
};
static const lkd_fd_ops g_dev_ops = {
    .name = "dev", .read = dev_read, .write = dev_write, .lseek = dev_lseek, .poll = dev_poll, .fstat = dev_fstat,
    .describe = dev_describe,
};

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
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_DEV, n->sub == D_ZERO ? &g_zero_ops : &g_dev_ops, -1,
                                 (fl & (LX_O_ACCMODE | LX_O_NONBLOCK | LX_O_APPEND | LX_O_PATH)) | LX_O_LARGEFILE,
                                 (void *)(uintptr_t)(n->sub + 1));
    if (!o) return -LX_ENOMEM;
    char gp[32];
    snprintf(gp, sizeof gp, "/dev/%s", g_dev[n->sub].name);
    o->gpath = strdup(gp);
    o->ino = n->ino;
    *out = o;
    return 0;
}
