/* test_fs_glibc.c: the file system calls of glibc 2.41's ld.so and early libc start-up, against the
 * Debian sysroot that will be the guest root (mounted READ-ONLY here; the tree is never modified).
 * Sequence modeled on elf/dl-load.c and elf/dl-cache.c: access(/etc/ld.so.preload), openat of
 * /etc/ld.so.cache (absent in this tree), openat of glibc-hwcaps candidates then
 * /lib/x86_64-linux-gnu/libc.so.6 with O_RDONLY|O_CLOEXEC, read of the first 832 bytes, fstat via
 * newfstatat(fd, "", AT_EMPTY_PATH), pread64 of program headers, close; readlink and getcwd. Skipped
 * (with a passing marker assertion) when the sysroot is absent. */
#include "test_fs_util.h"

#ifndef SYSROOT /* -DSYSROOT=... : the x86-64 Debian root of the guest (build-rootfs.sh in Voile) */
#define SYSROOT "/Users/mathieu/Documents/Github/Zagrios/sailover/build/experiments/rocket-hyperlinux-20260928/linux-root/rootfs"
#endif

static lkd_task *T;

LT_TEST(ldso_sequence)
{
    LT_ERRNO(SC(T, access, P("/etc/ld.so.preload"), LX_R_OK), LX_ENOENT);
    LT_ERRNO(SC(T, openat, AT_CWD, P("/etc/ld.so.cache"), LX_O_RDONLY | LX_O_CLOEXEC), LX_ENOENT);
    LT_ERRNO(SC(T, openat, AT_CWD, P("/lib/x86_64-linux-gnu/glibc-hwcaps/x86-64-v3/libc.so.6"),
                LX_O_RDONLY | LX_O_CLOEXEC), LX_ENOENT);
    int64_t fd = SC(T, openat, AT_CWD, P("/lib/x86_64-linux-gnu/libc.so.6"), LX_O_RDONLY | LX_O_CLOEXEC);
    LT_CHECK(fd >= 0);
    uint8_t eh[832];
    LT_EQ(SC(T, read, fd, P(eh), sizeof eh), 832);
    LT_MEMEQ(eh, "\177ELF", 4);
    LT_EQ(eh[4], 2);                           /* ELFCLASS64 */
    uint16_t mach;
    memcpy(&mach, eh + 18, 2);
    LT_EQ(mach, 62);                           /* EM_X86_64 */
    struct lx_stat st;
    LT_EQ(SC(T, newfstatat, fd, P(""), P(&st), LX_AT_EMPTY_PATH), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_EQ(st.st_size, 1995216);
    uint64_t phoff;
    uint16_t phnum, phentsize;
    memcpy(&phoff, eh + 32, 8);
    memcpy(&phentsize, eh + 54, 2);
    memcpy(&phnum, eh + 56, 2);
    LT_EQ(phentsize, 56);
    uint8_t ph[56 * 32];
    LT_CHECK(phnum > 0 && phnum <= 32);
    LT_EQ(SC(T, pread64, fd, P(ph), (uint64_t)phnum * 56, phoff), (int64_t)phnum * 56);
    uint32_t ptype;
    memcpy(&ptype, ph, 4);
    LT_EQ(ptype, 6);                           /* PT_PHDR first in libc.so.6 */
    LT_EQ(SC(T, fcntl, fd, LX_F_GETFD), LX_FD_CLOEXEC);
    LT_EQ(SC(T, close, fd), 0);
    LT_ERRNO(SC(T, close, fd), LX_EBADF);
}

LT_TEST(links_and_cwd)
{
    char buf[128];
    int64_t n = SC(T, readlink, P("/lib64/ld-linux-x86-64.so.2"), P(buf), sizeof buf);
    LT_EQ(n, 44);
    LT_MEMEQ(buf, "../lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", 44);
    struct lx_stat st;
    LT_EQ(SC(T, stat, P("/lib64/ld-linux-x86-64.so.2"), P(&st)), 0);   /* through two links */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_EQ(SC(T, access, P("/lib64/ld-linux-x86-64.so.2"), LX_X_OK), 0);
    char rp[LX_PATH_MAX];
    LT_CHECK(lkd_vfs_realpath(T, LX_AT_FDCWD, "/lib64/ld-linux-x86-64.so.2", rp, sizeof rp) > 0);
    LT_STREQ(rp, "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2");
    LT_EQ(SC(T, getcwd, P(buf), sizeof buf), 2);
    LT_STREQ(buf, "/");
    LT_EQ(SC(T, stat, P("/usr/bin/X11/X11/X11/basename"), P(&st)), 0);  /* X11 -> . */
    LT_EQ(SC(T, access, P("/bin/basename"), LX_X_OK), 0);
    LT_ERRNO(SC(T, access, P("/etc/hosts"), LX_X_OK), LX_EACCES);
    LT_ERRNO(SC(T, access, P("/etc/hosts"), LX_W_OK), LX_EROFS);       /* read-only guest root */
}

LT_TEST(readonly_root)
{
    LT_ERRNO(SC(T, openat, AT_CWD, P("/etc/ld.so.cache"), LX_O_RDWR | LX_O_CREAT, 0644), LX_EROFS);
    LT_ERRNO(SC(T, openat, AT_CWD, P("/etc/hosts"), LX_O_WRONLY), LX_EROFS);
    LT_ERRNO(SC(T, mkdir, P("/etc/x"), 0755), LX_EROFS);
    LT_ERRNO(SC(T, unlink, P("/etc/hosts")), LX_EROFS);
    LT_ERRNO(SC(T, rename, P("/etc/hosts"), P("/etc/hosts2")), LX_EROFS);
    LT_ERRNO(SC(T, chmod, P("/etc/hosts"), 0777), LX_EROFS);
    LT_ERRNO(SC(T, symlink, P("x"), P("/etc/ln")), LX_EROFS);
    LT_ERRNO(SC(T, truncate, P("/etc/hosts"), 0), LX_EROFS);
    struct lx_statfs sf;
    LT_EQ(SC(T, statfs, P("/etc"), P(&sf)), 0);
    LT_CHECK(sf.f_flags & 1);                  /* ST_RDONLY */
}

LT_TEST(directory_listing)
{
    int64_t fd = SC(T, openat, AT_CWD, P("/lib/x86_64-linux-gnu"), LX_O_RDONLY | LX_O_DIRECTORY | LX_O_CLOEXEC);
    LT_CHECK(fd >= 0);
    static char d[32768];
    int found = 0, total = 0;
    for (;;) {
        int64_t n = SC(T, getdents64, fd, P(d), sizeof d);
        if (n <= 0) { LT_EQ(n, 0); break; }
        for (int64_t p = 0; p < n;) {
            uint16_t rl;
            memcpy(&rl, d + p + 16, 2);
            if (!strcmp(d + p + 19, "libc.so.6")) found = d[p + 18] == LX_DT_REG;
            total++;
            p += rl;
        }
    }
    LT_CHECK(found);
    LT_EQ(total, 728 + 2);                     /* 728 names (MESURE, ls | wc -l) plus "." and ".." */
    SC(T, close, fd);
}

/* Engine helpers: an fd object for the ELF loader from a host fd or a guest path. */
LT_TEST(engine_helpers)
{
    int hfd = open(SYSROOT "/usr/lib/x86_64-linux-gnu/libc.so.6", O_RDONLY | O_CLOEXEC);
    LT_CHECK(hfd >= 0);
    lkd_fdobj *o = NULL;
    LT_EQ(lkd_fs_open_host_file("/usr/lib/x86_64-linux-gnu/libc.so.6", hfd, &o), 0);
    LT_CHECK(o && o->ops->mmap_fd && o->ops->mmap_fd(o) >= 0 && o->ops->mmap_fd(o) != hfd);
    char txt[128];
    int n = lkd_fs_describe(o, txt, sizeof txt);
    txt[n > 0 ? n : 0] = 0;
    LT_STREQ(txt, "/usr/lib/x86_64-linux-gnu/libc.so.6");
    struct lx_stat st;
    LT_EQ(o->ops->fstat(T, o, &st), 0);
    LT_EQ(st.st_size, 1995216);
    lkd_fdobj_put(o);
    LT_EQ(fcntl(hfd, F_GETFD), FD_CLOEXEC);    /* the caller's fd is untouched */
    close(hfd);
    LT_ERRNO(lkd_fs_open_host_file("/x", -1, &o), LX_EBADF);
    int dfd = open(SYSROOT "/etc", O_RDONLY);
    LT_ERRNO(lkd_fs_open_host_file("/etc", dfd, &o), LX_EACCES);
    close(dfd);
    LT_EQ(lkd_fs_open_guest_file(T, "/lib64/ld-linux-x86-64.so.2", &o), 0);
    n = lkd_fs_describe(o, txt, sizeof txt);
    txt[n > 0 ? n : 0] = 0;
    LT_STREQ(txt, "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2");
    lkd_fdobj_put(o);
    char hp[PATH_MAX];
    LT_EQ(lkd_vfs_to_host(T, LX_AT_FDCWD, "/lib/x86_64-linux-gnu/libc.so.6", 0, hp, sizeof hp), 0);
    LT_STREQ(hp, SYSROOT "/usr/lib/x86_64-linux-gnu/libc.so.6");
    LT_EQ(lkd_fs_host_to_guest(hp, txt, sizeof txt), 0);
    LT_STREQ(txt, "/usr/lib/x86_64-linux-gnu/libc.so.6");
}

LT_TEST(stdio_install)
{
    lkd_task *t = fs_task();
    LT_EQ(lkd_fs_install_stdio(t), 0);
    struct lx_stat st;
    LT_EQ(SC(t, fstat, 2, P(&st)), 0);
    LT_EQ(SC(t, write, 2, P(""), 0), 0);
    LT_EQ(lkd_fd_count(t), 3);
    lkd_set_current(T);
}

int main(void)
{
    struct stat hs;
    if (stat(SYSROOT "/usr/lib/x86_64-linux-gnu/libc.so.6", &hs) < 0) {
        printf("sysroot absent, skipping\n");
        LT_CHECK(1);
        return lt_finish();
    }
    char root[PATH_MAX];
    if (!realpath(SYSROOT, root)) return 2;
    lkd_config cfg = { .size = sizeof cfg, .guest_root = root };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    lkd_fs_set_root_ro(1);   /* the sysroot is read-only for this effort */
    T = fs_task();
    LT_RUN(ldso_sequence);
    LT_RUN(links_and_cwd);
    LT_RUN(readonly_root);
    LT_RUN(directory_listing);
    LT_RUN(engine_helpers);
    LT_RUN(stdio_install);
    return lt_finish();
}
