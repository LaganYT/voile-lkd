/* test_fs_vfs.c: rooted path resolution, symlinks, binds, virtual mounts, cwd/root (fs module).
 * Linux behaviors modeled: path_resolution(7), fs/namei.c (MAXSYMLINKS 40, ".." after a symlink is
 * the parent of the link's target, absolute targets restart at the process root, trailing slash
 * requires a directory), chroot(2), getcwd(2), openat2(2) RESOLVE_*. */
#include "test_fs_util.h"

static char ROOT[PATH_MAX], BIND[PATH_MAX];
static lkd_task *T;

/* ---- A small virtual mount, like procfs: fd/N magic links, an ordinary link and a loop ----------- */

static int vp_getattr(lkd_task *t, void *priv, const char *rel, int nofollow, struct lkd_vattr *out)
{
    (void)priv; (void)nofollow;
    memset(out, 0, sizeof *out);
    if (!rel[0] || !strcmp(rel, "fd")) { out->mode = LX_S_IFDIR | 0555; out->ino = rel[0] ? 3 : 1; return 0; }
    if (!strcmp(rel, "link")) { out->mode = LX_S_IFLNK | 0777; out->ino = 4; return 0; }
    if (!strcmp(rel, "loop")) { out->mode = LX_S_IFLNK | 0777; out->ino = 5; return 0; }
    if (!strcmp(rel, "note")) { out->mode = LX_S_IFREG | 0400; out->ino = 6; out->uid = 1000; out->gid = 1000; return 0; }
    if (!strncmp(rel, "fd/", 3)) {
        lkd_fdobj *o = lkd_fd_get(t, atoi(rel + 3), NULL);
        if (!o) return -LX_ENOENT;
        lkd_fdobj_put(o);
        out->mode = LX_S_IFLNK | 0700;
        out->magic_link = 1;
        out->ino = 100 + (uint64_t)atoi(rel + 3);
        return 0;
    }
    return -LX_ENOENT;
}
static int64_t vp_readlink(lkd_task *t, void *priv, const char *rel, char *buf, size_t cap)
{
    (void)priv;
    if (!strcmp(rel, "link")) { memcpy(buf, "/etc", 4); return 4; }
    if (!strcmp(rel, "loop")) { memcpy(buf, "loop", 4); return 4; }
    if (!strncmp(rel, "fd/", 3)) {
        lkd_fdobj *o = lkd_fd_get(t, atoi(rel + 3), NULL);
        if (!o) return -LX_ENOENT;
        int n = lkd_fs_describe(o, buf, cap);
        lkd_fdobj_put(o);
        return n;
    }
    return -LX_EINVAL;
}
static int vp_open(lkd_task *t, void *priv, const char *rel, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{
    (void)priv; (void)mode;
    if (!strncmp(rel, "fd/", 3)) {
        lkd_fdobj *o = lkd_fd_get(t, atoi(rel + 3), NULL);
        if (!o) return -LX_ENOENT;
        int rc = lkd_fdobj_reopen(t, o, fl, out);
        lkd_fdobj_put(o);
        return rc;
    }
    return -LX_EACCES;
}
static int vp_readdir(lkd_task *t, void *priv, const char *rel, uint64_t i, char name[256], uint8_t *ty, uint64_t *ino)
{
    (void)priv;
    if (!rel[0]) {
        static const char *n[] = { "fd", "link", "loop", "note" };
        static const uint8_t ty_[] = { LX_DT_DIR, LX_DT_LNK, LX_DT_LNK, LX_DT_REG };
        if (i >= 4) return 0;
        snprintf(name, 256, "%s", n[i]); *ty = ty_[i]; *ino = 3 + i;
        return 1;
    }
    if (!strcmp(rel, "fd")) {
        int fds[64];
        int n = lkd_fd_list(t, fds, 64);
        if ((int)i >= n) return 0;
        snprintf(name, 256, "%d", fds[i]); *ty = LX_DT_LNK; *ino = 100 + (uint64_t)fds[i];
        return 1;
    }
    return -LX_ENOTDIR;
}
static const lkd_vmount_ops vp_ops = { .fstype = "proc", .getattr = vp_getattr, .readlink = vp_readlink,
                                       .open = vp_open, .readdir = vp_readdir };

/* ---- Fixture ----------------------------------------------------------------------------------- */

static void build_tree(void)
{
    put_dir(ROOT, "etc");
    put_file(ROOT, "etc/hosts", "127.0.0.1 localhost\n");
    put_dir(ROOT, "usr"); put_dir(ROOT, "usr/lib"); put_dir(ROOT, "usr/lib/x");
    put_file(ROOT, "usr/lib/x/libc.so", "\177ELF");
    put_link(ROOT, "lib", "usr/lib");
    put_link(ROOT, "lib64", "/usr/lib");
    put_dir(ROOT, "x");
    put_link(ROOT, "x/sub", "/usr/lib");
    put_link(ROOT, "loop1", "loop2");
    put_link(ROOT, "loop2", "loop1");
    put_link(ROOT, "self", "self");
    put_link(ROOT, "dangling", "nowhere");
    put_link(ROOT, "escape", "../../../../../../../../etc/passwd");
    put_link(ROOT, "hostabs", "/private");
    put_dir(ROOT, "chain");
    put_file(ROOT, "chain/end", "end\n");
    char name[32], target[32];
    for (int i = 0; i <= 40; i++) {
        snprintf(name, sizeof name, "chain/l%d", i);
        if (i == 40) snprintf(target, sizeof target, "end");
        else snprintf(target, sizeof target, "l%d", i + 1);
        put_link(ROOT, name, target);
    }
    put_dir(ROOT, "home");
    put_dir(ROOT, "home/u");
    put_file(BIND, "data.txt", "bound\n");
}

static int64_t gopen(const char *p, uint32_t fl) { return SC(T, openat, AT_CWD, P(p), fl, 0644); }
static int64_t gstat(const char *p, struct lx_stat *st) { return SC(T, newfstatat, AT_CWD, P(p), P(st), 0); }
static int64_t glstat(const char *p, struct lx_stat *st) { return SC(T, newfstatat, AT_CWD, P(p), P(st), LX_AT_SYMLINK_NOFOLLOW); }

/* path_resolution(7): absolute symlink targets are resolved from the process root, so /lib64 -> /usr/lib
 * stays inside the guest root. */
LT_TEST(basic_resolution)
{
    struct lx_stat st;
    LT_EQ(gstat("/etc/hosts", &st), 0);
    LT_EQ(st.st_size, 20);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_EQ(gstat("/lib/x/libc.so", &st), 0);           /* relative link */
    LT_EQ(st.st_size, 4);
    LT_EQ(gstat("/lib64/x/libc.so", &st), 0);         /* absolute link, guest namespace */
    LT_EQ(gstat("//etc///hosts", &st), 0);            /* repeated slashes */
    LT_EQ(gstat("/etc/./hosts", &st), 0);
    LT_ERRNO(gstat("/etc/missing", &st), LX_ENOENT);
    LT_ERRNO(gstat("/etc/hosts/x", &st), LX_ENOTDIR);  /* component not a directory */
    LT_ERRNO(gstat("/etc/hosts/", &st), LX_ENOTDIR);   /* trailing slash requires a directory */
    LT_ERRNO(gstat("", &st), LX_ENOENT);
}

/* ".." after a symlink applies to the directory the link led to (namei.c follow_dotdot on the
 * physical path): /x/sub -> /usr/lib, so /x/sub/../lib is /usr/lib, not /x/lib. */
LT_TEST(dotdot_after_link)
{
    struct lx_stat st;
    LT_EQ(gstat("/x/sub/../lib/x/libc.so", &st), 0);
    LT_EQ(gstat("/x/sub/..", &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFDIR);
    char rp[LX_PATH_MAX];
    LT_EQ(lkd_vfs_realpath(T, LX_AT_FDCWD, "/x/sub/../lib", rp, sizeof rp), 8);
    LT_STREQ(rp, "/usr/lib");
    /* ".." at the root stays at the root (cannot climb out of the guest root) */
    LT_EQ(lkd_vfs_realpath(T, LX_AT_FDCWD, "/../../..", rp, sizeof rp), 1);
    LT_STREQ(rp, "/");
    LT_EQ(gstat("/../../etc/hosts", &st), 0);
}

/* ELOOP: a loop, a self loop, and exactly MAXSYMLINKS (40) followed links allowed, 41 refused. */
LT_TEST(symlink_loops)
{
    struct lx_stat st;
    LT_ERRNO(gstat("/loop1", &st), LX_ELOOP);
    LT_ERRNO(gstat("/self", &st), LX_ELOOP);
    LT_ERRNO(gopen("/loop1", LX_O_RDONLY), LX_ELOOP);
    LT_EQ(glstat("/loop1", &st), 0);                   /* lstat does not follow */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    LT_EQ(gstat("/chain/l1", &st), 0);                 /* l1..l40 + end: 40 links */
    LT_ERRNO(gstat("/chain/l0", &st), LX_ELOOP);       /* 41 links */
    LT_ERRNO(gstat("/self/x", &st), LX_ELOOP);         /* interior loop */
}

/* Escapes: a relative link climbing above the root stays inside; an absolute link naming a host
 * directory (/private) means the guest's /private, which does not exist. */
LT_TEST(no_host_escape)
{
    struct lx_stat st;
    LT_ERRNO(gstat("/escape", &st), LX_ENOENT);
    LT_ERRNO(gstat("/hostabs", &st), LX_ENOENT);
    LT_ERRNO(gstat("/hostabs/etc/hosts", &st), LX_ENOENT);
    char hp[PATH_MAX];
    LT_EQ(lkd_vfs_to_host(T, LX_AT_FDCWD, "/lib/x/libc.so", 0, hp, sizeof hp), 0);
    char want[PATH_MAX];
    snprintf(want, sizeof want, "%s/usr/lib/x/libc.so", ROOT);
    LT_STREQ(hp, want);
}

/* open(2) on symlinks: O_NOFOLLOW on a final link -ELOOP; O_PATH|O_NOFOLLOW gives the link itself;
 * O_CREAT through a dangling link creates the target; O_CREAT|O_EXCL on a link -EEXIST. */
LT_TEST(open_symlink_rules)
{
    LT_ERRNO(gopen("/lib64", LX_O_RDONLY | LX_O_NOFOLLOW), LX_ELOOP);
    int64_t fd = gopen("/lib64", LX_O_PATH | LX_O_NOFOLLOW);
    LT_CHECK(fd >= 0);
    struct lx_stat st;
    LT_EQ(SC(T, fstat, fd, P(&st)), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    char buf[64];
    LT_EQ(SC(T, readlinkat, fd, P(""), P(buf), sizeof buf), 8);   /* readlinkat(fd, "") on O_PATH link */
    LT_MEMEQ(buf, "/usr/lib", 8);
    LT_ERRNO(SC(T, read, fd, P(buf), 1), LX_EBADF);               /* O_PATH: no I/O */
    SC(T, close, fd);
    LT_ERRNO(gopen("/dangling", LX_O_CREAT | LX_O_EXCL | LX_O_WRONLY), LX_EEXIST);
    fd = gopen("/dangling", LX_O_CREAT | LX_O_WRONLY);
    LT_CHECK(fd >= 0);
    SC(T, close, fd);
    LT_EQ(gstat("/nowhere", &st), 0);
    LT_EQ(st.st_mode & 0777, 0644);
}

/* Binds: a host directory at /mnt/data; /mnt does not exist in the root and appears as a directory
 * (mount ancestor), listed by getdents of "/". Rename across mounts -EXDEV; a read-only bind -EROFS. */
LT_TEST(binds_and_synthetic)
{
    LT_EQ(lkd_vfs_mount_bind("/mnt/data", BIND, 0), 0);
    struct lx_stat st;
    LT_EQ(gstat("/mnt", &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFDIR);
    LT_EQ(gstat("/mnt/data/data.txt", &st), 0);
    LT_EQ(st.st_size, 6);
    int64_t fd = gopen("/mnt/data/data.txt", LX_O_RDONLY);
    char buf[16] = { 0 };
    LT_EQ(SC(T, read, fd, P(buf), sizeof buf), 6);
    LT_STREQ(buf, "bound\n");
    SC(T, close, fd);
    LT_ERRNO(SC(T, rename, P("/mnt/data/data.txt"), P("/etc/data.txt")), LX_EXDEV);
    LT_ERRNO(SC(T, rmdir, P("/mnt/data")), LX_EBUSY);   /* mount point */
    /* "/" lists "mnt" although the root directory has no such entry */
    fd = gopen("/", LX_O_RDONLY | LX_O_DIRECTORY);
    char d[8192];
    int64_t n = SC(T, getdents64, fd, P(d), sizeof d);
    LT_CHECK(n > 0);
    int saw_mnt = 0, saw_etc = 0;
    for (int64_t p = 0; p < n;) {
        uint16_t rl;
        memcpy(&rl, d + p + 16, 2);
        if (!strcmp(d + p + 19, "mnt")) saw_mnt = d[p + 18] == LX_DT_DIR;
        if (!strcmp(d + p + 19, "etc")) saw_etc = 1;
        p += rl;
    }
    LT_CHECK(saw_mnt);
    LT_CHECK(saw_etc);
    SC(T, close, fd);
    char ro[PATH_MAX];
    snprintf(ro, sizeof ro, "%s", BIND);
    LT_EQ(lkd_vfs_mount_bind("/ro", ro, 1), 0);
    LT_ERRNO(gopen("/ro/new", LX_O_CREAT | LX_O_WRONLY), LX_EROFS);
    LT_ERRNO(gopen("/ro/data.txt", LX_O_WRONLY), LX_EROFS);
    LT_EQ(gopen("/ro/data.txt", LX_O_RDONLY) >= 0, 1);
    LT_ERRNO(SC(T, mkdir, P("/ro/d"), 0755), LX_EROFS);
    LT_ERRNO(SC(T, access, P("/ro/data.txt"), LX_W_OK), LX_EROFS);
    char *m = lkd_vfs_render_mounts(T, 0, NULL);
    LT_CHECK(m && strstr(m, " /mnt/data ") && strstr(m, " /ro ") && strstr(m, "ro,relatime"));
    free(m);
    m = lkd_vfs_render_mounts(T, 1, NULL);
    LT_CHECK(m && strstr(m, " - "));
    free(m);
}

/* Virtual mount: getattr/readdir; an ordinary virtual link is followed textually; a magic link opens
 * the object through the mount (an independent description: own offset, fs/proc/base.c). */
LT_TEST(virtual_mount_and_magic_links)
{
    LT_EQ(lkd_vfs_mount_virtual("/vproc", &vp_ops, NULL), 0);
    LT_ERRNO(lkd_vfs_mount_virtual("/vproc", &vp_ops, NULL), LX_EBUSY);
    struct lx_stat st;
    LT_EQ(gstat("/vproc/link/hosts", &st), 0);         /* /vproc/link -> /etc */
    LT_ERRNO(gstat("/vproc/loop", &st), LX_ELOOP);
    LT_EQ(gstat("/vproc/note", &st), 0);
    LT_EQ(st.st_blksize, 1024);
    LT_EQ(SC(T, access, P("/vproc/note"), LX_R_OK), 0);
    LT_ERRNO(SC(T, access, P("/vproc/note"), LX_W_OK), LX_EACCES);
    int64_t fd = gopen("/etc/hosts", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    char buf[64], path[64];
    LT_EQ(SC(T, read, fd, P(buf), 4), 4);
    snprintf(path, sizeof path, "/vproc/fd/%d", (int)fd);
    int64_t n = SC(T, readlink, P(path), P(buf), sizeof buf);
    LT_EQ(n, 10);
    LT_MEMEQ(buf, "/etc/hosts", 10);
    int64_t fd2 = gopen(path, LX_O_RDONLY);           /* reopen through the magic link */
    LT_CHECK(fd2 >= 0 && fd2 != fd);
    LT_EQ(SC(T, lseek, fd2, 0, LX_SEEK_CUR), 0);      /* own offset */
    LT_EQ(SC(T, lseek, fd, 0, LX_SEEK_CUR), 4);
    LT_EQ(gstat(path, &st), 0);                        /* stat follows to the object */
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFREG);
    LT_EQ(glstat(path, &st), 0);
    LT_EQ(st.st_mode & LX_S_IFMT, LX_S_IFLNK);
    /* getdents on the virtual directory: ".", "..", then the mount's entries */
    int64_t dfd = gopen("/vproc", LX_O_RDONLY | LX_O_DIRECTORY);
    LT_CHECK(dfd >= 0);
    char d[4096];
    n = SC(T, getdents64, dfd, P(d), sizeof d);
    LT_CHECK(n > 0);
    LT_STREQ(d + 19, ".");
    int cnt = 0;
    for (int64_t p = 0; p < n; cnt++) { uint16_t rl; memcpy(&rl, d + p + 16, 2); p += rl; }
    LT_EQ(cnt, 6);
    LT_ERRNO(SC(T, openat, dfd, P("note"), LX_O_WRONLY), LX_EACCES);
    SC(T, close, dfd);
    SC(T, close, fd);
    SC(T, close, fd2);
    /* a deleted file: readlink shows " (deleted)" (d_path), the reopen gets its own offset and
     * SEEK_DATA on it leaves the original description's offset alone */
    int64_t del = gopen("/home/u/gone", LX_O_CREAT | LX_O_RDWR);
    LT_EQ(SC(T, write, del, P("abcdef"), 6), 6);
    LT_EQ(SC(T, unlink, P("/home/u/gone")), 0);
    snprintf(path, sizeof path, "/vproc/fd/%d", (int)del);
    n = SC(T, readlink, P(path), P(buf), sizeof buf);
    LT_EQ(n, 22);
    LT_MEMEQ(buf, "/home/u/gone (deleted)", 22);
    int64_t re = gopen(path, LX_O_RDWR);
    LT_CHECK(re >= 0);
    LT_EQ(SC(T, read, re, P(buf), 2), 2);
    LT_MEMEQ(buf, "ab", 2);
    LT_EQ(SC(T, lseek, re, 1, LX_SEEK_DATA), 1);
    LT_EQ(SC(T, lseek, re, 1, LX_SEEK_HOLE), 6);
    LT_EQ(SC(T, lseek, del, 0, LX_SEEK_CUR), 6);
    LT_ERRNO(gopen(path, LX_O_RDONLY | LX_O_TRUNC | LX_O_DIRECTORY), LX_ENOTDIR);
    SC(T, close, re);
    SC(T, close, del);
}

/* chdir/fchdir/getcwd and *at with a directory fd. */
LT_TEST(cwd_and_dirfd)
{
    char buf[256];
    LT_EQ(SC(T, chdir, P("/lib/x")), 0);
    LT_EQ(SC(T, getcwd, P(buf), sizeof buf), 11);      /* strlen("/usr/lib/x") + 1 */
    LT_STREQ(buf, "/usr/lib/x");
    struct lx_stat st;
    LT_EQ(gstat("libc.so", &st), 0);
    LT_ERRNO(SC(T, getcwd, P(buf), 5), LX_ERANGE);
    LT_ERRNO(SC(T, chdir, P("/etc/hosts")), LX_ENOTDIR);
    int64_t dfd = gopen("/etc", LX_O_RDONLY | LX_O_DIRECTORY);
    LT_EQ(SC(T, newfstatat, dfd, P("hosts"), P(&st), 0), 0);
    LT_EQ(SC(T, newfstatat, dfd, P("../usr/lib/x/libc.so"), P(&st), 0), 0);
    int64_t ffd = gopen("/etc/hosts", LX_O_RDONLY);
    LT_ERRNO(SC(T, newfstatat, ffd, P("x"), P(&st), 0), LX_ENOTDIR);   /* dirfd not a directory */
    LT_ERRNO(SC(T, newfstatat, 999, P("x"), P(&st), 0), LX_EBADF);
    LT_EQ(SC(T, newfstatat, 999, P("/etc/hosts"), P(&st), 0), 0);       /* absolute path ignores dirfd */
    LT_EQ(SC(T, newfstatat, ffd, P(""), P(&st), LX_AT_EMPTY_PATH), 0);
    LT_EQ(st.st_size, 20);
    LT_EQ(SC(T, fchdir, dfd), 0);
    LT_EQ(SC(T, getcwd, P(buf), sizeof buf), 5);
    LT_STREQ(buf, "/etc");
    LT_ERRNO(SC(T, fchdir, ffd), LX_ENOTDIR);
    SC(T, close, dfd);
    SC(T, close, ffd);
    LT_EQ(SC(T, chdir, P("/")), 0);
}

/* chroot(2): -EPERM without CAP_SYS_CHROOT; as euid 0, ".." and absolute links stop at the new root;
 * getcwd outside the root reports "(unreachable)". */
LT_TEST(chroot_rules)
{
    LT_ERRNO(SC(T, chroot, P("/usr")), LX_EPERM);
    lkd_task *r = fs_task();
    r->proc->cred.euid = 0;
    LT_EQ(SC(r, chdir, P("/etc")), 0);
    LT_EQ(SC(r, chroot, P("/usr")), 0);
    struct lx_stat st;
    LT_EQ(SC(r, newfstatat, AT_CWD, P("/lib/x/libc.so"), P(&st), 0), 0);
    LT_EQ(SC(r, newfstatat, AT_CWD, P("/../../lib/x/libc.so"), P(&st), 0), 0);
    LT_ERRNO(SC(r, newfstatat, AT_CWD, P("/etc/hosts"), P(&st), 0), LX_ENOENT);
    char buf[128];
    LT_CHECK(SC(r, getcwd, P(buf), sizeof buf) > 0);
    LT_STREQ(buf, "(unreachable)/etc");
    lkd_set_current(T);
}

/* openat2(2) RESOLVE_*: BENEATH refuses absolute paths and ".." escapes (-EXDEV), NO_SYMLINKS any link
 * (-ELOOP), IN_ROOT clamps, unknown resolve bits -EINVAL, extra non-zero struct bytes -E2BIG. */
LT_TEST(openat2_resolve)
{
    struct { uint64_t flags, mode, resolve; uint64_t extra; } how = { LX_O_RDONLY, 0, 0, 0 };
    int64_t dfd = gopen("/usr", LX_O_RDONLY | LX_O_DIRECTORY);
    how.resolve = 0x08; /* BENEATH */
    LT_ERRNO(SC(T, openat2, dfd, P("/etc/hosts"), P(&how), 24), LX_EXDEV);
    LT_ERRNO(SC(T, openat2, dfd, P("../etc/hosts"), P(&how), 24), LX_EXDEV);
    int64_t fd = SC(T, openat2, dfd, P("lib/x/libc.so"), P(&how), 24);
    LT_CHECK(fd >= 0);
    SC(T, close, fd);
    how.resolve = 0x04; /* NO_SYMLINKS */
    LT_ERRNO(SC(T, openat2, AT_CWD, P("/lib/x/libc.so"), P(&how), 24), LX_ELOOP);
    how.resolve = 0x10; /* IN_ROOT: "/" means dfd */
    fd = SC(T, openat2, dfd, P("/lib/x/libc.so"), P(&how), 24);
    LT_CHECK(fd >= 0);
    SC(T, close, fd);
    how.resolve = 0x40;
    LT_ERRNO(SC(T, openat2, dfd, P("lib"), P(&how), 24), LX_EINVAL);
    how.resolve = 0;
    how.extra = 1;
    LT_ERRNO(SC(T, openat2, dfd, P("lib"), P(&how), 32), LX_E2BIG);
    LT_ERRNO(SC(T, openat2, dfd, P("lib"), P(&how), 16), LX_EINVAL);
    how.extra = 0;
    how.flags = 1ull << 40;
    LT_ERRNO(SC(T, openat2, dfd, P("lib"), P(&how), 24), LX_EINVAL);
    SC(T, close, dfd);
}

/* Case sensitivity: APFS here is case-insensitive (MESURE pathconf _PC_CASE_SENSITIVE = 0). Linux
 * would answer ENOENT for /ETC/HOSTS; the host finds it. This test records the documented gap: the
 * resolution succeeds and the canonical path keeps the spelling given. */
LT_TEST(case_insensitivity_gap)
{
    long cs = pathconf(ROOT, _PC_CASE_SENSITIVE);
    struct lx_stat st;
    if (cs == 0) LT_EQ(gstat("/ETC/HOSTS", &st), 0);
    else LT_ERRNO(gstat("/ETC/HOSTS", &st), LX_ENOENT);
}

int main(void)
{
    make_tmpdir(ROOT, sizeof ROOT, "root");
    make_tmpdir(BIND, sizeof BIND, "bind");
    build_tree();
    lkd_config cfg = { .size = sizeof cfg, .guest_root = ROOT };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); return 2; }
    T = fs_task();
    LT_RUN(basic_resolution);
    LT_RUN(dotdot_after_link);
    LT_RUN(symlink_loops);
    LT_RUN(no_host_escape);
    LT_RUN(open_symlink_rules);
    LT_RUN(binds_and_synthetic);
    LT_RUN(virtual_mount_and_magic_links);
    LT_RUN(cwd_and_dirfd);
    LT_RUN(chroot_rules);
    LT_RUN(openat2_resolve);
    LT_RUN(case_insensitivity_gap);
    rm_rf(ROOT);
    rm_rf(BIND);
    return lt_finish();
}
