/* file.c: open() of the virtual trees and the file object of generated files.
 *
 * Generated files follow seq_file semantics (fs/seq_file.c): the content is produced at the first read
 * and produced again by any read at offset 0 (after lseek(0) or pread(0)), so a reader that rewinds
 * sees current state; lseek accepts SEEK_SET and SEEK_CUR only (seq_lseek), a negative offset is
 * -EINVAL. Permission checks of open follow generic_permission on the node mode and owner, then the
 * per-file rules of fs/proc/base.c (proc_mem_open for maps/smaps/environ/auxv).
 */
#include <fcntl.h>
#include <unistd.h>

#include "procfs_priv.h"

struct pf_file {
    struct pf_node n;
    char *text;
    size_t len;
    int have;
    int64_t pos;
};

static _Atomic uint64_t g_mnt_dev[3];

uint64_t pf_mount_dev(lkd_task *t, int mnt)
{
    if (mnt < 0 || mnt > 2) return 0;
    uint64_t d = atomic_load(&g_mnt_dev[mnt]);
    if (d || !t) return d;
    struct lx_stat st;
    if (lkd_vfs_stat(t, LX_AT_FDCWD, pf_mount_point(mnt), LX_AT_SYMLINK_NOFOLLOW, &st) == 0) {
        d = st.st_dev;
        atomic_store(&g_mnt_dev[mnt], d);
    }
    return d;
}

void pf_stat_node(lkd_task *t, const struct pf_node *n, struct lx_stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = pf_mount_dev(t, n->mnt);
    st->st_ino = n->ino;
    st->st_nlink = n->nlink ? n->nlink : 1;
    st->st_mode = n->mode;
    st->st_uid = n->uid;
    st->st_gid = n->gid;
    st->st_rdev = n->rdev;
    st->st_size = (int64_t)n->size;
    /* proc and sysfs report s_blocksize 1024 / 4096 (fs/proc/inode.c, fs/kernfs); devtmpfs 4096 */
    st->st_blksize = n->mnt == PF_PROC ? 1024 : 4096;
    st->st_atime_sec = st->st_mtime_sec = st->st_ctime_sec = n->mtime;
}

/* Produce the current content of a regular node for the viewer t. */
static int generate(lkd_task *t, const struct pf_node *n, char **text, size_t *len)
{
    *text = NULL;
    *len = 0;
    if (n->mnt == PF_PROC && n->pid > 0) return pf_generate_pid(t, n, text, len);
    struct pbuf b = { 0 };
    int rc = n->mnt == PF_SYS ? pf_sys_generate(n, &b) : pf_generate_global(t, n, &b);
    if (rc < 0) { pb_free(&b); return rc; }
    if (b.oom) { pb_free(&b); return -LX_ENOMEM; }
    *text = b.p;
    *len = b.n;
    return 0;
}

static int64_t gf_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{
    struct pf_file *f = o->priv;
    pthread_mutex_lock(&o->lock);
    int64_t pos = offset ? *offset : f->pos;
    int need = !f->have || pos == 0;
    pthread_mutex_unlock(&o->lock);
    if (pos < 0) return -LX_EINVAL;
    if (need) {
        /* Generate without the object lock: the remote path may wait for another process. */
        char *txt;
        size_t len;
        int rc = generate(t, &f->n, &txt, &len);
        if (rc < 0) return rc;
        pthread_mutex_lock(&o->lock);
        free(f->text);
        f->text = txt;
        f->len = len;
        f->have = 1;
        pthread_mutex_unlock(&o->lock);
    }
    pthread_mutex_lock(&o->lock);
    int64_t r = 0;
    if ((uint64_t)pos < f->len && count) {
        uint64_t n = f->len - (uint64_t)pos;
        if (n > count) n = count;
        if (lkd_user_write(gva, f->text + pos, n) < 0) r = -LX_EFAULT;
        else r = (int64_t)n;
    }
    if (r > 0 && !offset) f->pos = pos + r;
    pthread_mutex_unlock(&o->lock);
    return r;
}

static int64_t gf_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{
    struct pf_file *f = o->priv;
    if (f->n.kind == PK_COMM) {
        char buf[64];
        size_t n = count < sizeof buf - 1 ? (size_t)count : sizeof buf - 1;   /* comm_write: TASK_COMM_LEN buffer */
        if (n && lkd_user_read(gva, buf, n) < 0) return -LX_EFAULT;
        buf[n] = 0;
        int64_t r = pf_comm_write(t, &f->n, buf, n);
        return r < 0 ? r : (int64_t)count;
    }
    if (f->n.kind == PK_SFILE) {
        /* Only root reaches this (0644 root). LKD cannot change these kernel parameters: refuse visibly. */
        lkd_log_once("procfs: write to %s/sys refused (EPERM): kernel parameters are fixed in LKD", pf_mount_point(PF_PROC));
        return -LX_EPERM;
    }
    (void)offset;
    return -LX_EINVAL;   /* no ->write: vfs_write returns -EINVAL */
}

static int64_t gf_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    struct pf_file *f = o->priv;
    pthread_mutex_lock(&o->lock);
    int64_t np;
    if (whence == LX_SEEK_SET) np = off;
    else if (whence == LX_SEEK_CUR) np = f->pos + off;
    else { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
    if (np < 0) { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
    f->pos = np;
    pthread_mutex_unlock(&o->lock);
    return np;
}

static int gf_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    struct pf_file *f = o->priv;
    pf_stat_node(t, &f->n, out);
    return 0;
}

static void gf_release(lkd_fdobj *o)
{
    struct pf_file *f = o->priv;
    if (!f) return;
    free(f->text);
    free(f);
    o->priv = NULL;
}

static const lkd_fd_ops g_gen_ops = {
    .name = "procfs", .read = gf_read, .write = gf_write, .lseek = gf_lseek, .fstat = gf_fstat,
    .release = gf_release,
};

static char *make_gpath(int mnt, const char *rel)
{
    size_t n = strlen(pf_mount_point(mnt)) + strlen(rel ? rel : "") + 2;
    char *p = malloc(n);
    if (!p) return NULL;
    snprintf(p, n, "%s%s%s", pf_mount_point(mnt), rel && rel[0] ? "/" : "", rel ? rel : "");
    return p;
}

/* Open-time checks of the per-process files that read the target's memory (proc_mem_open with
 * PTRACE_MODE_READ: maps, smaps, environ, auxv). A zombie has no mm: the open succeeds and reads
 * return nothing, like mm_access returning NULL. */
static int mem_open_check(lkd_task *t, const struct pf_node *n)
{
    switch (n->kind) {
    case PK_MAPS: case PK_SMAPS: case PK_ENVIRON: case PK_AUXV:
        return pf_ptrace_ok(t, n->pid, LKD_PTRACE_MODE_READ) < 0 ? -LX_EACCES : 0;
    default:
        return 0;
    }
}

int pf_open(lkd_task *t, int mnt, const char *rel, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{
    (void)mode;
    struct pf_node n;
    int rc = pf_resolve(t, mnt, rel, &n);
    if (rc < 0) return rc;
    if (fl & LX_O_CREAT && (fl & LX_O_EXCL)) return -LX_EEXIST;
    if (n.mnt == PF_DEV && n.kind == PK_DEVNODE) {
        uint32_t acc = fl & LX_O_ACCMODE;
        int want = (fl & LX_O_PATH) ? 0 : ((acc != LX_O_WRONLY ? 4 : 0) | (acc != LX_O_RDONLY || (fl & LX_O_TRUNC) ? 2 : 0));
        if (want && (rc = pf_permission(t, n.mode, n.uid, n.gid, want)) < 0) return rc;
        return pf_dev_open(t, &n, fl, out);
    }
    if (n.magic) return pf_pid_open_link(t, &n, fl, out);
    if (LX_S_ISDIR(n.mode)) return -LX_EISDIR;
    if (LX_S_ISLNK(n.mode)) return -LX_ELOOP;
    uint32_t acc = fl & LX_O_ACCMODE;
    int wants_write = acc == LX_O_WRONLY || acc == LX_O_RDWR || (fl & LX_O_TRUNC);
    int want = (acc != LX_O_WRONLY ? 4 : 0) | (wants_write ? 2 : 0);
    if ((rc = pf_permission(t, n.mode, n.uid, n.gid, want)) < 0) return rc;
    if (n.kind == PK_MEM) return pf_mem_open(t, &n, fl, out);
    if ((rc = mem_open_check(t, &n)) < 0) return rc;
    if (n.kind == PK_CPUINFO && !pf_cpuid_available()) {
        lkd_log_once("procfs: /proc/cpuinfo needs the engine CPUID provider (lkd_procfs_set_cpuid): ENOSYS");
        return -LX_ENOSYS;
    }
    struct pf_file *f = calloc(1, sizeof *f);
    if (!f) return -LX_ENOMEM;
    f->n = n;
    uint32_t keep = fl & (LX_O_ACCMODE | LX_O_APPEND | LX_O_NONBLOCK | LX_O_DIRECT | LX_O_NOATIME | LX_O_SYNC | LX_O_DSYNC);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_PROCFD, &g_gen_ops, -1, keep | LX_O_LARGEFILE, f);
    if (!o) { free(f); return -LX_ENOMEM; }
    o->gpath = make_gpath(mnt, rel);
    o->ino = n.ino;
    o->dev = pf_mount_dev(t, mnt);
    *out = o;
    return 0;
}
