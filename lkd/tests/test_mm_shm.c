/* test_mm_shm.c: System V shared memory. Linux references: shmget(2), shmat(2)/shmdt(2), shmctl(2),
 * proc_pid_maps(5) ("/SYSV%08x (deleted)"), ipc/shm.c and ipc/util.c ipcperms of v6.12. */
#include <sys/mman.h>
#include <sys/wait.h>

#include "ltest.h"
#include "fake_engine.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

#define IPC_PRIVATE 0
#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_RMID 0
#define IPC_SET 1
#define IPC_STAT 2
#define IPC_INFO 3
#define SHM_STAT 13
#define SHM_INFO 14
#define SHM_LOCK 11
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_DEST 01000
#define SHM_LOCKED 02000

struct ds { /* struct shmid64_ds, x86-64 */
    int32_t key; uint32_t uid, gid, cuid, cgid, mode; uint16_t seq, pad; uint64_t u1, u2;
    uint64_t segsz; int64_t atime, dtime, ctime; int32_t cpid, lpid; uint64_t nattch, u4, u5;
};

static lkd_task *T;
static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define SHMGET(k, s, f) sc(LKD_NR_shmget, (uint64_t)(int64_t)(k), (s), (f), 0, 0, 0)
#define SHMAT(i, a, f) sc(LKD_NR_shmat, (i), (a), (f), 0, 0, 0)
#define SHMDT(a) sc(LKD_NR_shmdt, (a), 0, 0, 0, 0, 0)
#define SHMCTL(i, c, b) sc(LKD_NR_shmctl, (i), (c), (uint64_t)(uintptr_t)(b), 0, 0, 0)
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }

static void up(void)
{
    if (!T) { fake_engine_init(); T = fake_make_task(); }
    T->mm = lkd_mm_create();
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }

struct child_arg { int id; };
static void attach_child(void *p)
{
    struct child_arg *a = p;
    int64_t at = SHMAT(a->id, 0, 0);
    if (at <= 0) _exit(1);
    H(at)[100] = 0x99;          /* seen by the parent through the segment */
    if (H(at)[0] != 0x42) _exit(2);
    if (SHMDT(at)) _exit(3);
    _exit(0);
}

LT_TEST(create_attach_stat)
{
    up();
    int64_t id = SHMGET(IPC_PRIVATE, 10000, IPC_CREAT | 0600);
    LT_CHECK(id >= 0);
    int64_t at = SHMAT(id, 0, 0);
    LT_CHECK(at > 0);
    LT_EQ(at & 0x3fff, 0);
    LT_EQ(H(at)[0], 0);              /* "initialized to zero" */
    H(at)[0] = 0x42;
    struct ds d;
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_EQ(d.segsz, 10000);
    LT_EQ(d.nattch, 1);
    LT_EQ(d.mode & 0777, 0600);
    LT_EQ(d.cpid, T->proc->pid);
    LT_EQ(d.uid, T->proc->cred.euid);
    LT_CHECK(d.ctime > 0 && d.atime > 0);
    /* maps: "/SYSV%08x (deleted)", shared, inode = shmid */
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, at, 0, &vi), 0);
    LT_STREQ(vi.name, "/SYSV00000000");
    LT_EQ(vi.deleted, 1);
    LT_EQ(vi.ino, (uint64_t)id);
    LT_EQ(vi.end - vi.start, 0x3000);   /* PAGE_ALIGN(10000) */
    LT_EQ(vi.flags & LX_MAP_TYPE, LX_MAP_SHARED);
    /* another process attaches by id and writes */
    struct child_arg ca = { (int)id };
    LT_EQ(lt_run_child(attach_child, &ca, 10), 0);
    LT_EQ(H(at)[100], 0x99);
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_EQ(d.nattch, 1);
    LT_EQ(SHMDT(at), 0);
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_EQ(d.nattch, 0);
    LT_CHECK(d.dtime > 0);
    LT_ERRNO(SHMDT(at), LX_EINVAL);    /* nothing attached there any more */
    LT_EQ(SHMCTL(id, IPC_RMID, NULL), 0);
    LT_ERRNO(SHMCTL(id, IPC_STAT, &d), LX_EINVAL);
    LT_ERRNO(SHMAT(id, 0, 0), LX_EINVAL);
    down();
}

LT_TEST(keys_and_errors)
{
    up();
    int32_t key = 0x4c4b4400 + (getpid() & 0xff);
    LT_ERRNO(SHMGET(key, 4096, 0600), LX_ENOENT);                       /* no IPC_CREAT */
    int64_t id = SHMGET(key, 4096, IPC_CREAT | 0640);
    LT_CHECK(id >= 0);
    LT_EQ(SHMGET(key, 4096, 0), id);                                    /* found by key */
    LT_EQ(SHMGET(key, 100, 0), id);                                     /* smaller size is fine */
    LT_ERRNO(SHMGET(key, 8192, 0), LX_EINVAL);                          /* larger than the segment */
    LT_ERRNO(SHMGET(key, 4096, IPC_CREAT | IPC_EXCL | 0600), LX_EEXIST);
    LT_ERRNO(SHMGET(IPC_PRIVATE, 0, IPC_CREAT | 0600), LX_EINVAL);      /* size < SHMMIN */
    LT_ERRNO(SHMAT(-1, 0, 0), LX_EINVAL);
    LT_ERRNO(SHMAT(id, lkd_mm_window_guest_lo() + 0x40000123, 0), LX_EINVAL); /* not SHMLBA aligned */
    /* SHM_RND rounds down to SHMLBA; a fixed address must also be a 16 KiB host page (documented) */
    uint64_t want = lkd_mm_window_guest_lo() + 0x40000000;
    LT_EQ(SHMAT(id, want + 0x123, SHM_RND), want);
    LT_ERRNO(SHMAT(id, want + 0x8000 + 0x1000, 0), LX_EINVAL);
    /* SHM_RDONLY maps read-only */
    int64_t ro = SHMAT(id, 0, SHM_RDONLY);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, ro, 0, &vi), 0);
    LT_EQ(vi.prot, LX_PROT_READ);
    LT_ERRNO(lkd_mm_protect(T->mm, ro, 4096, LX_PROT_READ | LX_PROT_WRITE), LX_EACCES);
    /* without SHM_REMAP an occupied fixed address is EINVAL */
    LT_ERRNO(SHMAT(id, want, 0), LX_EINVAL);
    struct ds d;
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_EQ(d.nattch, 2);
    /* IPC_RMID while attached: marked SHM_DEST, key becomes private, destroyed at the last detach */
    LT_EQ(SHMCTL(id, IPC_RMID, NULL), 0);
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_CHECK(d.mode & SHM_DEST);
    LT_EQ(d.key, 0);
    LT_ERRNO(SHMGET(key, 4096, 0), LX_ENOENT);
    LT_EQ(SHMDT(want), 0);
    LT_EQ(SHMDT(ro), 0);
    LT_ERRNO(SHMCTL(id, IPC_STAT, &d), LX_EINVAL);
    LT_ERRNO(SHMCTL(id, 99, &d), LX_EINVAL);
    down();
}

LT_TEST(permissions_and_info)
{
    up();
    int64_t id = SHMGET(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    struct ds d;
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    d.mode = 0000;
    LT_EQ(SHMCTL(id, IPC_SET, &d), 0);
    LT_ERRNO(SHMAT(id, 0, 0), LX_EACCES);            /* ipcperms: owner bits 0 */
    LT_ERRNO(SHMCTL(id, IPC_STAT, &d), LX_EACCES);
    uint32_t saved = T->proc->cred.euid;
    T->proc->cred.euid = saved + 1;                  /* neither owner nor creator */
    LT_ERRNO(SHMCTL(id, IPC_RMID, NULL), LX_EPERM);
    LT_ERRNO(SHMCTL(id, IPC_SET, &d), LX_EPERM);
    T->proc->cred.euid = saved;
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
    LT_EQ(SHMCTL(id, SHM_LOCK, NULL), 0);
    d.mode = 0600;
    LT_EQ(SHMCTL(id, IPC_SET, &d), 0);
    LT_EQ(SHMCTL(id, IPC_STAT, &d), 0);
    LT_CHECK(d.mode & SHM_LOCKED);
    uint64_t info[9];
    int64_t mx = SHMCTL(0, IPC_INFO, info);
    LT_CHECK(mx >= id % 32768);
    LT_EQ(info[1], 1);                               /* shmmin */
    uint64_t si[6];
    LT_CHECK(SHMCTL(0, SHM_INFO, si) >= 0);
    LT_CHECK((int32_t)si[0] >= 1);                   /* used_ids */
    LT_EQ(SHMCTL(id % 32768, SHM_STAT, &d), id);     /* SHM_STAT takes an index, returns the id */
    LT_EQ(SHMCTL(id, IPC_RMID, NULL), 0);
    down();
}

int main(void)
{
    LT_RUN(create_attach_stat);
    LT_RUN(keys_and_errors);
    LT_RUN(permissions_and_info);
    return lt_finish();
}
