/* shm.c: System V shared memory (ipc/shm.c, ipc/util.c of Linux v6.12). Owner: mm.
 *
 * The segment table lives in an anonymous MAP_SHARED region created by lkd_mm_init, inherited by every
 * forked process of the family. Each segment is a POSIX shared memory object (shm_open) whose name is
 * kept in the table, so any process of the family can attach it by id; the name is unlinked when the
 * segment is destroyed (IPC_RMID with no attachment left). The table lock is a spin lock word holding
 * the host pid of its owner; a dead owner's lock is taken over (CONVENTIONS rule 3.4).
 * Gap: the table does not survive a host re-exec of the runtime (guest execve); docs/requests/mm-3.md.
 * nattch counts VMAs, like Linux (shm_open/shm_close run for every VMA, splits and forks included).
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mm_priv.h"

#define LX_IPC_PRIVATE 0
#define LX_IPC_CREAT 01000
#define LX_IPC_EXCL 02000
#define LX_IPC_RMID 0
#define LX_IPC_SET 1
#define LX_IPC_STAT 2
#define LX_IPC_INFO 3
#define LX_SHM_LOCK 11
#define LX_SHM_UNLOCK 12
#define LX_SHM_STAT 13
#define LX_SHM_INFO 14
#define LX_SHM_STAT_ANY 15
#define LX_SHM_RDONLY 010000
#define LX_SHM_RND 020000
#define LX_SHM_REMAP 040000
#define LX_SHM_EXEC 0100000
#define LX_SHM_HUGETLB 04000
#define LX_SHM_DEST 01000
#define LX_SHM_LOCKED 02000
#define SHMLBA 4096ull
#define SEQ_MULT 32768
#define SHM_SLOTS 1024
#define SHMMAX (UINT64_MAX - (1ull << 24))
#define SHMALL (UINT64_MAX - (1ull << 24))

struct lx_ipc64_perm {
    int32_t key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq, pad2;
    uint64_t unused1, unused2;
};
struct lx_shmid64_ds {
    struct lx_ipc64_perm shm_perm;
    uint64_t shm_segsz;
    int64_t shm_atime, shm_dtime, shm_ctime;
    int32_t shm_cpid, shm_lpid;
    uint64_t shm_nattch;
    uint64_t unused4, unused5;
};
struct lx_shminfo64 { uint64_t shmmax, shmmin, shmmni, shmseg, shmall, unused[4]; };
struct lx_shm_info { int32_t used_ids; int32_t pad; uint64_t shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes; };
_Static_assert(sizeof(struct lx_ipc64_perm) == 48, "ipc64_perm");
_Static_assert(sizeof(struct lx_shmid64_ds) == 112, "shmid64_ds");
_Static_assert(sizeof(struct lx_shminfo64) == 72, "shminfo64");
_Static_assert(sizeof(struct lx_shm_info) == 48, "shm_info");

struct seg {
    int32_t used, key, seq;
    uint32_t mode, uid, gid, cuid, cgid;
    int32_t cpid, lpid;
    uint64_t size;
    int64_t atime, dtime, ctime;
    int64_t nattch;
    char name[32];
};
struct table {
    _Atomic int32_t lock;
    int32_t creator;
    int32_t next_seq;
    struct seg seg[SHM_SLOTS];
};
static struct table *T;

static void tlock(void)
{
    int32_t me = getpid();
    for (unsigned spins = 0;; spins++) {
        int32_t cur = 0;
        if (atomic_compare_exchange_weak(&T->lock, &cur, me)) return;
        if (cur && (spins & 1023) == 1023 && kill(cur, 0) < 0 && errno == ESRCH)
            atomic_compare_exchange_strong(&T->lock, &cur, 0); /* owner died holding it */
        if (spins > 64) sched_yield();
    }
}
static void tunlock(void) { atomic_store(&T->lock, 0); }

void mm_shm_init(void)
{
    if (T) return;
    void *p = mmap(NULL, sizeof *T, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { lkd_log("mm: SysV shm table: mmap failed (%d)", errno); return; }
    T = p;
    T->creator = getpid();
    T->next_seq = 1;
}

static int32_t cur_pid(void)
{
    lkd_task *t = lkd_current();
    return t && t->proc ? t->proc->pid : 0;
}

static void creds(lkd_task *t, uint32_t *uid, uint32_t *gid)
{
    *uid = t && t->proc ? t->proc->cred.euid : 0;
    *gid = t && t->proc ? t->proc->cred.egid : 0;
}

static int in_group(lkd_task *t, uint32_t g)
{
    if (!t || !t->proc) return 0;
    if (t->proc->cred.egid == g) return 1;
    for (uint32_t i = 0; i < t->proc->cred.ngroups && i < LKD_MAX_GROUPS; i++)
        if (t->proc->cred.groups[i] == g) return 1;
    return 0;
}

/* ipcperms (ipc/util.c) */
static int ipcperms(lkd_task *t, const struct seg *s, int flag)
{
    uint32_t uid, gid;
    creds(t, &uid, &gid);
    int requested = (flag >> 6) | (flag >> 3) | flag;
    int granted = (int)s->mode;
    if (uid == s->cuid || uid == s->uid) granted >>= 6;
    else if (in_group(t, s->cgid) || in_group(t, s->gid)) granted >>= 3;
    if ((requested & ~granted & 0007) && !mm_capable(t, LKD_CAP_IPC_OWNER)) return -1;
    return 0;
}

static int64_t now_s(void) { return (int64_t)time(NULL); }

static void destroy(struct seg *s)
{
    shm_unlink(s->name);
    s->used = 0;
}

static struct seg *by_id(int shmid)
{
    if (shmid < 0) return NULL;
    int slot = shmid % SEQ_MULT, seq = shmid / SEQ_MULT;
    if (slot >= SHM_SLOTS) return NULL;
    struct seg *s = &T->seg[slot];
    return s->used && s->seq == seq ? s : NULL;
}

static int seg_id(const struct seg *s) { return (int)(s - T->seg) + s->seq * SEQ_MULT; }

LKD_SYSCALL(shmget)
{
    int32_t key = (int32_t)LKD_ARGI(0);
    uint64_t size = LKD_ARGU(1);
    int flg = LKD_ARGI(2);
    if (!T) return -LX_ENOMEM;
    tlock();
    struct seg *s = NULL;
    if (key != LX_IPC_PRIVATE)
        for (int i = 0; i < SHM_SLOTS; i++)
            if (T->seg[i].used && T->seg[i].key == key) { s = &T->seg[i]; break; }
    int64_t r;
    if (s) {
        if ((flg & LX_IPC_CREAT) && (flg & LX_IPC_EXCL)) r = -LX_EEXIST;
        else if (ipcperms(t, s, flg)) r = -LX_EACCES;
        else if (s->size < size) r = -LX_EINVAL; /* shm_more_checks */
        else r = seg_id(s);
        tunlock();
        return r;
    }
    if (key != LX_IPC_PRIVATE && !(flg & LX_IPC_CREAT)) { tunlock(); return -LX_ENOENT; }
    /* newseg */
    if (size < 1 || size > SHMMAX) { tunlock(); return -LX_EINVAL; }
    if (flg & LX_SHM_HUGETLB) { tunlock(); return -LX_ENOMEM; } /* no huge page pool */
    for (int i = 0; i < SHM_SLOTS && !s; i++)
        if (!T->seg[i].used) s = &T->seg[i];
    if (!s) { tunlock(); return -LX_ENOSPC; }
    int slot = (int)(s - T->seg);
    int seq = T->next_seq++ % 65536;
    if (T->next_seq >= 65536) T->next_seq = 1;
    char name[32];
    snprintf(name, sizeof name, "/lkd.%d.%d.%d", T->creator, slot, seq);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) { shm_unlink(name); fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600); }
    if (fd < 0) { r = LKD_HOST_ERR(); tunlock(); return r == -LX_EMFILE || r == -LX_ENFILE ? r : -LX_ENOSPC; }
    if (ftruncate(fd, (off_t)HP_UP(size)) < 0) {
        close(fd);
        shm_unlink(name);
        tunlock();
        return -LX_ENOMEM;
    }
    close(fd);
    memset(s, 0, sizeof *s);
    s->used = 1;
    s->key = key;
    s->seq = seq;
    s->mode = (uint32_t)flg & 0777;
    creds(t, &s->uid, &s->gid);
    s->cuid = s->uid;
    s->cgid = s->gid;
    s->cpid = t && t->proc ? t->proc->pid : 0;
    s->size = size;
    s->ctime = now_s();
    snprintf(s->name, sizeof s->name, "%s", name);
    r = seg_id(s);
    (void)slot;
    tunlock();
    return r;
}

void mm_shm_attach_vma(const mm_vma *v)
{
    if (!T || !v->bk || v->bk->kind != MB_SYSV) return;
    tlock();
    struct seg *s = by_id(v->bk->shm_id);
    if (s) { s->nattch++; s->atime = now_s(); s->lpid = cur_pid(); }
    tunlock();
}

int mm_shm_detach_vma(const mm_vma *v)
{
    if (!T || !v->bk || v->bk->kind != MB_SYSV) return 0;
    tlock();
    struct seg *s = by_id(v->bk->shm_id);
    if (s) {
        if (s->nattch > 0) s->nattch--;
        s->dtime = now_s();
        s->lpid = cur_pid();
        if (!s->nattch && (s->mode & LX_SHM_DEST)) destroy(s);
    }
    tunlock();
    return 0;
}

void mm_shm_fork_child(lkd_mm *mm)
{
    for (mm_vma *v = vt_first(&mm->vt); v; v = vt_next(&mm->vt, v))
        if (v->bk && v->bk->kind == MB_SYSV) mm_shm_attach_vma(v);
}

LKD_SYSCALL(shmat)
{
    int shmid = LKD_ARGI(0);
    uint64_t addr = LKD_ARGU(1);
    int flg = LKD_ARGI(2);
    lkd_mm *mm = mm_of(t);
    if (shmid < 0 || !T) return -LX_EINVAL;
    int fixed = 0;
    if (addr) {
        if (addr & (SHMLBA - 1)) {
            if (flg & LX_SHM_RND) {
                addr &= ~(SHMLBA - 1);
                if (!addr && (flg & LX_SHM_REMAP)) return -LX_EINVAL;
            } else {
                return -LX_EINVAL;
            }
        }
        fixed = 1;
    } else if (flg & LX_SHM_REMAP) {
        return -LX_EINVAL;
    }
    uint32_t prot = LX_PROT_READ;
    int acc = 0444;
    if (!(flg & LX_SHM_RDONLY)) { prot |= LX_PROT_WRITE; acc |= 0222; }
    if (flg & LX_SHM_EXEC) { prot |= LX_PROT_EXEC; acc |= 0111; }
    tlock();
    struct seg *s = by_id(shmid);
    if (!s) { tunlock(); return -LX_EINVAL; }
    if (ipcperms(t, s, acc)) { tunlock(); return -LX_EACCES; }
    uint64_t size = s->size;
    int32_t key = s->key;
    char name[32];
    memcpy(name, s->name, sizeof name);
    tunlock();
    if (!mm) return -LX_ENOMEM;
    int fd = shm_open(name, (flg & LX_SHM_RDONLY) ? O_RDONLY : O_RDWR);
    if (fd < 0) return -LX_EIDRM;
    struct mm_backing *bk = mb_new(MB_SYSV);
    char label[32];
    snprintf(label, sizeof label, "/SYSV%08x", (uint32_t)key);
    if (!bk || !(bk->name = strdup(label))) { mb_put(bk); close(fd); return -LX_ENOMEM; }
    bk->host_fd = fd;
    bk->shm_id = shmid;
    bk->shm_slot = shmid % SEQ_MULT;
    bk->obj_size = HP_UP(size);
    bk->deleted = 1;           /* the shm file is never linked anywhere: "/SYSV%08x (deleted)" */
    bk->dev_major = 0;
    bk->dev_minor = 1;
    bk->ino = (uint64_t)(uint32_t)shmid; /* Linux uses the shmid as the inode number of the shm file */
    uint64_t len = GP_ALIGN(size);
    struct mm_inval iv = { 0 };
    int64_t r;
    mm_lock(mm);
    if (fixed) {
        mm_vma *v = vt_first_end_after(&mm->vt, addr);
        if (!(flg & LX_SHM_REMAP) && v && v->start < addr + len) r = -LX_EINVAL;
        else if (addr < mm_win.min_addr || addr > mm_win.g_hi - len) r = addr < mm_win.min_addr ? -LX_EPERM : -LX_ENOMEM;
        else r = 0;
    } else {
        int64_t err;
        addr = mm_get_unmapped(mm, 0, len, 0, 0, 1, 0, &err);
        r = err;
    }
    if (!r) {
        struct mm_mapreq rq = { .addr = addr, .len = len, .prot = prot,
                                .vflags = VF_SHARED | VF_HOSTSHARED | ((flg & LX_SHM_RDONLY) ? 0 : VF_MAYWRITE),
                                .bk = bk, .pgoff = 0, .fixed = 1 };
        r = mm_map_locked(mm, &rq, &iv);
    }
    mm_unlock(mm);
    mm_inval_flush(&iv);
    close(fd);
    bk->host_fd = -1;
    mb_put(bk);
    return r;
}

LKD_SYSCALL(shmdt)
{
    uint64_t addr = LKD_ARGU(0);
    lkd_mm *mm = mm_of(t);
    if (addr & (GP - 1)) return -LX_EINVAL;
    if (!mm) return -LX_EINVAL;
    struct mm_inval iv = { 0 };
    int64_t r = -LX_EINVAL;
    mm_lock(mm);
    mm_vma *v = vt_first_end_after(&mm->vt, addr);
    int id = -1;
    uint64_t size = 0;
    for (; v; v = vt_next(&mm->vt, v))
        if (v->bk && v->bk->kind == MB_SYSV && v->start - addr == v->pgoff) {
            id = v->bk->shm_id;
            size = v->bk->obj_size;
            uint64_t s = v->start, e = v->end;
            v = vt_next(&mm->vt, v);
            mm_unmap_locked(mm, s, e, &iv);
            r = 0;
            break;
        }
    if (!r) {
        /* the other VMAs of the same attachment, within the segment size */
        for (v = vt_first_end_after(&mm->vt, addr); v && v->end - addr <= GP_ALIGN(size);) {
            if (v->bk && v->bk->kind == MB_SYSV && v->bk->shm_id == id && v->start - addr == v->pgoff) {
                uint64_t s = v->start, e = v->end;
                mm_unmap_locked(mm, s, e, &iv);
                v = vt_first_end_after(&mm->vt, e);
            } else {
                v = vt_next(&mm->vt, v);
            }
        }
    }
    mm_unlock(mm);
    mm_inval_flush(&iv);
    return r;
}

static void fill_ds(const struct seg *s, struct lx_shmid64_ds *d)
{
    memset(d, 0, sizeof *d);
    d->shm_perm.key = s->key;
    d->shm_perm.uid = s->uid;
    d->shm_perm.gid = s->gid;
    d->shm_perm.cuid = s->cuid;
    d->shm_perm.cgid = s->cgid;
    d->shm_perm.mode = s->mode;
    d->shm_perm.seq = (uint16_t)s->seq;
    d->shm_segsz = s->size;
    d->shm_atime = s->atime;
    d->shm_dtime = s->dtime;
    d->shm_ctime = s->ctime;
    d->shm_cpid = s->cpid;
    d->shm_lpid = s->lpid;
    d->shm_nattch = (uint64_t)s->nattch;
}

static int max_idx(void)
{
    int m = -1;
    for (int i = 0; i < SHM_SLOTS; i++)
        if (T->seg[i].used) m = i;
    return m;
}

static int owner_or_admin(lkd_task *t, const struct seg *s)
{
    uint32_t uid, gid;
    creds(t, &uid, &gid);
    return uid == s->uid || uid == s->cuid || mm_capable(t, LKD_CAP_SYS_ADMIN);
}

LKD_SYSCALL(shmctl)
{
    int shmid = LKD_ARGI(0), cmd = LKD_ARGI(1);
    uint64_t buf = LKD_ARGP(2);
    if (cmd < 0 || shmid < 0 || !T) return -LX_EINVAL;
    switch (cmd) {
    case LX_IPC_INFO: {
        struct lx_shminfo64 in = { SHMMAX, 1, SHM_SLOTS, SHM_SLOTS, SHMALL, { 0 } };
        tlock();
        int m = max_idx();
        tunlock();
        if (lkd_user_write(buf, &in, sizeof in)) return -LX_EFAULT;
        return m < 0 ? 0 : m;
    }
    case LX_SHM_INFO: {
        struct lx_shm_info in = { 0 };
        tlock();
        for (int i = 0; i < SHM_SLOTS; i++)
            if (T->seg[i].used) { in.used_ids++; in.shm_tot += HP_UP(T->seg[i].size) / GP; }
        int m = max_idx();
        tunlock();
        /* shm_rss and shm_swp would need host residency of objects not mapped here: reported as 0 */
        if (lkd_user_write(buf, &in, sizeof in)) return -LX_EFAULT;
        return m < 0 ? 0 : m;
    }
    case LX_SHM_STAT:
    case LX_SHM_STAT_ANY:
    case LX_IPC_STAT: {
        struct lx_shmid64_ds d;
        int64_t r;
        tlock();
        struct seg *s = cmd == LX_IPC_STAT ? by_id(shmid)
                        : (shmid < SHM_SLOTS && T->seg[shmid].used ? &T->seg[shmid] : NULL);
        if (!s) r = -LX_EINVAL;
        else if (cmd != LX_SHM_STAT_ANY && ipcperms(t, s, 0444)) r = -LX_EACCES;
        else { fill_ds(s, &d); r = cmd == LX_IPC_STAT ? 0 : seg_id(s); }
        tunlock();
        if (r < 0) return r;
        if (lkd_user_write(buf, &d, sizeof d)) return -LX_EFAULT;
        return r;
    }
    case LX_IPC_SET: {
        struct lx_shmid64_ds d;
        if (lkd_user_read(buf, &d, sizeof d)) return -LX_EFAULT;
        int64_t r = 0;
        tlock();
        struct seg *s = by_id(shmid);
        if (!s) r = -LX_EINVAL;
        else if (!owner_or_admin(t, s)) r = -LX_EPERM;
        else {
            s->uid = d.shm_perm.uid;
            s->gid = d.shm_perm.gid;
            s->mode = (s->mode & ~0777u) | (d.shm_perm.mode & 0777u);
            s->ctime = now_s();
        }
        tunlock();
        return r;
    }
    case LX_IPC_RMID: {
        int64_t r = 0;
        tlock();
        struct seg *s = by_id(shmid);
        if (!s) r = -LX_EINVAL;
        else if (!owner_or_admin(t, s)) r = -LX_EPERM;
        else if (s->nattch) { s->mode |= LX_SHM_DEST; s->key = LX_IPC_PRIVATE; }
        else destroy(s);
        tunlock();
        return r;
    }
    case LX_SHM_LOCK:
    case LX_SHM_UNLOCK: {
        int64_t r = 0;
        tlock();
        struct seg *s = by_id(shmid);
        if (!s) r = -LX_EINVAL;
        else if (!mm_capable(t, LKD_CAP_IPC_LOCK)) {
            uint32_t uid, gid;
            creds(t, &uid, &gid);
            if (uid != s->uid && uid != s->cuid) r = -LX_EPERM;
            else if (cmd == LX_SHM_LOCK && t && t->proc && t->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur == 0) r = -LX_EPERM;
        }
        if (!r) {
            /* Linux only marks the pages unevictable; LKD records the flag (the host may still page out) */
            if (cmd == LX_SHM_LOCK) s->mode |= LX_SHM_LOCKED;
            else s->mode &= ~(uint32_t)LX_SHM_LOCKED;
        }
        tunlock();
        return r;
    }
    }
    return -LX_EINVAL;
}

void mm_shm_register(void)
{
    LKD_REGISTER(shmget);
    LKD_REGISTER(shmat);
    LKD_REGISTER(shmdt);
    LKD_REGISTER(shmctl);
}
