/* test_fs_memfd.c: memfd_create(2) and file seals (memfd_create(2), fcntl(2) "File Sealing", mm/memfd.c
 * memfd_add_seals and memfd_create of Linux 6.12, mm/shmem.c shmem_write_begin/shmem_setattr/
 * shmem_fallocate). */
#include <dirent.h>

#include "test_fs_util.h"
#include "mm.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

#define SEAL_SEAL LX_F_SEAL_SEAL
#define SEAL_SHRINK LX_F_SEAL_SHRINK
#define SEAL_GROW LX_F_SEAL_GROW
#define SEAL_WRITE LX_F_SEAL_WRITE
#define SEAL_FWRITE LX_F_SEAL_FUTURE_WRITE
#define SEAL_EXEC LX_F_SEAL_EXEC

static int64_t mfd(const char *name, uint32_t flags) { return SC(T, memfd_create, P(name), flags); }

LT_TEST(create_and_identity)
{
    int64_t fd = mfd("wine-shm", LX_MFD_CLOEXEC);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFL) & (LX_O_ACCMODE | LX_O_LARGEFILE), LX_O_RDWR | LX_O_LARGEFILE);
    struct lx_stat st;
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode, LX_S_IFREG | 0777);
    LT_EQ(st.st_nlink, 1);
    LT_EQ(st.st_size, 0);
    lkd_fdobj *o = lkd_fd_get(T, (int)fd, NULL);
    char txt[64];
    int n = lkd_fs_describe(o, txt, sizeof txt);
    txt[n > 0 ? n : 0] = 0;
    LT_STREQ(txt, "/memfd:wine-shm (deleted)");
    LT_CHECK(o->ops->mmap_fd && o->ops->mmap_fd(o) >= 0);          /* mm can map it */
    LT_EQ(lkd_fs_memfd_seals(o), SEAL_SEAL);                         /* no ALLOW_SEALING */
    lkd_fdobj_put(o);
    LT_EQ(SC(T, write, fd, P("0123456789"), 10), 10);
    LT_EQ(SC(T, lseek, fd, 2, LX_SEEK_SET), 2);
    char b[8];
    LT_EQ(SC(T, read, fd, P(b), 3), 3);
    LT_MEMEQ(b, "234", 3);
    LT_EQ(SC(T, ftruncate, fd, 4096), 0);
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_size, 4096);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), SEAL_SEAL);
    LT_ERRNO(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_WRITE), LX_EPERM);
    struct lx_statfs sf;
    LT_EQ(SC(T, fstatfs, fd, P(&sf)), 0);
    LT_EQ(sf.f_type, 0x01021994);                                    /* TMPFS_MAGIC */
    LT_EQ(SC(T, lseek, fd, 100, LX_SEEK_DATA), 100);
    LT_EQ(SC(T, lseek, fd, 100, LX_SEEK_HOLE), 4096);
    SC(T, close, fd);
}

LT_TEST(create_errors)
{
    char longname[260];
    memset(longname, 'n', sizeof longname);
    longname[250] = 0;                                               /* 250 > MFD_NAME_MAX_LEN 249 */
    LT_ERRNO(mfd(longname, 0), LX_EINVAL);
    longname[249] = 0;
    int64_t fd = mfd(longname, 0);
    LT_CHECK(fd >= 0);
    SC(T, close, fd);
    LT_ERRNO(mfd("x", 0x100), LX_EINVAL);
    LT_ERRNO(mfd("x", LX_MFD_EXEC | LX_MFD_NOEXEC_SEAL), LX_EINVAL);
    LT_ERRNO(mfd("x", LX_MFD_HUGETLB), LX_EINVAL);                  /* no hugetlbfs on this host */
    LT_ERRNO(SC(T, memfd_create, 0, 0), LX_EFAULT);
}

/* F_SEAL_SHRINK / F_SEAL_GROW on ftruncate; GROW on writes past EOF (page chunks). */
LT_TEST(shrink_grow)
{
    int64_t fd = mfd("sg", LX_MFD_ALLOW_SEALING);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), 0);
    LT_EQ(SC(T, ftruncate, fd, 5000), 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_SHRINK), 0);
    LT_ERRNO(SC(T, ftruncate, fd, 100), LX_EPERM);
    LT_EQ(SC(T, ftruncate, fd, 6000), 0);
    LT_EQ(SC(T, ftruncate, fd, 5000), -LX_EPERM);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_GROW), 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), SEAL_SHRINK | SEAL_GROW);
    LT_ERRNO(SC(T, ftruncate, fd, 7000), LX_EPERM);
    LT_EQ(SC(T, ftruncate, fd, 6000), 0);                            /* same size: allowed */
    static char buf[8192];
    memset(buf, 'w', sizeof buf);
    /* size 6000: a write of 8000 at 0 stops after the 4096-byte page chunk [0,4096); the next chunk
     * [4096,8000) ends past EOF (shmem_write_begin -EPERM) */
    LT_EQ(SC(T, pwrite64, fd, P(buf), 8000, 0), 4096);
    LT_ERRNO(SC(T, pwrite64, fd, P(buf), 2000, 4096), LX_EPERM);
    LT_EQ(SC(T, pwrite64, fd, P(buf), 1904, 4096), 1904);            /* ends exactly at EOF */
    LT_ERRNO(SC(T, fallocate, fd, 0, 0, 7000), LX_EPERM);
    LT_EQ(SC(T, fallocate, fd, 0, 0, 100), 0);
    LT_ERRNO(SC(T, fcntl, fd, LX_F_ADD_SEALS, 0x40), LX_EINVAL);     /* unknown seal bit */
    SC(T, close, fd);
}

/* F_SEAL_WRITE: write, pwrite, writev, hole punching -EPERM; reads and size changes allowed unless
 * sealed separately. F_SEAL_SEAL blocks further seals. */
LT_TEST(write_seal)
{
    int64_t fd = mfd("w", LX_MFD_ALLOW_SEALING);
    LT_EQ(SC(T, write, fd, P("data"), 4), 4);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_WRITE), 0);
    LT_ERRNO(SC(T, write, fd, P("x"), 1), LX_EPERM);
    LT_ERRNO(SC(T, pwrite64, fd, P("x"), 1, 0), LX_EPERM);
    struct lx_iovec iov = { P("y"), 1 };
    LT_ERRNO(SC(T, writev, fd, P(&iov), 1), LX_EPERM);
    LT_ERRNO(SC(T, fallocate, fd, 3, 0, 2), LX_EPERM);
    char b[4];
    LT_EQ(SC(T, pread64, fd, P(b), 4, 0), 4);
    LT_MEMEQ(b, "data", 4);
    LT_EQ(SC(T, ftruncate, fd, 8), 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_SEAL), 0);
    LT_ERRNO(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_GROW), LX_EPERM);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), SEAL_WRITE | SEAL_SEAL);
    SC(T, close, fd);
    fd = mfd("fw", LX_MFD_ALLOW_SEALING);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_FWRITE), 0);
    LT_ERRNO(SC(T, write, fd, P("x"), 1), LX_EPERM);
    SC(T, close, fd);
}

/* EBUSY: adding F_SEAL_WRITE while a writable shared mapping exists (mapping_deny_writable). The mm
 * query is simulated through the fs hook. */
static int fake_wsm_answer;
static int fake_wsm(struct lkd_mm *mm, struct lkd_fdobj *f) { (void)mm; (void)f; return fake_wsm_answer; }
LT_TEST(write_seal_busy)
{
    int (*saved)(struct lkd_mm *, struct lkd_fdobj *) = lkd_fs_wsm_query;
    lkd_fs_wsm_query = fake_wsm;
    int64_t fd = mfd("busy", LX_MFD_ALLOW_SEALING);
    fake_wsm_answer = 1;
    LT_ERRNO(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_WRITE), LX_EBUSY);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), 0);                      /* nothing added */
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_FWRITE), 0);         /* FUTURE_WRITE does not wait */
    fake_wsm_answer = 0;
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_WRITE), 0);
    lkd_fs_wsm_query = saved;
    SC(T, close, fd);
}

/* Seals need a writable description (-EPERM), only memfds are sealable (-EINVAL). */
LT_TEST(seal_permissions)
{
    int64_t fd = mfd("perm", LX_MFD_ALLOW_SEALING);
    lkd_fdobj *o = lkd_fd_get(T, (int)fd, NULL), *ro = NULL;
    LT_EQ(lkd_fdobj_reopen(T, o, LX_O_RDONLY, &ro), 0);
    int rfd = lkd_fd_alloc(T, ro, 0, 0);
    LT_ERRNO(SC(T, fcntl, rfd, LX_F_ADD_SEALS, SEAL_GROW), LX_EPERM);
    LT_ERRNO(SC(T, write, rfd, P("x"), 1), LX_EBADF);
    LT_EQ(SC(T, write, fd, P("abcdef"), 6), 6);
    char b[4];
    LT_EQ(SC(T, read, rfd, P(b), 3), 3);                              /* own offset from 0 */
    LT_MEMEQ(b, "abc", 3);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_CUR), 6);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_SHRINK), 0);
    LT_EQ(SC(T, fcntl, rfd, LX_F_GET_SEALS), SEAL_SHRINK);           /* same inode, same seals */
    lkd_fdobj_put(ro);
    lkd_fdobj_put(o);
    int64_t f = SC(T, openat, AT_CWD, P("/plain"), LX_O_RDWR | LX_O_CREAT, 0644);
    LT_ERRNO(SC(T, fcntl, f, LX_F_ADD_SEALS, SEAL_WRITE), LX_EINVAL);
    LT_ERRNO(SC(T, fcntl, f, LX_F_GET_SEALS), LX_EINVAL);
    SC(T, close, f);
    SC(T, close, rfd);
    SC(T, close, fd);
}

/* MFD_NOEXEC_SEAL: mode 0666 and F_SEAL_EXEC; chmod adding an exec bit -EPERM (shmem_setattr).
 * F_SEAL_EXEC on an executable memfd implies SHRINK|GROW|WRITE|FUTURE_WRITE (memfd_add_seals). */
LT_TEST(exec_seal)
{
    int64_t fd = mfd("nx", LX_MFD_NOEXEC_SEAL);
    struct lx_stat st;
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode, LX_S_IFREG | 0666);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), SEAL_EXEC);
    LT_ERRNO(SC(T, fchmod, fd, 0755), LX_EPERM);
    LT_EQ(SC(T, fchmod, fd, 0644), 0);
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode & 07777, 0644);
    SC(T, close, fd);
    fd = mfd("x", LX_MFD_ALLOW_SEALING | LX_MFD_EXEC);
    LT_EQ(SC(T, fcntl, fd, LX_F_ADD_SEALS, SEAL_EXEC), 0);
    LT_EQ(SC(T, fcntl, fd, LX_F_GET_SEALS), SEAL_EXEC | SEAL_SHRINK | SEAL_GROW | SEAL_WRITE | SEAL_FWRITE);
    LT_ERRNO(SC(T, write, fd, P("x"), 1), LX_EPERM);
    SC(T, close, fd);
}

/* Seals live in shared memory: a seal added by a forked child is seen by the parent. */
static int64_t g_fd;
static void child_seals(void *arg)
{
    (void)arg;
    lkd_set_current(T);
    _exit(SC(T, fcntl, g_fd, LX_F_ADD_SEALS, SEAL_GROW) == 0 ? 0 : 1);
}
LT_TEST(seals_across_fork)
{
    g_fd = mfd("fork", LX_MFD_ALLOW_SEALING);
    LT_EQ(lt_run_child(child_seals, NULL, 10), 0);
    LT_EQ(SC(T, fcntl, g_fd, LX_F_GET_SEALS), SEAL_GROW);
    LT_ERRNO(SC(T, ftruncate, g_fd, 10), LX_EPERM);
    SC(T, close, g_fd);
}

/* No backing file is left behind: memfds are unlinked host files in the /dev/shm host directory. */
LT_TEST(no_leftover)
{
    const char *shm = lkd_fs_host_shm_dir();
    LT_CHECK(shm && shm[0]);
    DIR *d = opendir(shm);
    int left = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) if (!strncmp(e->d_name, ".lkd-memfd.", 11)) left++;
    if (d) closedir(d);
    LT_EQ(left, 0);
    struct lx_stat st;
    LT_EQ(SC(T, stat, P("/dev/shm"), P(&st)), 0);                    /* bound at /dev/shm */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFDIR);
    LT_EQ(SC(T, stat, P("/tmp"), P(&st)), 0);
    LT_EQ(st.st_mode & 07777, 01777);
}

/* mmap checks owned by the file (mm/mmap.c do_mmap, memfd_check_seals_mmap) and fdinfo basics. */
static int mcheck(int64_t fd, int prot, uint32_t flags)
{
    lkd_fdobj *o = lkd_fd_get(T, (int)fd, NULL);
    int r = lkd_fs_mmap_check(o, prot, flags);
    lkd_fdobj_put(o);
    return r;
}
/* mprotect(PROT_WRITE) of a read-only MAP_SHARED mapping of a memfd that got F_SEAL_FUTURE_WRITE: EACCES
 * (seal_check_write removes VM_MAYWRITE, mm/memfd.c; request fs-mm point 2). Before the seal, the same
 * mprotect is allowed (the description is O_RDWR). */
LT_TEST(sealed_memfd_mprotect)
{
    lkd_mm *saved = T->mm;
    T->mm = lkd_mm_create();
    LT_CHECK(T->mm != NULL);
    if (!T->mm) { T->mm = saved; return; }
    int64_t m = mfd("prot", LX_MFD_ALLOW_SEALING);
    LT_EQ(SC(T, ftruncate, m, 16384), 0);
    int64_t a = SC(T, mmap, 0, 16384, LX_PROT_READ, LX_MAP_SHARED, m, 0);
    LT_CHECK(a > 0);
    LT_EQ(SC(T, mprotect, a, 16384, LX_PROT_READ | LX_PROT_WRITE), 0);
    LT_EQ(SC(T, mprotect, a, 16384, LX_PROT_READ), 0);
    LT_EQ(SC(T, fcntl, m, LX_F_ADD_SEALS, SEAL_FWRITE), 0);
    LT_ERRNO(SC(T, mprotect, a, 16384, LX_PROT_READ | LX_PROT_WRITE), LX_EACCES);
    LT_EQ(SC(T, mprotect, a, 16384, LX_PROT_READ), 0);
    SC(T, munmap, a, 16384);
    SC(T, close, m);
    lkd_mm_put(T->mm);
    T->mm = saved;
}

/* Request wine-7: mmap(addr, 4096, PROT_READ|PROT_EXEC, MAP_SHARED|MAP_FIXED_NOREPLACE, memfd, 0) at an address
 * 0x3000 into its 16 KiB host page (the EAC module of the game). Linux accepts it. The memfd takes the host
 * phase 0x3000 at this first mapping; the guest sees no difference: content, size, read/write through the
 * descriptor and through other views agree. A later view that needs another phase is EINVAL. */
LT_TEST(phased_shared_mapping)
{
    lkd_mm *saved = T->mm;
    T->mm = lkd_mm_create();
    LT_CHECK(T->mm != NULL);
    if (!T->mm) { T->mm = saved; return; }
    int64_t m = mfd("eac", LX_MFD_CLOEXEC);
    uint8_t pat[6000];
    for (int i = 0; i < (int)sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 1);
    LT_EQ(SC(T, write, m, P(pat), sizeof pat), (int64_t)sizeof pat);    /* content before the first mapping */
    LT_EQ(SC(T, ftruncate, m, 0x2000), 0);
    uint64_t A = lkd_mm_window_guest_lo() + 0x30000000 + 0x3000;           /* 0x3000 into its host page */
    int64_t a = SC(T, mmap, A, 0x1000, LX_PROT_READ | LX_PROT_EXEC, LX_MAP_SHARED | LX_MAP_FIXED_NOREPLACE, m, 0);
    LT_EQ(a, (int64_t)A);
    if (a == (int64_t)A) LT_MEMEQ(lkd_g2h(A), pat, 0x1000);
    /* fstat, lseek and pread are unchanged for the guest */
    struct lx_stat st;
    LT_EQ(SC(T, fstat, m, P(&st)), 0);
    LT_EQ(st.st_size, 0x2000);
    LT_EQ(SC(T, lseek, m, 0, LX_SEEK_END), 0x2000);
    uint8_t b[16];
    LT_EQ(SC(T, pread64, m, P(b), 16, 0x1000), 16);
    LT_MEMEQ(b, pat + 0x1000, 16);
    /* a RW view at an address chosen by mm: congruent with offset + phase; writes are shared both ways */
    int64_t w = SC(T, mmap, 0, 0x2000, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED, m, 0);
    LT_CHECK(w > 0);
    LT_EQ((uint64_t)w & 0x3fff, 0x3000);
    if (w > 0 && a == (int64_t)A) {
        ((uint8_t *)lkd_g2h((uint64_t)w))[5] = 0xee;
        LT_EQ(((uint8_t *)lkd_g2h(A))[5], 0xee);
        LT_EQ(SC(T, pread64, m, P(b), 1, 5), 1);
        LT_EQ(b[0], 0xee);
        uint8_t z = 0x42;
        LT_EQ(SC(T, pwrite64, m, P(&z), 1, 6), 1);
        LT_EQ(((uint8_t *)lkd_g2h((uint64_t)w))[6], 0x42);
        LT_EQ(((uint8_t *)lkd_g2h(A))[6], 0x42);
    }
    /* another phase for the same object: visible EINVAL (the content cannot sit at two positions) */
    uint64_t B = lkd_mm_window_guest_lo() + 0x31000000;                   /* 16 KiB aligned: phase 0 needed */
    LT_ERRNO(SC(T, mmap, B, 0x1000, LX_PROT_READ, LX_MAP_SHARED | LX_MAP_FIXED, m, 0), LX_EINVAL);
    /* same phase, other offset: fine (B + 0x3000 + 0x1000 holds offset 0x1000) */
    LT_EQ(SC(T, mmap, B + 0x4000, 0x1000, LX_PROT_READ, LX_MAP_SHARED | LX_MAP_FIXED, m, 0x1000), (int64_t)(B + 0x4000));
    /* a private copy-on-write view reads the right bytes too */
    int64_t pv = SC(T, mmap, 0, 0x2000, LX_PROT_READ, LX_MAP_PRIVATE, m, 0);
    LT_CHECK(pv > 0);
    if (pv > 0) LT_EQ(((uint8_t *)lkd_g2h((uint64_t)pv))[6], 0x42);
    /* the raw host descriptor cannot leave LKD's offset handling any more (SCM_RIGHTS, exec) */
    lkd_fdobj *o = lkd_fd_get(T, (int)m, NULL);
    LT_ERRNO(lkd_fs_pin_raw(o), LX_EOPNOTSUPP);
    lkd_fdobj_put(o);
    /* a memfd that was never mapped is pinned at phase 0 and still maps at congruent addresses only */
    int64_t m2 = mfd("raw", 0);
    LT_EQ(SC(T, ftruncate, m2, 0x1000), 0);
    o = lkd_fd_get(T, (int)m2, NULL);
    LT_EQ(lkd_fs_pin_raw(o), 0);
    lkd_fdobj_put(o);
    LT_ERRNO(SC(T, mmap, A + 0x10000, 0x1000, LX_PROT_READ, LX_MAP_SHARED | LX_MAP_FIXED, m2, 0), LX_EINVAL);
    SC(T, close, m2);
    SC(T, close, m);
    lkd_mm_put(T->mm);
    T->mm = saved;
}

LT_TEST(mmap_checks_and_fdinfo)
{
    const int RW = LX_PROT_READ | LX_PROT_WRITE;
    int64_t m = mfd("map", LX_MFD_ALLOW_SEALING);
    LT_EQ(mcheck(m, RW, LX_MAP_SHARED), 0);
    LT_EQ(SC(T, fcntl, m, LX_F_ADD_SEALS, SEAL_WRITE), 0);
    LT_ERRNO(mcheck(m, RW, LX_MAP_SHARED), LX_EPERM);
    LT_EQ(mcheck(m, RW, LX_MAP_PRIVATE), 0);                       /* private copy-on-write stays allowed */
    LT_EQ(mcheck(m, LX_PROT_READ, LX_MAP_SHARED), 0);
    int64_t ro = SC(T, openat, AT_CWD, P("/plain"), LX_O_RDONLY | LX_O_CREAT, 0644);
    LT_ERRNO(mcheck(ro, RW, LX_MAP_SHARED), LX_EACCES);
    LT_EQ(mcheck(ro, RW, LX_MAP_PRIVATE), 0);
    int64_t wo = SC(T, openat, AT_CWD, P("/plain"), LX_O_WRONLY, 0);
    LT_ERRNO(mcheck(wo, LX_PROT_READ, LX_MAP_PRIVATE), LX_EACCES);
    int64_t ap = SC(T, openat, AT_CWD, P("/plain"), LX_O_RDWR | LX_O_APPEND, 0);
    LT_ERRNO(mcheck(ap, RW, LX_MAP_SHARED), LX_EACCES);
    int64_t pa = SC(T, openat, AT_CWD, P("/plain"), LX_O_PATH, 0);
    LT_ERRNO(mcheck(pa, LX_PROT_READ, LX_MAP_PRIVATE), LX_EBADF);
    int32_t p[2];
    SC(T, pipe, P(p));
    LT_ERRNO(mcheck(p[0], LX_PROT_READ, LX_MAP_PRIVATE), LX_ENODEV);
    int64_t d = SC(T, openat, AT_CWD, P("/"), LX_O_RDONLY | LX_O_DIRECTORY, 0);
    LT_ERRNO(mcheck(d, LX_PROT_READ, LX_MAP_PRIVATE), LX_ENODEV);
    LT_EQ(SC(T, write, m, P("x"), 1), -LX_EPERM);
    LT_EQ(SC(T, lseek, ap, 0, LX_SEEK_END) >= 0, 1);
    lkd_fdobj *o = lkd_fd_get(T, (int)ro, NULL);
    int64_t pos = -1;
    uint32_t fl = 0;
    int mid = 0;
    LT_EQ(lkd_fs_fdinfo_basic(T, o, &pos, &fl, &mid), 0);
    LT_EQ(pos, 0);
    LT_EQ(fl, LX_O_RDONLY | LX_O_LARGEFILE);
    LT_CHECK(mid > 0);
    lkd_fdobj_put(o);
    int64_t fds[] = { m, ro, wo, ap, pa, p[0], p[1], d };
    for (int i = 0; i < 8; i++) SC(T, close, fds[i]);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "memfd");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(create_and_identity);
    LT_RUN(create_errors);
    LT_RUN(shrink_grow);
    LT_RUN(write_seal);
    LT_RUN(write_seal_busy);
    LT_RUN(seal_permissions);
    LT_RUN(exec_seal);
    LT_RUN(seals_across_fork);
    LT_RUN(sealed_memfd_mprotect);
    LT_RUN(phased_shared_mapping);
    LT_RUN(mmap_checks_and_fdinfo);
    LT_RUN(no_leftover);
    rm_rf(ROOT);
    return lt_finish();
}
