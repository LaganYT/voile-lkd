/* test_fs_io.c: read/write family, stat and statx layouts (byte oracle), getdents formats, namespace
 * operations, attributes, xattrs. Linux references: fs/read_write.c, fs/stat.c cp_new_stat (x86-64
 * struct stat, 144 bytes), include/uapi/linux/stat.h (statx, 256 bytes), fs/readdir.c (filldir,
 * filldir64), fs/namei.c, fs/open.c, fs/xattr.c, man 2 of each call. */
#include <sys/xattr.h>

#include "test_fs_util.h"

static char ROOT[PATH_MAX];
static lkd_task *T;

static int64_t gopen(const char *p, uint32_t fl, uint32_t mode) { return SC(T, openat, AT_CWD, P(p), fl, mode); }

static uint64_t rd64(const uint8_t *b, int off) { uint64_t v; memcpy(&v, b + off, 8); return v; }
static uint32_t rd32(const uint8_t *b, int off) { uint32_t v; memcpy(&v, b + off, 4); return v; }
static uint16_t rd16(const uint8_t *b, int off) { uint16_t v; memcpy(&v, b + off, 2); return v; }

/* x86-64 struct stat (arch/x86/include/uapi/asm/stat.h): st_dev 0, st_ino 8, st_nlink 16, st_mode 24,
 * st_uid 28, st_gid 32, __pad0 36, st_rdev 40, st_size 48, st_blksize 56, st_blocks 64, st_atime 72,
 * st_atime_nsec 80, st_mtime 88, st_mtime_nsec 96, st_ctime 104, st_ctime_nsec 112, __unused 120..143. */
LT_TEST(stat_layout_oracle)
{
    int64_t fd = gopen("/data", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0640);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, write, fd, P("hello world"), 11), 11);
    struct lx_timespec ts[2] = { { 1000000000, 5 }, { 1234567890, 123456789 } };
    LT_EQ(SC(T, utimensat, AT_CWD, P("/data"), P(ts), 0), 0);
    uint8_t b[160];
    memset(b, 0xAA, sizeof b);
    LT_EQ(SC(T, fstat, fd, P(b)), 0);
    LT_EQ(rd32(b, 24), 0100640);            /* S_IFREG | 0640 (umask 022 does not touch 0640) */
    LT_EQ(rd64(b, 16), 1);                   /* nlink */
    LT_EQ(rd32(b, 28), 1000);                /* guest uid */
    LT_EQ(rd32(b, 32), 1000);                /* guest gid */
    LT_EQ(rd32(b, 36), 0);                   /* __pad0 */
    LT_EQ(rd64(b, 48), 11);                  /* size */
    LT_CHECK(rd64(b, 56) >= 512);            /* blksize */
    LT_EQ(rd64(b, 88), 1234567890);          /* mtime */
    LT_EQ(rd64(b, 96), 123456789);           /* mtime nsec */
    LT_EQ(rd64(b, 72), 1000000000);          /* atime */
    LT_EQ(rd64(b, 80), 5);
    LT_EQ(rd64(b, 120) | rd64(b, 128) | rd64(b, 136), 0);   /* __unused */
    LT_EQ(b[144], 0xAA);                     /* exactly 144 bytes written */
    LT_EQ(b[159], 0xAA);
    struct stat hs;
    char hp[PATH_MAX];
    snprintf(hp, sizeof hp, "%s/data", ROOT);
    stat(hp, &hs);
    LT_EQ(rd64(b, 8), hs.st_ino);
    /* stat by path gives the same bytes for the fields that do not move */
    uint8_t c[144];
    LT_EQ(SC(T, stat, P("/data"), P(c)), 0);
    LT_MEMEQ(b, c, 72);
    SC(T, close, fd);
}

/* statx (struct statx, 256 bytes): stx_mask 0, stx_blksize 4, stx_nlink 16, stx_uid 20, stx_gid 24,
 * stx_mode 28, stx_ino 32, stx_size 40, stx_blocks 48, stx_atime 64, stx_btime 80, stx_ctime 96,
 * stx_mtime 112, stx_dev_major 136, stx_mnt_id 144. */
LT_TEST(statx_layout)
{
    uint8_t b[272];
    memset(b, 0xAA, sizeof b);
    LT_EQ(SC(T, statx, AT_CWD, P("/data"), 0, LX_STATX_BASIC_STATS | LX_STATX_MNT_ID | LX_STATX_BTIME, P(b)), 0);
    uint32_t mask = rd32(b, 0);
    LT_CHECK((mask & LX_STATX_BASIC_STATS) == LX_STATX_BASIC_STATS);
    LT_CHECK(mask & LX_STATX_MNT_ID);
    LT_CHECK(mask & LX_STATX_BTIME);
    LT_EQ(rd16(b, 28), 0100640);
    LT_EQ(rd64(b, 40), 11);
    LT_EQ(rd64(b, 112), 1234567890);
    LT_EQ(rd32(b, 120), 123456789);
    LT_EQ(rd32(b, 20), 1000);
    LT_CHECK(rd64(b, 80) > 0);               /* birth time */
    LT_CHECK(rd64(b, 144) > 0);              /* mount id */
    LT_EQ(b[256], 0xAA);
    LT_ERRNO(SC(T, statx, AT_CWD, P("/data"), 0x6000, 0, P(b)), LX_EINVAL);   /* both sync types */
    LT_ERRNO(SC(T, statx, AT_CWD, P("/data"), 0, 0x80000000u, P(b)), LX_EINVAL);
    LT_ERRNO(SC(T, statx, AT_CWD, P("/nope"), 0, LX_STATX_BASIC_STATS, P(b)), LX_ENOENT);
    int64_t fd = gopen("/data", LX_O_RDONLY, 0);
    LT_EQ(SC(T, statx, fd, P(""), LX_AT_EMPTY_PATH, LX_STATX_BASIC_STATS, P(b)), 0);
    LT_EQ(rd64(b, 40), 11);
    LT_EQ(SC(T, statx, fd, 0, LX_AT_EMPTY_PATH, LX_STATX_BASIC_STATS, P(b)), 0);  /* NULL path (6.11+) */
    SC(T, close, fd);
}

LT_TEST(read_write_family)
{
    int64_t fd = gopen("/rw", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0644);
    char buf[64];
    LT_EQ(SC(T, write, fd, P("abcdefghij"), 10), 10);
    LT_EQ(SC(T, pread64, fd, P(buf), 4, 2), 4);
    LT_MEMEQ(buf, "cdef", 4);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_CUR), 10);        /* pread does not move the offset */
    LT_ERRNO(SC(T, pread64, fd, P(buf), 4, (uint64_t)-1), LX_EINVAL);
    LT_EQ(SC(T, pwrite64, fd, P("XY"), 2, 0), 2);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_SET), 0);
    LT_EQ(SC(T, read, fd, P(buf), sizeof buf), 10);
    LT_MEMEQ(buf, "XYcdefghij", 10);
    LT_EQ(SC(T, read, fd, P(buf), sizeof buf), 0);      /* EOF */
    struct lx_iovec iov[3] = { { P("12"), 2 }, { P(""), 0 }, { P("345"), 3 } };
    LT_EQ(SC(T, writev, fd, P(iov), 3), 5);
    LT_EQ(SC(T, lseek, fd, -5, LX_SEEK_END), 10);
    char a[2], c[3];
    struct lx_iovec riov[2] = { { P(a), 2 }, { P(c), 3 } };
    LT_EQ(SC(T, readv, fd, P(riov), 2), 5);
    LT_MEMEQ(a, "12", 2);
    LT_MEMEQ(c, "345", 3);
    LT_EQ(SC(T, preadv, fd, P(riov), 2, 0), 5);
    LT_MEMEQ(a, "XY", 2);
    LT_ERRNO(SC(T, readv, fd, P(riov), 1025), LX_EINVAL);              /* UIO_MAXIOV */
    LT_EQ(SC(T, readv, fd, P(riov), 0), 0);
    struct lx_iovec bad[2] = { { P(a), 0x7fffffffffffffffull }, { P(c), 2 } };
    LT_ERRNO(SC(T, readv, fd, P(bad), 2), LX_EINVAL);                  /* length sum overflows ssize_t */
    LT_ERRNO(SC(T, preadv2, fd, P(riov), 2, 0, 0, 0x80), LX_EOPNOTSUPP); /* unknown RWF flag */
    LT_EQ(SC(T, lseek, fd, 3, LX_SEEK_SET), 3);
    LT_EQ(SC(T, preadv2, fd, P(riov), 1, (uint64_t)-1, 0, 0), 2);       /* -1: current position */
    LT_MEMEQ(a, "de", 2);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_CUR), 5);
    LT_ERRNO(SC(T, lseek, fd, -100, LX_SEEK_SET), LX_EINVAL);
    LT_ERRNO(SC(T, lseek, fd, 0, 7), LX_EINVAL);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_DATA), 0);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_HOLE), 15);
    LT_ERRNO(SC(T, lseek, fd, 100, LX_SEEK_DATA), LX_ENXIO);
    int64_t ro = gopen("/rw", LX_O_RDONLY, 0);
    LT_ERRNO(SC(T, write, ro, P("x"), 1), LX_EBADF);
    LT_ERRNO(SC(T, ftruncate, ro, 0), LX_EINVAL);
    int64_t wo = gopen("/rw", LX_O_WRONLY, 0);
    LT_ERRNO(SC(T, read, wo, P(buf), 1), LX_EBADF);
    LT_EQ(SC(T, ftruncate, wo, 4), 0);
    LT_ERRNO(SC(T, ftruncate, wo, (uint64_t)-1), LX_EINVAL);
    LT_EQ(SC(T, lseek, ro, 0, LX_SEEK_END), 4);
    /* O_APPEND: pwrite appends whatever the offset (pwrite(2) BUGS on Linux) */
    int64_t ap = gopen("/rw", LX_O_WRONLY | LX_O_APPEND, 0);
    LT_EQ(SC(T, pwrite64, ap, P("Z"), 1, 0), 1);
    LT_EQ(SC(T, pread64, ro, P(buf), 8, 0), 5);
    LT_MEMEQ(buf, "XYcdZ", 5);
    /* FIONREAD on a regular file: size - position */
    int32_t avail = -1;
    LT_EQ(SC(T, lseek, ro, 1, LX_SEEK_SET), 1);
    LT_EQ(SC(T, ioctl, ro, LX_FIONREAD, P(&avail)), 0);
    LT_EQ(avail, 4);
    LT_ERRNO(SC(T, ioctl, ro, LX_TCGETS, P(buf)), LX_ENOTTY);
    /* fallocate grows, KEEP_SIZE does not, PUNCH_HOLE zeroes */
    LT_EQ(SC(T, fallocate, wo, 0, 0, 100), 0);
    LT_EQ(SC(T, lseek, ro, 0, LX_SEEK_END), 100);
    LT_EQ(SC(T, fallocate, wo, 1, 0, 200), 0);
    LT_EQ(SC(T, lseek, ro, 0, LX_SEEK_END), 100);
    LT_EQ(SC(T, fallocate, wo, 3, 0, 2), 0);
    LT_EQ(SC(T, pread64, ro, P(buf), 3, 0), 3);
    LT_MEMEQ(buf, "\0\0c", 3);
    LT_ERRNO(SC(T, fallocate, wo, 2, 0, 2), LX_EOPNOTSUPP);   /* PUNCH_HOLE needs KEEP_SIZE */
    LT_ERRNO(SC(T, fallocate, wo, 0x10, 0, 2), LX_EOPNOTSUPP); /* ZERO_RANGE unsupported */
    LT_ERRNO(SC(T, fallocate, ro, 0, 0, 2), LX_EBADF);
    LT_ERRNO(SC(T, fallocate, wo, 0, 0, 0), LX_EINVAL);
    LT_EQ(SC(T, fsync, wo), 0);
    LT_EQ(SC(T, fdatasync, wo), 0);
    LT_EQ(SC(T, fadvise64, ro, 0, 0, 3), 0);
    LT_ERRNO(SC(T, fadvise64, ro, 0, 0, 9), LX_EINVAL);
    SC(T, close, fd); SC(T, close, ro); SC(T, close, wo); SC(T, close, ap);
}

LT_TEST(sendfile_copy_range)
{
    int64_t src = gopen("/src", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0644);
    char big[100000];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7);
    LT_EQ(SC(T, write, src, P(big), sizeof big), (int64_t)sizeof big);
    int64_t dst = gopen("/dst", LX_O_RDWR | LX_O_CREAT | LX_O_TRUNC, 0644);
    int64_t off = 10;
    LT_EQ(SC(T, sendfile, dst, src, P(&off), 70000), 70000);
    LT_EQ(off, 70010);
    LT_EQ(SC(T, lseek, src, 0, LX_SEEK_CUR), 100000);   /* offset given: src position untouched */
    char back[70000];
    LT_EQ(SC(T, pread64, dst, P(back), sizeof back, 0), 70000);
    LT_MEMEQ(back, big + 10, 70000);
    int64_t oin = 0, oout = 0;
    LT_EQ(SC(T, copy_file_range, src, P(&oin), dst, P(&oout), 100, 0), 100);
    LT_EQ(oin, 100);
    LT_ERRNO(SC(T, copy_file_range, src, P(&oin), dst, P(&oout), 100, 1), LX_EINVAL);
    oin = 0; oout = 10;
    LT_ERRNO(SC(T, copy_file_range, src, P(&oin), src, P(&oout), 50, 0), LX_EINVAL);   /* overlap */
    int64_t ap = gopen("/dst", LX_O_WRONLY | LX_O_APPEND, 0);
    LT_ERRNO(SC(T, sendfile, ap, src, 0, 10), LX_EINVAL);
    LT_ERRNO(SC(T, copy_file_range, src, 0, ap, 0, 10, 0), LX_EBADF);
    SC(T, close, src); SC(T, close, dst); SC(T, close, ap);
}

/* getdents64 record: d_ino 0, d_off 8, d_reclen 16, d_type 18, d_name 19, padded to 8;
 * legacy linux_dirent: d_ino 0, d_off 8, d_reclen 16, d_name 18, d_type in the last byte. */
static int count_entries(const char *d, int64_t n, int legacy, char names[][32], uint8_t *types, int max)
{
    int k = 0;
    for (int64_t p = 0; p < n && k < max; k++) {
        uint16_t rl = rd16((const uint8_t *)d, (int)p + 16);
        const char *nm = d + p + (legacy ? 18 : 19);
        snprintf(names[k], 32, "%s", nm);
        types[k] = (uint8_t)(legacy ? d[p + rl - 1] : d[p + 18]);
        p += rl;
    }
    return k;
}

/* A directory of game data (Rocket League's CookedPCConsole holds 20,746 files) lists every name once, and fast:
 * Wine lists the whole directory for each case-insensitive lookup that misses. */
LT_TEST(getdents_large_directory)
{
    enum { N = 20000 };
    char hp[PATH_MAX];
    snprintf(hp, sizeof hp, "%s/big", ROOT);
    LT_EQ(mkdir(hp, 0755), 0);
    for (int i = 0; i < N; i++) {
        snprintf(hp, sizeof hp, "%s/big/Package_%05d_SF.upk", ROOT, i);
        close(open(hp, O_CREAT | O_WRONLY, 0644));
    }
    static uint8_t seen[N];
    memset(seen, 0, sizeof seen);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t fd = gopen("/big", LX_O_RDONLY | LX_O_DIRECTORY, 0);
    LT_CHECK(fd >= 0);
    static char d[32768];
    int total = 0, dots = 0;
    for (int64_t n; (n = SC(T, getdents64, fd, P(d), sizeof d)) > 0;) {
        for (int64_t p = 0; p < n; p += rd16((uint8_t *)d, (int)p + 16)) {
            const char *nm = d + p + 19;
            int i;
            if (!strcmp(nm, ".") || !strcmp(nm, "..")) dots++;
            else if (sscanf(nm, "Package_%05d_SF.upk", &i) == 1 && i >= 0 && i < N) seen[i]++;
            total++;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    SC(T, close, fd);
    LT_EQ(total, N + 2);
    LT_EQ(dots, 2);
    int once = 0;
    for (int i = 0; i < N; i++) once += seen[i] == 1;
    LT_EQ(once, N);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("INFO listing %d names took %.1f ms\n", N + 2, ms);
    LT_CHECK(ms < 150);
}

/* FS_IOC_GETFLAGS reports FS_CASEFOLD_FL on a directory of a case-insensitive host volume (default APFS). Wine checks
 * it before listing a whole directory for each name it does not find as written (get_dir_case_sensitivity). */
LT_TEST(ioctl_getflags_casefold)
{
    enum { GETFLAGS = 0x80086601, CASEFOLD = 0x40000000 };
    const int insensitive = pathconf(ROOT, _PC_CASE_SENSITIVE) == 0;
    LT_EQ(SC(T, mkdir, P("/cf"), 0755), 0);
    LT_EQ(SC(T, close, gopen("/cf/f", LX_O_CREAT | LX_O_WRONLY, 0644)), 0);
    int64_t d = gopen("/cf", LX_O_RDONLY | LX_O_NONBLOCK, 0);
    LT_CHECK(d >= 0);
    int32_t flags[2] = {-1, 0x5a5a5a5a};
    LT_EQ(SC(T, ioctl, d, GETFLAGS, P(flags)), 0);
    LT_EQ(flags[0], insensitive ? CASEFOLD : 0);
    LT_EQ(flags[1], 0x5a5a5a5a); /* the kernel writes an int, not a long */
    LT_EQ(SC(T, ioctl, d, GETFLAGS, 8), -LX_EFAULT);
    SC(T, close, d);
    int64_t f = gopen("/cf/f", LX_O_RDONLY, 0);
    flags[0] = -1;
    LT_EQ(SC(T, ioctl, f, GETFLAGS, P(flags)), 0);
    LT_EQ(flags[0], 0); /* casefold is a directory flag */
    SC(T, close, f);
    int64_t pth = gopen("/cf", LX_O_PATH, 0);
    LT_EQ(SC(T, ioctl, pth, GETFLAGS, P(flags)), -LX_EBADF);
    SC(T, close, pth);
    printf("INFO host volume case-%s\n", insensitive ? "insensitive" : "sensitive");
}

LT_TEST(getdents_formats)
{
    LT_EQ(SC(T, mkdir, P("/dd"), 0755), 0);
    LT_EQ(SC(T, close, gopen("/dd/a", LX_O_CREAT | LX_O_WRONLY, 0644)), 0);
    LT_EQ(SC(T, close, gopen("/dd/bbbbbbbb", LX_O_CREAT | LX_O_WRONLY, 0644)), 0);
    LT_EQ(SC(T, mkdir, P("/dd/sub"), 0755), 0);
    LT_EQ(SC(T, symlink, P("a"), P("/dd/ln")), 0);
    int64_t fd = gopen("/dd", LX_O_RDONLY | LX_O_DIRECTORY, 0);
    char d[4096];
    int64_t n = SC(T, getdents64, fd, P(d), sizeof d);
    LT_CHECK(n > 0);
    char names[16][32];
    uint8_t ty[16];
    int k = count_entries(d, n, 0, names, ty, 16);
    LT_EQ(k, 6);
    int seen = 0;
    for (int i = 0; i < k; i++) {
        if (!strcmp(names[i], "a")) { seen |= 1; LT_EQ(ty[i], LX_DT_REG); }
        if (!strcmp(names[i], "bbbbbbbb")) seen |= 2;
        if (!strcmp(names[i], "sub")) { seen |= 4; LT_EQ(ty[i], LX_DT_DIR); }
        if (!strcmp(names[i], "ln")) { seen |= 8; LT_EQ(ty[i], LX_DT_LNK); }
        if (!strcmp(names[i], ".")) seen |= 16;
        if (!strcmp(names[i], "..")) seen |= 32;
    }
    LT_EQ(seen, 63);
    /* record of the first entry: reclen = align8(19 + len + 1), d_off = 1 */
    size_t l0 = strlen(d + 19);
    LT_EQ(rd16((uint8_t *)d, 16), (19 + l0 + 1 + 7) & ~7u);
    LT_EQ(rd64((uint8_t *)d, 8), 1);
    LT_EQ(SC(T, getdents64, fd, P(d), sizeof d), 0);    /* end */
    /* legacy format */
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_SET), 0);
    n = SC(T, getdents, fd, P(d), sizeof d);
    k = count_entries(d, n, 1, names, ty, 16);
    LT_EQ(k, 6);
    for (int i = 0; i < k; i++) if (!strcmp(names[i], "sub")) LT_EQ(ty[i], LX_DT_DIR);
    l0 = strlen(d + 18);
    LT_EQ(rd16((uint8_t *)d, 16), (18 + l0 + 2 + 7) & ~7u);
    /* a buffer too small for one entry: -EINVAL; a small buffer returns one entry, lseek(d_off)
     * resumes after it */
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_SET), 0);
    LT_ERRNO(SC(T, getdents64, fd, P(d), 10), LX_EINVAL);
    n = SC(T, getdents64, fd, P(d), 24);
    LT_EQ(n, 24);
    int64_t next = (int64_t)rd64((uint8_t *)d, 8);
    char first[32];
    snprintf(first, sizeof first, "%s", d + 19);
    LT_EQ(SC(T, lseek, fd, next, LX_SEEK_SET), next);
    n = SC(T, getdents64, fd, P(d), sizeof d);
    k = count_entries(d, n, 0, names, ty, 16);
    LT_EQ(k, 5);
    for (int i = 0; i < k; i++) LT_CHECK(strcmp(names[i], first) != 0);
    /* rewind sees a new entry */
    LT_EQ(SC(T, close, gopen("/dd/new", LX_O_CREAT | LX_O_WRONLY, 0644)), 0);
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_SET), 0);
    n = SC(T, getdents64, fd, P(d), sizeof d);
    LT_EQ(count_entries(d, n, 0, names, ty, 16), 7);
    char rb[4];
    LT_ERRNO(SC(T, read, fd, P(rb), 1), LX_EISDIR);
    int64_t f = gopen("/dd/a", LX_O_RDONLY, 0);
    LT_ERRNO(SC(T, getdents64, f, P(d), sizeof d), LX_ENOTDIR);
    SC(T, close, f);
    SC(T, close, fd);
}

/* Namespace calls: mkdir/rmdir/unlink/rename/link/symlink/readlink and their errors. */
LT_TEST(namespace_ops)
{
    LT_EQ(SC(T, mkdir, P("/n"), 0777), 0);
    struct lx_stat st;
    LT_EQ(SC(T, stat, P("/n"), P(&st)), 0);
    LT_EQ(st.st_mode, 040755);                          /* 0777 & ~umask 022 */
    LT_ERRNO(SC(T, mkdir, P("/n"), 0755), LX_EEXIST);
    LT_ERRNO(SC(T, mkdir, P("/n/x/y"), 0755), LX_ENOENT);
    LT_EQ(SC(T, mkdirat, AT_CWD, P("/n/d/"), 0700), 0);  /* trailing slash allowed for mkdir */
    LT_ERRNO(SC(T, rmdir, P("/n")), LX_ENOTEMPTY);
    LT_ERRNO(SC(T, rmdir, P("/n/d/.")), LX_EINVAL);
    LT_ERRNO(SC(T, rmdir, P("/n/..")), LX_ENOTEMPTY);
    LT_EQ(SC(T, close, gopen("/n/f", LX_O_CREAT | LX_O_WRONLY, 0600)), 0);
    LT_ERRNO(SC(T, rmdir, P("/n/f")), LX_ENOTDIR);
    LT_ERRNO(SC(T, unlink, P("/n/d")), LX_EISDIR);
    LT_ERRNO(SC(T, unlinkat, AT_CWD, P("/n/f"), 0x1), LX_EINVAL);
    LT_EQ(SC(T, link, P("/n/f"), P("/n/g")), 0);
    LT_EQ(SC(T, stat, P("/n/f"), P(&st)), 0);
    LT_EQ(st.st_nlink, 2);
    LT_ERRNO(SC(T, link, P("/n/f"), P("/n/g")), LX_EEXIST);
    LT_ERRNO(SC(T, link, P("/n/d"), P("/n/dl")), LX_EPERM);
    LT_EQ(SC(T, symlink, P("f"), P("/n/s")), 0);
    char buf[16];
    LT_EQ(SC(T, readlink, P("/n/s"), P(buf), sizeof buf), 1);
    LT_EQ(buf[0], 'f');
    LT_EQ(SC(T, readlink, P("/n/s"), P(buf), 0) , -LX_EINVAL);
    LT_ERRNO(SC(T, readlink, P("/n/f"), P(buf), sizeof buf), LX_EINVAL);
    LT_EQ(SC(T, rename, P("/n/g"), P("/n/h")), 0);
    LT_ERRNO(SC(T, stat, P("/n/g"), P(&st)), LX_ENOENT);
    LT_ERRNO(SC(T, renameat2, AT_CWD, P("/n/h"), AT_CWD, P("/n/f"), LX_AT_RENAME_NOREPLACE), LX_EEXIST);
    LT_ERRNO(SC(T, renameat2, AT_CWD, P("/n/h"), AT_CWD, P("/n/f"), 3), LX_EINVAL);
    LT_ERRNO(SC(T, renameat2, AT_CWD, P("/n/h"), AT_CWD, P("/n/zz"), LX_AT_RENAME_EXCHANGE), LX_ENOENT);
    LT_EQ(SC(T, renameat2, AT_CWD, P("/n/h"), AT_CWD, P("/n/s"), LX_AT_RENAME_EXCHANGE), 0);
    LT_EQ(SC(T, lstat, P("/n/h"), P(&st)), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);           /* swapped */
    LT_ERRNO(SC(T, rename, P("/n/d"), P("/n/f")), LX_ENOTDIR);
    LT_ERRNO(SC(T, rename, P("/n/f"), P("/n/d")), LX_EISDIR);
    LT_ERRNO(SC(T, rename, P("/n"), P("/n/d/inside")), LX_EINVAL);
    LT_ERRNO(SC(T, rename, P("/n/."), P("/m")), LX_EBUSY);
    LT_EQ(SC(T, unlink, P("/n/h")), 0);
    LT_EQ(SC(T, mknod, P("/n/fifo"), LX_S_IFIFO | 0600, 0), 0);
    LT_EQ(SC(T, lstat, P("/n/fifo"), P(&st)), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFIFO);
    LT_ERRNO(SC(T, mknod, P("/n/chr"), LX_S_IFCHR | 0600, 0x0103), LX_EPERM);
    LT_EQ(SC(T, mknodat, AT_CWD, P("/n/reg"), 0600, 0), 0);
    LT_ERRNO(SC(T, mknod, P("/n/reg"), 0600, 0), LX_EEXIST);
}

/* open(2) error rules of Linux 6.12. */
LT_TEST(open_rules)
{
    LT_ERRNO(gopen("/n", LX_O_RDWR, 0), LX_EISDIR);
    LT_ERRNO(gopen("/n", LX_O_CREAT | LX_O_RDONLY, 0644), LX_EISDIR);
    LT_ERRNO(gopen("/newdir", LX_O_CREAT | LX_O_DIRECTORY, 0755), LX_EINVAL);
    LT_ERRNO(gopen("/n/f", LX_O_RDONLY | LX_O_DIRECTORY, 0), LX_ENOTDIR);
    LT_ERRNO(gopen("/n", LX_O_TMPFILE | LX_O_RDWR, 0600), LX_EOPNOTSUPP);
    LT_ERRNO(gopen("/n", LX_O_TMPFILE | LX_O_RDONLY, 0600), LX_EINVAL);
    LT_ERRNO(gopen("/missing/x", LX_O_CREAT | LX_O_WRONLY, 0600), LX_ENOENT);
    LT_ERRNO(gopen("/n/newfile/", LX_O_CREAT | LX_O_WRONLY, 0600), LX_EISDIR);
    /* the descriptor returned by a creating open is writable even if the mode forbids writing */
    int64_t fd = gopen("/n/ro_file", LX_O_CREAT | LX_O_RDWR, 0444);
    LT_CHECK(fd >= 0);
    LT_EQ(SC(T, write, fd, P("ok"), 2), 2);
    SC(T, close, fd);
    LT_ERRNO(gopen("/n/ro_file", LX_O_RDWR, 0), LX_EACCES);
    LT_EQ(SC(T, access, P("/n/ro_file"), LX_R_OK), 0);
    LT_ERRNO(SC(T, access, P("/n/ro_file"), LX_W_OK), LX_EACCES);
    LT_ERRNO(SC(T, access, P("/n/ro_file"), 8), LX_EINVAL);
    LT_ERRNO(SC(T, faccessat2, AT_CWD, P("/n/ro_file"), LX_R_OK, 0x8000), LX_EINVAL);
    LT_EQ(SC(T, faccessat2, AT_CWD, P("/n/ro_file"), LX_R_OK, LX_AT_EACCESS), 0);
    fd = gopen("/n/trunc", LX_O_CREAT | LX_O_WRONLY, 0644);
    SC(T, write, fd, P("12345"), 5);
    SC(T, close, fd);
    fd = gopen("/n/trunc", LX_O_WRONLY | LX_O_TRUNC, 0);
    struct lx_stat st;
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_size, 0);
    SC(T, close, fd);
    fd = SC(T, creat, P("/n/c"), 0600);
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFL) & LX_O_ACCMODE, LX_O_WRONLY);
    SC(T, close, fd);
    LT_EQ(SC(T, truncate, P("/n/c"), 77), 0);
    LT_EQ(SC(T, stat, P("/n/c"), P(&st)), 0);
    LT_EQ(st.st_size, 77);
    LT_ERRNO(SC(T, truncate, P("/n"), 0), LX_EISDIR);
}

/* chmod/chown/umask/utimes. chown to another user needs CAP_CHOWN (-EPERM). */
LT_TEST(attributes)
{
    struct lx_stat st;
    LT_EQ(SC(T, chmod, P("/n/c"), 0751), 0);
    LT_EQ(SC(T, stat, P("/n/c"), P(&st)), 0);
    LT_EQ(st.st_mode & 07777, 0751);
    int64_t fd = gopen("/n/c", LX_O_RDONLY, 0);
    LT_EQ(SC(T, fchmod, fd, 0600), 0);
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode & 07777, 0600);
    LT_EQ(SC(T, chown, P("/n/c"), 1000, 1000), 0);
    LT_EQ(SC(T, fchown, fd, (uint64_t)-1, (uint64_t)-1), 0);
    LT_ERRNO(SC(T, chown, P("/n/c"), 0, 0), LX_EPERM);
    LT_ERRNO(SC(T, fchownat, AT_CWD, P("/n/c"), 1000, 1000, 0x8), LX_EINVAL);
    LT_EQ(SC(T, symlink, P("c"), P("/n/sl")), 0);
    LT_ERRNO(SC(T, fchmodat2, AT_CWD, P("/n/sl"), 0600, LX_AT_SYMLINK_NOFOLLOW), LX_EOPNOTSUPP);
    LT_EQ(SC(T, umask, 077), 022);
    LT_EQ(SC(T, umask, 022), 077);
    struct lx_timeval tv[2] = { { 100, 5 }, { 200, 999999 } };
    LT_EQ(SC(T, utimes, P("/n/c"), P(tv)), 0);
    LT_EQ(SC(T, stat, P("/n/c"), P(&st)), 0);
    LT_EQ(st.st_mtime_sec, 200);
    LT_EQ(st.st_mtime_nsec, 999999000);
    tv[1].tv_usec = 1000000;
    LT_ERRNO(SC(T, utimes, P("/n/c"), P(tv)), LX_EINVAL);
    struct lx_timespec ts[2] = { { 0, LX_UTIME_OMIT }, { 300, 0 } };
    LT_EQ(SC(T, utimensat, fd, 0, P(ts), 0), 0);         /* NULL path: the fd itself */
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mtime_sec, 300);
    LT_EQ(st.st_atime_sec, 100);                           /* omitted */
    ts[0].tv_nsec = 1000000000;
    LT_ERRNO(SC(T, utimensat, AT_CWD, P("/n/c"), P(ts), 0), LX_EINVAL);
    int64_t ub[2] = { 400, 500 };
    LT_EQ(SC(T, utime, P("/n/c"), P(ub)), 0);
    LT_EQ(SC(T, stat, P("/n/c"), P(&st)), 0);
    LT_EQ(st.st_atime_sec, 400);
    SC(T, close, fd);
}

/* xattrs: user namespace stored; trusted/security/system refused as for an unprivileged Linux user. */
LT_TEST(xattrs)
{
    char v[32];
    LT_EQ(SC(T, setxattr, P("/n/c"), P("user.k"), P("val"), 3, 0), 0);
    LT_EQ(SC(T, getxattr, P("/n/c"), P("user.k"), P(v), sizeof v), 3);
    LT_MEMEQ(v, "val", 3);
    LT_EQ(SC(T, getxattr, P("/n/c"), P("user.k"), 0, 0), 3);
    LT_ERRNO(SC(T, getxattr, P("/n/c"), P("user.k"), P(v), 2), LX_ERANGE);
    LT_ERRNO(SC(T, setxattr, P("/n/c"), P("user.k"), P("x"), 1, 1), LX_EEXIST);   /* XATTR_CREATE */
    LT_ERRNO(SC(T, setxattr, P("/n/c"), P("user.q"), P("x"), 1, 2), LX_ENODATA);  /* XATTR_REPLACE */
    LT_ERRNO(SC(T, getxattr, P("/n/c"), P("user.none"), P(v), sizeof v), LX_ENODATA);
    LT_ERRNO(SC(T, setxattr, P("/n/c"), P("trusted.k"), P("x"), 1, 0), LX_EPERM);
    LT_ERRNO(SC(T, getxattr, P("/n/c"), P("security.selinux"), P(v), sizeof v), LX_ENODATA);
    LT_ERRNO(SC(T, getxattr, P("/n/c"), P("system.posix_acl_access"), P(v), sizeof v), LX_EOPNOTSUPP);
    LT_ERRNO(SC(T, getxattr, P("/n/c"), P("bogus"), P(v), sizeof v), LX_EOPNOTSUPP);
    LT_ERRNO(SC(T, setxattr, P("/n/c"), P(""), P("x"), 1, 0), LX_ERANGE);
    LT_ERRNO(SC(T, lsetxattr, P("/n/sl"), P("user.k"), P("x"), 1, 0), LX_EPERM);   /* user.* on a symlink */
    char list[64];
    int64_t n = SC(T, listxattr, P("/n/c"), P(list), sizeof list);
    LT_EQ(n, 7);
    LT_STREQ(list, "user.k");
    LT_EQ(SC(T, listxattr, P("/n/c"), 0, 0), 7);
    int64_t fd = gopen("/n/c", LX_O_RDONLY, 0);
    LT_EQ(SC(T, fgetxattr, fd, P("user.k"), P(v), sizeof v), 3);
    LT_EQ(SC(T, fremovexattr, fd, P("user.k")), 0);
    LT_EQ(SC(T, flistxattr, fd, P(list), sizeof list), 0);
    SC(T, close, fd);
    /* getxattr looks in the attribute list first: a list longer than LKD's 4 KiB buffer still finds its last entry */
    char name[160];
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "user.%0120d", i);
        LT_EQ(SC(T, setxattr, P("/n/c"), P(name), P("v"), 1, 0), 0);
    }
    LT_EQ(SC(T, getxattr, P("/n/c"), P(name), P(v), sizeof v), 1);
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "user.%0120d", i);
        SC(T, removexattr, P("/n/c"), P(name));
    }
    /* a directory descriptor (open for search on the host) reads and misses attributes too */
    LT_EQ(SC(T, setxattr, P("/n"), P("user.d"), P("dir"), 3, 0), 0);
    int64_t dfd = gopen("/n", LX_O_RDONLY | LX_O_DIRECTORY, 0);
    LT_EQ(SC(T, fgetxattr, dfd, P("user.d"), P(v), sizeof v), 3);
    LT_ERRNO(SC(T, fgetxattr, dfd, P("user.none"), P(v), sizeof v), LX_ENODATA);
    LT_EQ(SC(T, fremovexattr, dfd, P("user.d")), 0);
    SC(T, close, dfd);
}

LT_TEST(statfs_and_sync)
{
    struct lx_statfs s;
    LT_EQ(SC(T, statfs, P("/"), P(&s)), 0);
    LT_CHECK(s.f_bsize > 0);
    LT_EQ(s.f_namelen, 255);
    int64_t fd = gopen("/n/c", LX_O_RDONLY, 0);
    LT_EQ(SC(T, fstatfs, fd, P(&s)), 0);
    LT_EQ(SC(T, syncfs, fd), 0);
    LT_EQ(SC(T, sync_file_range, fd, 0, 0, 2), 0);
    LT_ERRNO(SC(T, sync_file_range, fd, 0, 0, 8), LX_EINVAL);
    LT_EQ(SC0(T, sync), 0);
    LT_ERRNO(SC(T, ustat, 0xffff, P(&s)), LX_EINVAL);
    LT_ERRNO(SC(T, name_to_handle_at, AT_CWD, P("/n/c"), P(&s), P(&s), 0), LX_EOPNOTSUPP);
    LT_ERRNO(SC(T, open_by_handle_at, fd, P(&s), 0), LX_EPERM);
    SC(T, close, fd);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "io");
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(stat_layout_oracle);
    LT_RUN(statx_layout);
    LT_RUN(read_write_family);
    LT_RUN(sendfile_copy_range);
    LT_RUN(getdents_formats);
    LT_RUN(getdents_large_directory);
    LT_RUN(ioctl_getflags_casefold);
    LT_RUN(namespace_ops);
    LT_RUN(open_rules);
    LT_RUN(attributes);
    LT_RUN(xattrs);
    LT_RUN(statfs_and_sync);
    rm_rf(ROOT);
    return lt_finish();
}
