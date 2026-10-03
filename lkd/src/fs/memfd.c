/* memfd.c: memfd_create(2) and file seals (mm/memfd.c, mm/shmem.c of Linux 6.12).
 *
 * Backing: an unlinked host file created in the /dev/shm host directory (ftruncate and MAP_SHARED of
 * an unlinked file work on APFS, MESURE). The offset lives in the host fd (shared across fork).
 * Seals live in a slot of a MAP_SHARED anonymous arena created at init, so that a forked process and
 * its parent see the same seals (a host re-exec does not keep the arena: documented gap).
 *
 * Enforcement, all in LKD because every guest access goes through LKD or through mappings it controls:
 *   F_SEAL_WRITE / F_SEAL_FUTURE_WRITE: write, pwrite, writev... -EPERM; fallocate PUNCH_HOLE -EPERM.
 *   F_SEAL_GROW: ftruncate up -EPERM, fallocate past EOF -EPERM, a write past EOF stops at the last
 *                4 KiB chunk that ends inside the file (shmem_write_begin per page) or -EPERM.
 *   F_SEAL_SHRINK: ftruncate down -EPERM.
 *   F_SEAL_SEAL: further F_ADD_SEALS -EPERM.
 *   F_SEAL_EXEC: chmod changing an exec bit -EPERM.
 * Adding F_SEAL_WRITE fails with -EBUSY when the mm module reports a writable shared mapping of the
 * object (lkd_mm_has_writable_shared_mapping, Linux mapping_deny_writable). What cannot be blocked:
 * stores through a writable shared mapping created in ANOTHER process of the family (the query sees the
 * caller's address space only) or through a mapping the mm module did not register. New writable shared
 * mappings after sealing must be refused by mm (it reads lkd_fs_memfd_seals).
 *
 * Phase (request wine-7). The host maps files by 16 KiB pages: a MAP_SHARED view needs (address - host
 * offset) to be a multiple of 16 KiB. A guest mmap of offset 0 at an address 0x3000 into its host page
 * is legal on Linux. Because the memfd is LKD's own object, its host file may store guest byte x at host
 * offset x + B, B in {0, 4, 8, 12} KiB (the "phase"). The phase is decided by the first mapping of the
 * memfd (any process of the family, any description), moving the content up when B != 0; it never
 * changes afterwards. Every access LKD makes through the host fd adds B (read, write, pread/pwrite,
 * readv/writev, sendfile, splice, copy_file_range, ftruncate, fallocate, fstat, seals); the guest never
 * sees it. A later MAP_SHARED view that needs another phase gets -EINVAL (visible, logged). A raw use of
 * the host fd outside LKD's offset handling (SCM_RIGHTS to another process, exec of the memfd, a
 * /proc/<pid>/fd reopen served to another process) pins the phase at 0 first, or is refused with
 * -EOPNOTSUPP when the phase is already not 0 (lkd_fs_pin_raw). The phase lives in the shared seal word
 * (bits 16 to 18), so a fork child and reopened descriptions see the same value.
 * Gap: a process that reads or writes the memfd while another process decides a non-zero phase (the
 * first mmap) can see the content during the move.
 */
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fs_priv.h"
#include "mm.h"

#define MEMFD_SLOTS (1u << 20)
#define LX_F_ALL_SEALS (LX_F_SEAL_SEAL | LX_F_SEAL_SHRINK | LX_F_SEAL_GROW | LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE | LX_F_SEAL_EXEC)
#define LX_MFD_NAME_MAX 249
#define LX_MFD_HUGE_SHIFT 26
#define LX_MFD_HUGE_MASK 0x3fu

#define MFD_PHASE_SHIFT 16
#define MFD_PHASE_MASK (3u << MFD_PHASE_SHIFT)
#define MFD_DECIDED (1u << 18)

struct seal_arena { _Atomic uint32_t next; _Atomic uint32_t seals[MEMFD_SLOTS]; };
static struct seal_arena *g_arena;
static pthread_once_t g_arena_once = PTHREAD_ONCE_INIT;

static void arena_init(void)
{
    void *p = mmap(NULL, sizeof(struct seal_arena), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) g_arena = p;
}

struct fs_memfd {
    struct fs_file file;          /* must be first: own_offset/pos for reopened descriptions */
    _Atomic uint32_t *seals;
    _Atomic uint32_t private_seals;
    uint32_t mode;                /* permission bits presented by fstat (chmod changes them) */
    char name[LX_MFD_NAME_MAX + 8];
    int shared_priv;              /* 1: this priv is shared by reopened descriptions (refcount below) */
    _Atomic int *refs;
};

static _Atomic uint32_t *seal_slot(struct fs_memfd *m)
{
    pthread_once(&g_arena_once, arena_init);
    if (g_arena) {
        uint32_t i = atomic_fetch_add(&g_arena->next, 1);
        if (i < MEMFD_SLOTS) return &g_arena->seals[i];
    }
    lkd_log_once("fs: memfd seal arena exhausted, seals no longer shared across fork");
    return &m->private_seals;
}

/* Query used for F_SEAL_WRITE (-EBUSY). Defaults to the mm module; unit tests may point it elsewhere
 * because a test must not redefine a symbol a module owns. */
int (*lkd_fs_wsm_query)(lkd_mm *mm, struct lkd_fdobj *file) = lkd_mm_has_writable_shared_mapping;

int fs_memfd_seals(lkd_fdobj *o)
{
    if (!o || o->ops != &fs_memfd_ops) return -1;
    struct fs_memfd *m = o->priv;
    return (int)(atomic_load(m->seals) & LX_F_ALL_SEALS);
}

static uint64_t mbias(struct fs_memfd *m)
{
    uint32_t s = atomic_load(m->seals);
    return (s & MFD_DECIDED) ? (uint64_t)((s & MFD_PHASE_MASK) >> MFD_PHASE_SHIFT) * 4096 : 0;
}

uint64_t fs_memfd_bias(lkd_fdobj *o) { return o && o->ops == &fs_memfd_ops ? mbias(o->priv) : 0; }

/* Move the content of an undecided memfd up by b bytes (b < 16 KiB): host size G becomes G + b, the first
 * b bytes are zero padding. o->lock held. */
static int move_up(int fd, uint64_t b)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return (int)LKD_HOST_ERR();
    int64_t g = st.st_size;
    if (ftruncate(fd, g + (off_t)b) < 0) return (int)LKD_HOST_ERR();
    static char buf[65536];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int rc = 0;
    for (int64_t end = g; end > 0 && rc == 0;) {
        int64_t start = end > (int64_t)sizeof buf ? end - (int64_t)sizeof buf : 0;
        size_t n = (size_t)(end - start);
        if (pread(fd, buf, n, start) != (ssize_t)n || pwrite(fd, buf, n, start + (off_t)b) != (ssize_t)n) rc = (int)LKD_HOST_ERR();
        end = start;
    }
    if (rc == 0) {
        memset(buf, 0, b);
        if (pwrite(fd, buf, b, 0) != (ssize_t)b) rc = (int)LKD_HOST_ERR();
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

/* lkd_fs.h. want < 0: any phase (the current one, 0 if undecided); else exactly want (0, 4, 8 or 12 KiB). */
int lkd_fs_map_phase(lkd_fdobj *o, int64_t want)
{
    if (!o || o->ops != &fs_memfd_ops) return want <= 0 ? 0 : -LX_EINVAL;   /* host files cannot shift */
    if (want > 0 && (want >= 16384 || (want & 4095))) return -LX_EINVAL;
    struct fs_memfd *m = o->priv;
    if (want == LKD_PHASE_PEEK) return (int)mbias(m);
    pthread_mutex_lock(&o->lock);
    uint32_t s = atomic_load(m->seals);
    int rc;
    if (s & MFD_DECIDED) {
        uint64_t b = (uint64_t)((s & MFD_PHASE_MASK) >> MFD_PHASE_SHIFT) * 4096;
        rc = (want < 0 || (uint64_t)want == b) ? (int)b : -LX_EINVAL;
    } else {
        uint64_t b = want < 0 ? 0 : (uint64_t)want;
        rc = b ? move_up(o->host_fd, b) : 0;
        if (rc == 0) {
            atomic_fetch_or(m->seals, MFD_DECIDED | (uint32_t)((b / 4096) << MFD_PHASE_SHIFT));
            rc = (int)b;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return rc;
}

int lkd_fs_pin_raw(lkd_fdobj *o)
{
    int r = lkd_fs_map_phase(o, 0);
    if (r < 0 && o && o->ops == &fs_memfd_ops) {
        lkd_log_once("fs: memfd with a host phase (mapped at an address not congruent to its offset modulo 16 KiB) "
                     "cannot be handed out as a raw host descriptor: EOPNOTSUPP");
        return -LX_EOPNOTSUPP;
    }
    return r < 0 ? r : 0;
}
int lkd_fs_memfd_seals(lkd_fdobj *o) { return fs_memfd_seals(o); }

static int64_t memfd_pos(lkd_fdobj *o, const int64_t *off)
{
    struct fs_memfd *m = o->priv;
    if (off) return *off;
    if (m->file.own_offset) return m->file.pos;
    off_t p = lseek(o->host_fd, 0, SEEK_CUR);
    return p < 0 ? 0 : p;
}

/* Seal checks of a write of *n bytes at pos (caller holds o->lock or accepts the race): WRITE and
 * FUTURE_WRITE refuse; GROW keeps the page-sized chunks that end inside the file
 * (generic_perform_write + shmem_write_begin) or refuses when none does. */
int fs_memfd_limit_write(lkd_fdobj *o, int64_t pos, size_t *n);
int fs_memfd_limit_write(lkd_fdobj *o, int64_t pos, size_t *n)
{
    struct fs_memfd *m = o->priv;
    uint32_t seals = atomic_load(m->seals);
    if (seals & (LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE)) return -LX_EPERM;
    if (!(seals & LX_F_SEAL_GROW) || *n == 0) return 0;
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) return (int)LKD_HOST_ERR();
    st.st_size -= (off_t)mbias(m);
    if (pos + (int64_t)*n <= st.st_size) return 0;
    int64_t allowed = 0, p = pos, end_all = pos + (int64_t)*n;
    while (p < end_all) {
        int64_t chunk_end = (p & ~(int64_t)4095) + 4096;
        int64_t end = end_all < chunk_end ? end_all : chunk_end;
        if (end > st.st_size) break;
        allowed += end - p;
        p = end;
    }
    if (allowed == 0) return -LX_EPERM;
    *n = (size_t)allowed;
    return 0;
}

static int64_t memfd_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    (void)t;
    struct fs_memfd *m = o->priv;
    if (fs_obj_access(o) == LX_O_WRONLY) return -LX_EBADF;
    int64_t b = (int64_t)mbias(m);
    if (off) {
        int64_t at = *off + b;
        return fs_host_io(o->host_fd, gva, count, &at, 0);
    }
    /* the file position is kept in guest units (host fd offset or own_offset), the access adds the phase */
    pthread_mutex_lock(&o->lock);
    int64_t pos = memfd_pos(o, NULL), at = pos + b;
    int64_t r = fs_host_io(o->host_fd, gva, count, &at, 0);
    if (r > 0) {
        if (m->file.own_offset) m->file.pos = pos + r;
        else lseek(o->host_fd, pos + r, SEEK_SET);
    }
    pthread_mutex_unlock(&o->lock);
    return r;
}

static int64_t memfd_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    (void)t;
    struct fs_memfd *m = o->priv;
    if (fs_obj_access(o) == LX_O_RDONLY) return -LX_EBADF;
    pthread_mutex_lock(&o->lock);
    uint32_t seals = atomic_load(m->seals);
    int64_t ret;
    if (seals & (LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE)) { ret = -LX_EPERM; goto out; }
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) { ret = LKD_HOST_ERR(); goto out; }
    int append = (fs_obj_getfl(o) & LX_O_APPEND) != 0;
    int64_t b = (int64_t)mbias(m);
    int64_t pos = append ? st.st_size - b : memfd_pos(o, off);
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    size_t n = (size_t)count;
    int lim = fs_memfd_limit_write(o, pos, &n);
    if (lim < 0) { ret = lim; goto out; }
    count = n;
    int64_t at = pos + b;
    ret = fs_host_io(o->host_fd, gva, count, &at, 1);
    if (ret > 0 && !off) {
        if (m->file.own_offset) m->file.pos = pos + ret;
        else lseek(o->host_fd, pos + ret, SEEK_SET);
    }
out:
    pthread_mutex_unlock(&o->lock);
    if (ret > 0) lkd_mm_file_written(o, (uint64_t)pos, (uint64_t)ret);   /* translated code of mapped views */
    return ret;
}

static int64_t memfd_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    (void)t;
    struct fs_memfd *m = o->priv;
    int64_t base;
    struct stat st;
    pthread_mutex_lock(&o->lock);
    int64_t cur = m->file.own_offset ? m->file.pos : lseek(o->host_fd, 0, SEEK_CUR);
    switch (whence) {
    case LX_SEEK_SET: base = 0; break;
    case LX_SEEK_CUR: base = cur; break;
    case LX_SEEK_END:
        if (fstat(o->host_fd, &st) < 0) { pthread_mutex_unlock(&o->lock); return LKD_HOST_ERR(); }
        base = st.st_size - (off_t)mbias(m);
        break;
    case LX_SEEK_DATA: case LX_SEEK_HOLE: {
        if (fstat(o->host_fd, &st) < 0) { pthread_mutex_unlock(&o->lock); return LKD_HOST_ERR(); }
        st.st_size -= (off_t)mbias(m);
        /* shmem_file_llseek: with no holes tracked, DATA is off itself, HOLE is EOF; past EOF -ENXIO. */
        if (off < 0 || off >= st.st_size) { pthread_mutex_unlock(&o->lock); return -LX_ENXIO; }
        int64_t np = whence == LX_SEEK_DATA ? off : st.st_size;
        if (m->file.own_offset) m->file.pos = np; else lseek(o->host_fd, np, SEEK_SET);
        pthread_mutex_unlock(&o->lock);
        return np;
    }
    default: pthread_mutex_unlock(&o->lock); return -LX_EINVAL;
    }
    if ((off > 0 && base > INT64_MAX - off) || base + off < 0) { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
    int64_t np = base + off;
    if (m->file.own_offset) m->file.pos = np;
    else if (lseek(o->host_fd, np, SEEK_SET) < 0) { pthread_mutex_unlock(&o->lock); return LKD_HOST_ERR(); }
    pthread_mutex_unlock(&o->lock);
    return np;
}

static int memfd_ftruncate(lkd_task *t, lkd_fdobj *o, int64_t len)
{
    (void)t;
    struct fs_memfd *m = o->priv;
    pthread_mutex_lock(&o->lock);
    struct stat st;
    int rc = 0;
    uint32_t seals = atomic_load(m->seals);
    off_t b = (off_t)mbias(m);
    if (fstat(o->host_fd, &st) < 0) rc = (int)LKD_HOST_ERR();
    else if (len < st.st_size - b && (seals & LX_F_SEAL_SHRINK)) rc = -LX_EPERM;
    else if (len > st.st_size - b && (seals & LX_F_SEAL_GROW)) rc = -LX_EPERM;
    else if (ftruncate(o->host_fd, len + b) < 0) rc = (int)LKD_HOST_ERR();
    pthread_mutex_unlock(&o->lock);
    return rc;
}

/* fallocate on a memfd (shmem_fallocate): mode 0 / KEEP_SIZE / PUNCH_HOLE|KEEP_SIZE. */
int fs_memfd_fallocate(lkd_fdobj *o, int mode, int64_t off, int64_t len);
int fs_memfd_fallocate(lkd_fdobj *o, int mode, int64_t off, int64_t len)
{
    struct fs_memfd *m = o->priv;
    const int KEEP = 1, PUNCH = 2;
    if (mode & ~(KEEP | PUNCH)) return -LX_EOPNOTSUPP;
    pthread_mutex_lock(&o->lock);
    uint32_t seals = atomic_load(m->seals);
    struct stat st;
    int rc = 0;
    off_t b = (off_t)mbias(m);
    if (fstat(o->host_fd, &st) < 0) rc = (int)LKD_HOST_ERR();
    else st.st_size -= b;                         /* guest size */
    if (rc) { }
    else if (mode & PUNCH) {
        if (seals & (LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE)) rc = -LX_EPERM;
        else {
            /* zero the range inside the file: reads of a punched hole return zeros */
            int64_t end = off + len > st.st_size ? st.st_size : off + len;
            static const char zeros[4096];
            for (int64_t p = off; p < end && rc == 0;) {
                size_t n = (size_t)(end - p > 4096 ? 4096 : end - p);
                ssize_t w = pwrite(o->host_fd, zeros, n, p + b);
                if (w < 0) rc = (int)LKD_HOST_ERR(); else p += w;
            }
        }
    } else if ((seals & LX_F_SEAL_GROW) && off + len > st.st_size) {
        rc = -LX_EPERM;
    } else if (!(mode & KEEP) && off + len > st.st_size) {
        if (ftruncate(o->host_fd, off + len + b) < 0) rc = (int)LKD_HOST_ERR();
    }
    pthread_mutex_unlock(&o->lock);
    return rc;
}

/* fchmod on a memfd: the presented mode changes; F_SEAL_EXEC forbids changing an exec bit. */
int fs_memfd_chmod(lkd_fdobj *o, uint32_t mode);
int fs_memfd_chmod(lkd_fdobj *o, uint32_t mode)
{
    struct fs_memfd *m = o->priv;
    pthread_mutex_lock(&o->lock);
    int rc = 0;
    if ((atomic_load(m->seals) & LX_F_SEAL_EXEC) && ((m->mode ^ mode) & 0111)) rc = -LX_EPERM;
    else m->mode = mode & 07777;
    pthread_mutex_unlock(&o->lock);
    return rc;
}

int64_t fs_memfd_fcntl_seals(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    if (!o || o->ops != &fs_memfd_ops) return -LX_EINVAL;   /* memfd_fcntl: not a sealable file */
    struct fs_memfd *m = o->priv;
    if (cmd == LX_F_GET_SEALS) return (int64_t)(atomic_load(m->seals) & LX_F_ALL_SEALS);
    if (cmd != LX_F_ADD_SEALS) return -LX_EINVAL;
    uint32_t seals = (uint32_t)arg;
    if (seals & ~(uint32_t)LX_F_ALL_SEALS) return -LX_EINVAL;
    if (fs_obj_access(o) == LX_O_RDONLY) return -LX_EPERM;    /* needs FMODE_WRITE */
    pthread_mutex_lock(&o->lock);
    int64_t rc = 0;
    uint32_t cur = atomic_load(m->seals);
    if (cur & LX_F_SEAL_SEAL) rc = -LX_EPERM;
    else if ((seals & LX_F_SEAL_WRITE) && !(cur & LX_F_SEAL_WRITE) && t &&
             lkd_fs_wsm_query && lkd_fs_wsm_query(t->mm, o) > 0)
        rc = -LX_EBUSY;
    if (rc == 0) {
        if ((seals & LX_F_SEAL_EXEC) && (m->mode & 0111))
            seals |= LX_F_SEAL_SHRINK | LX_F_SEAL_GROW | LX_F_SEAL_WRITE | LX_F_SEAL_FUTURE_WRITE;
        atomic_fetch_or(m->seals, seals);
    }
    pthread_mutex_unlock(&o->lock);
    return rc;
}

static int64_t memfd_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    return fs_memfd_fcntl_seals(t, o, cmd, arg);
}

/* shmem inode of a memfd: S_IFREG, mode 0777 (0666 with MFD_NOEXEC_SEAL), nlink 1, the internal shm
 * mount's device (0:1), owner the creator. Size and blocks are the host file's. */
static int memfd_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    (void)t;
    struct fs_memfd *m = o->priv;
    struct stat st;
    if (fstat(o->host_fd, &st) < 0) return LKD_HOST_ERR();
    st.st_size -= (off_t)mbias(m);
    fs_stat_from_host(&st, out);
    out->st_dev = 1;
    out->st_nlink = 1;
    out->st_mode = LX_S_IFREG | m->mode;
    out->st_blksize = 4096;
    out->st_blocks = (st.st_size + 511) / 512;
    return 0;
}

static int memfd_fsync(lkd_task *t, lkd_fdobj *o, int datasync) { (void)t; (void)o; (void)datasync; return 0; }
static int memfd_mmap_fd(lkd_fdobj *o) { return o->host_fd; }

static int memfd_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    struct fs_memfd *m = o->priv;
    int n = snprintf(buf, cap, "/memfd:%s (deleted)", m->name);
    return n < 0 ? -LX_EINVAL : ((size_t)n >= cap ? (int)cap : n);
}

static void memfd_release(lkd_fdobj *o)
{
    struct fs_memfd *m = o->priv;
    if (!m) return;
    free(m->file.host_path);
    free(m);
    o->priv = NULL;
}

const lkd_fd_ops fs_memfd_ops = {
    .name = "memfd", .read = memfd_read, .write = memfd_write, .lseek = memfd_lseek, .fcntl = memfd_fcntl,
    .fstat = memfd_fstat, .ftruncate = memfd_ftruncate, .fsync = memfd_fsync, .mmap_fd = memfd_mmap_fd,
    .describe = memfd_describe, .release = memfd_release,
};

int fs_memfd_reopen(lkd_fdobj *o, uint32_t fl, lkd_fdobj **out);
int fs_memfd_reopen(lkd_fdobj *o, uint32_t fl, lkd_fdobj **out)
{
    struct fs_memfd *m = o->priv;
    struct fs_memfd *n = calloc(1, sizeof *n);
    if (!n) return -LX_ENOMEM;
    int d = fcntl(o->host_fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0) { free(n); return LKD_HOST_ERR(); }
    n->seals = m->seals == &m->private_seals ? &n->private_seals : m->seals;
    if (m->seals == &m->private_seals) atomic_store(&n->private_seals, atomic_load(&m->private_seals));
    n->mode = m->mode;
    memcpy(n->name, m->name, sizeof n->name);
    n->file.own_offset = 1;
    lkd_fdobj *no = lkd_fdobj_new(LKD_FD_MEMFD, &fs_memfd_ops, d, (fl & (LX_O_ACCMODE | LX_O_APPEND)) | LX_O_LARGEFILE, n);
    if (!no) { close(d); free(n); return -LX_ENOMEM; }
    no->ino = o->ino;
    no->dev = o->dev;
    no->gpath = o->gpath ? strdup(o->gpath) : NULL;
    if (fl & LX_O_TRUNC) {
        int rc = memfd_ftruncate(NULL, no, 0);
        if (rc < 0) { lkd_fdobj_put(no); return rc; }
    }
    *out = no;
    return 0;
}

static int memfd_backing(void)
{
    const char *dir = lkd_fs_host_shm_dir();
    char tmpl[PATH_MAX];
    if (!dir || !dir[0]) {
        char tdir[PATH_MAX];
        size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, tdir, sizeof tdir);
        snprintf(tmpl, sizeof tmpl, "%s/lkd-memfd.XXXXXX", n ? tdir : "/tmp");
    } else {
        snprintf(tmpl, sizeof tmpl, "%s/.lkd-memfd.XXXXXX", dir);
    }
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/* memfd_create(2), mm/memfd.c (Linux 6.12). */
LKD_SYSCALL(memfd_create)
{
    uint32_t flags = LKD_ARGUI(1);
    const uint32_t known = LX_MFD_CLOEXEC | LX_MFD_ALLOW_SEALING | LX_MFD_HUGETLB | LX_MFD_NOEXEC_SEAL | LX_MFD_EXEC;
    if (flags & LX_MFD_HUGETLB) {
        if (flags & ~(known | (LX_MFD_HUGE_MASK << LX_MFD_HUGE_SHIFT))) return -LX_EINVAL;
        /* hugetlbfs is not available on this host: the answer of a kernel without CONFIG_HUGETLBFS */
        return -LX_EINVAL;
    }
    if (flags & ~known) return -LX_EINVAL;
    if ((flags & LX_MFD_EXEC) && (flags & LX_MFD_NOEXEC_SEAL)) return -LX_EINVAL;
    char name[LX_MFD_NAME_MAX + 2];
    int64_t nl = lkd_user_strn(LKD_ARGP(0), name, sizeof name);
    if (nl == -LX_EFAULT) return -LX_EFAULT;
    if (nl < 0 || nl > LX_MFD_NAME_MAX) return -LX_EINVAL;
    struct fs_memfd *m = calloc(1, sizeof *m);
    if (!m) return -LX_ENOMEM;
    memcpy(m->name, name, (size_t)nl + 1);
    m->seals = seal_slot(m);
    uint32_t seals = LX_F_SEAL_SEAL;
    m->mode = 0777;
    if (flags & LX_MFD_NOEXEC_SEAL) { m->mode = 0666; seals = LX_F_SEAL_EXEC; }
    else if (flags & LX_MFD_ALLOW_SEALING) seals = 0;
    atomic_store(m->seals, seals);
    int hfd = memfd_backing();
    if (hfd < 0) { int64_t e = LKD_HOST_ERR(); free(m); return e == -LX_EMFILE ? -LX_EMFILE : -LX_ENOMEM; }
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_MEMFD, &fs_memfd_ops, hfd, LX_O_RDWR | LX_O_LARGEFILE, m);
    if (!o) { close(hfd); free(m); return -LX_ENOMEM; }
    char gp[LX_MFD_NAME_MAX + 16];
    snprintf(gp, sizeof gp, "/memfd:%s", name);
    o->gpath = strdup(gp);
    struct stat st;
    if (fstat(hfd, &st) == 0) o->ino = st.st_ino;
    o->dev = 1;
    int fd = lkd_fd_alloc(t, o, 0, (flags & LX_MFD_CLOEXEC) != 0);
    lkd_fdobj_put(o);
    return fd;
}

void fs_register_memfd(void) { LKD_REGISTER(memfd_create); }
