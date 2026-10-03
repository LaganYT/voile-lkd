/* test_procfs_util.h: helpers shared by the tests/test_procfs_*.c programs (procfs module).
 * The task runs with a real mm: every guest pointer handed to a syscall points into one scratch
 * mapping obtained from lkd_mm_map (identity window, lkd_guest_base == 0). */
#ifndef TEST_PROCFS_UTIL_H
#define TEST_PROCFS_UTIL_H

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "lkd_internal.h"
#include "fd.h"
#include "vfs.h"
#include "mm.h"
#include <lkd/syscall_nr.h>
#include "lkd_fs.h"
#include "lkd_procfs.h"

#define P(x) ((uint64_t)(uintptr_t)(x))
#define AT_CWD ((uint64_t)(int64_t)LX_AT_FDCWD)
#define SCRATCH_SZ (8u << 20)

static lkd_task *T;
static uint8_t *SB;          /* scratch mapping, guest address == host address */

static int64_t sc_call(lkd_task *t, unsigned nr, const uint64_t *a, size_t n)
{
    lkd_sysreq rq = { .nr = nr };
    for (size_t i = 0; i < n && i < 6; i++) rq.a[i] = a[i];
    int64_t rax = 0;
    lkd_disp d = lkd_syscall(t, &rq, &rax);
    if (d != LKD_RETURN) return -100000 - (int64_t)d;
    return rax;
}
#define SC(t, name, ...) sc_call((t), LKD_NR_##name, (const uint64_t[]){ __VA_ARGS__ }, \
                                 sizeof((const uint64_t[]){ __VA_ARGS__ }) / sizeof(uint64_t))

/* Scratch layout: [0, 64K) strings and small buffers, [64K, 8M) bulk read buffer. */
static uint64_t gstr(const char *s)
{
    static size_t off;
    size_t n = strlen(s) + 1;
    if (off + n > 60000) off = 0;
    memcpy(SB + off, s, n);
    uint64_t g = (uint64_t)(uintptr_t)(SB + off);
    off += n;
    return g;
}
static uint64_t gbulk(void) { return (uint64_t)(uintptr_t)(SB + 65536); }
#define BULK_MAX (SCRATCH_SZ - 65536 - 4096)

static int64_t gopen(const char *path, uint32_t fl) { return SC(T, openat, AT_CWD, gstr(path), fl, 0); }

/* Whole content of a file through open/read/close, malloc'ed and NUL terminated. *len = bytes, or a
 * negative errno (and NULL returned) when the open or a read failed. */
static char *slurp_as(lkd_task *t, const char *path, int64_t *len)
{
    lkd_task *prev = lkd_current();
    lkd_set_current(t);
    int64_t fd = SC(t, openat, AT_CWD, gstr(path), LX_O_RDONLY, 0);
    if (fd < 0) { *len = fd; lkd_set_current(prev); return NULL; }
    size_t cap = 4096, n = 0;
    char *out = malloc(cap + 1);
    for (;;) {
        int64_t r = SC(t, read, (uint64_t)fd, gbulk(), 65536);
        if (r < 0) { free(out); *len = r; SC(t, close, (uint64_t)fd); lkd_set_current(prev); return NULL; }
        if (r == 0) break;
        if (n + (size_t)r > cap) { while (n + (size_t)r > cap) cap *= 2; out = realloc(out, cap + 1); }
        memcpy(out + n, (void *)(uintptr_t)gbulk(), (size_t)r);
        n += (size_t)r;
    }
    out[n] = 0;
    SC(t, close, (uint64_t)fd);
    *len = (int64_t)n;
    lkd_set_current(prev);
    return out;
}
static char *slurp(const char *path, int64_t *len) { return slurp_as(T, path, len); }

/* readlink through the syscall, into a C string; returns the length or a negative errno. */
static int64_t rdlink_as(lkd_task *t, const char *path, char *out, size_t cap)
{
    lkd_task *prev = lkd_current();
    lkd_set_current(t);
    int64_t r = SC(t, readlink, gstr(path), gbulk(), cap - 1);
    if (r >= 0) { memcpy(out, (void *)(uintptr_t)gbulk(), (size_t)r); out[r] = 0; }
    lkd_set_current(prev);
    return r;
}
static int64_t rdlink(const char *path, char *out, size_t cap) { return rdlink_as(T, path, out, cap); }

static int64_t gstat_as(lkd_task *t, const char *path, struct lx_stat *st, int flags)
{
    lkd_task *prev = lkd_current();
    lkd_set_current(t);
    int64_t r = SC(t, newfstatat, AT_CWD, gstr(path), gbulk(), (uint64_t)flags);
    if (r == 0) memcpy(st, (void *)(uintptr_t)gbulk(), sizeof *st);
    lkd_set_current(prev);
    return r;
}
static int64_t gstat(const char *path, struct lx_stat *st) { return gstat_as(T, path, st, 0); }
static int64_t glstat(const char *path, struct lx_stat *st) { return gstat_as(T, path, st, LX_AT_SYMLINK_NOFOLLOW); }

/* Value of "Key:\t..." in a status text (pointer into text, or NULL). */
static const char *field(const char *text, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p;) {
        if (!strncmp(p, key, kl) && p[kl] == ':') return p + kl + 1;
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}
/* Whole line of the field, without newline, in a static buffer. */
static const char *fline(const char *text, const char *key)
{
    static char buf[512];
    const char *v = field(text, key);
    if (!v) return NULL;
    const char *nl = strchr(v, '\n');
    size_t n = nl ? (size_t)(nl - v) : strlen(v);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    memcpy(buf, v, n);
    buf[n] = 0;
    return buf;
}

static void procfs_fixture(void)
{
    lkd_config cfg = { .size = sizeof cfg };
    if (lkd_init_config(&cfg) < 0 || fake_engine_init() < 0) { fprintf(stderr, "init failed\n"); exit(2); }
    T = fake_make_task();
    T->fdt = lkd_fdtable_new();
    T->fsctx = lkd_fsctx_new();
    T->mm = lkd_mm_create();
    if (!T->mm) { fprintf(stderr, "mm failed\n"); exit(2); }
    int64_t a = lkd_mm_map(T->mm, 0, SCRATCH_SZ, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, NULL, 0, NULL);
    if (a < 0) { fprintf(stderr, "scratch map failed %lld\n", (long long)a); exit(2); }
    SB = lkd_g2h((uint64_t)a);
}

#endif
