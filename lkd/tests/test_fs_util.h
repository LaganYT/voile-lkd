/* test_fs_util.h: helpers shared by the tests/test_fs_*.c programs (fs module). */
#ifndef TEST_FS_UTIL_H
#define TEST_FS_UTIL_H

#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "lkd_internal.h"
#include "fd.h"
#include "vfs.h"
#include <lkd/syscall_nr.h>
#include "lkd_fs.h"

#define P(x) ((uint64_t)(uintptr_t)(x))

static inline int64_t sc_call(lkd_task *t, unsigned nr, const uint64_t *a, size_t n)
{
    lkd_sysreq rq = { .nr = nr };
    for (size_t i = 0; i < n && i < 6; i++) rq.a[i] = a[i];
    int64_t rax = 0;
    lkd_disp d = lkd_syscall(t, &rq, &rax);
    if (d != LKD_RETURN) return -100000 - (int64_t)d;
    return rax;
}
/* SC(t, name, args...): run one guest syscall through lkd_syscall and return RAX. */
#define SC(t, name, ...) sc_call((t), LKD_NR_##name, (const uint64_t[]){ __VA_ARGS__ }, \
                                 sizeof((const uint64_t[]){ __VA_ARGS__ }) / sizeof(uint64_t))
#define SC0(t, name) sc_call((t), LKD_NR_##name, NULL, 0)
#define AT_CWD ((uint64_t)(int64_t)LX_AT_FDCWD)

static int rm_cb(const char *p, const struct stat *s, int f, struct FTW *w)
{
    (void)s; (void)f; (void)w;
    if (remove(p) < 0) { chmod(p, 0700); remove(p); }
    return 0;
}
static inline void rm_rf(const char *p) { nftw(p, rm_cb, 32, FTW_DEPTH | FTW_PHYS); }

/* A fresh host temp directory (realpath'ed). */
static inline void make_tmpdir(char *out, size_t cap, const char *tag)
{
    char base[PATH_MAX], tmpl[PATH_MAX];
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, base, sizeof base);
    if (!n) snprintf(base, sizeof base, "/tmp/");
    snprintf(tmpl, sizeof tmpl, "%slkdfs-%s-XXXXXX", base, tag);
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); exit(2); }
    if (!realpath(tmpl, out)) snprintf(out, cap, "%s", tmpl);
}

static inline void put_file(const char *root, const char *rel, const char *content)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { perror(p); exit(2); }
    if (content) (void)!write(fd, content, strlen(content));
    close(fd);
}
static inline void put_dir(const char *root, const char *rel)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    mkdir(p, 0755);
}
static inline void put_link(const char *root, const char *rel, const char *target)
{
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    unlink(p);
    if (symlink(target, p) < 0) { perror(p); exit(2); }
}

/* A task with its own fd table and fsctx, bound as current. */
static inline lkd_task *fs_task(void)
{
    lkd_task *t = fake_make_task();
    if (!t) { fprintf(stderr, "fake_make_task failed\n"); exit(2); }
    t->fdt = lkd_fdtable_new();
    t->fsctx = lkd_fsctx_new();
    return t;
}

#endif
