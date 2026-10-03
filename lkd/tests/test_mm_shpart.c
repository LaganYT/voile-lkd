/* test_mm_shpart.c: MAP_SHARED mappings that cover less than a 16 KiB host page (docs/status/mm.md,
 * "pages hôte partagées partiellement"). The motivating case is Wine: the client maps the 4 KiB
 * KUSER_SHARED_DATA memfd of wineserver MAP_FIXED|MAP_SHARED over its own 4 KiB reservation at
 * 0x7ffe0000 and the server updates it every few ms, so the page must be truly shared.
 * Linux references: mmap(2) MAP_SHARED ("updates ... visible to other processes that map this file"),
 * MAP_FIXED, mprotect(2) ENOMEM, fork(2) (shared mappings stay shared), memfd_create(2). */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

static lkd_task *T;
static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define MPROTECT(a, l, p) sc(LKD_NR_mprotect, (a), (l), (p), 0, 0, 0)
#define MUNMAP(a, l) sc(LKD_NR_munmap, (a), (l), 0, 0, 0, 0)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)
#define SHF (LX_MAP_SHARED | LX_MAP_FIXED)
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }

static int t_mmap_fd(lkd_fdobj *o) { return o->host_fd; }
static int t_describe(lkd_fdobj *o, char *buf, size_t cap) { return snprintf(buf, cap, "%s", o->gpath); }
static const lkd_fd_ops mf_ops = { .name = "memfd", .mmap_fd = t_mmap_fd, .describe = t_describe };

/* A memfd-like object: unlinked temp file of `size` bytes, byte i of page k = marker k + 1 at offset 0. */
static lkd_fdobj *mk_obj(uint64_t size, const char *name)
{
    char path[512];
    const char *d = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/lkd_shpart_XXXXXX", d && *d ? d : "/tmp");
    int fd = mkstemp(path);
    unlink(path);
    if (fd < 0 || ftruncate(fd, (off_t)size)) return NULL;
    for (uint64_t k = 0; k * 0x1000 < size; k++) {
        uint8_t m = (uint8_t)(0xa0 + k);
        pwrite(fd, &m, 1, (off_t)(k * 0x1000));
    }
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_MEMFD, &mf_ops, fd, LX_O_RDWR, NULL);
    o->gpath = strdup(name);
    return o;
}

static int host_readable(uint64_t g)
{
    int p[2];
    if (pipe(p)) return -1;
    int r = write(p[1], H(g), 1) == 1;
    close(p[0]); close(p[1]);
    return r;
}

static uint64_t A; /* a 16 KiB aligned base inside the window, fresh per case */
static void up(int slot)
{
    if (!T) { fake_engine_init(); T = fake_make_task(); }
    T->mm = lkd_mm_create();
    T->fdt = lkd_fdtable_new();
    fake_engine_reset();
    A = lkd_mm_window_guest_lo() + 0x40000000ull + (uint64_t)slot * 0x100000ull;
}
static void down(void)
{
    lkd_fdtable_put(T->fdt);
    T->fdt = NULL;
    lkd_mm_put(T->mm);
    T->mm = NULL;
}
#define SMAP(addr, len, prot, flags, o, off) lkd_mm_map(T->mm, (addr), (len), (prot), (flags), (o), (off), NULL)

/* The exact Wine sequence. */
LT_TEST(wine_kuser_shared_data)
{
    up(0);
    lkd_fdobj *o = mk_obj(0x1000, "/memfd:wine-kuser");
    LT_EQ(sc(LKD_NR_mmap, A, 0x1000, LX_PROT_READ, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A);   /* reservation */
    LT_EQ(SMAP(A, 0x1000, LX_PROT_READ, SHF, o, 0), A);                                       /* shared over it */
    LT_EQ(T->mm->vt.n, 1);
    LT_EQ(H(A)[0], 0xa0);                                    /* the object's bytes */
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, A, 0, &vi), 0);
    LT_EQ(vi.end - vi.start, 0x1000);
    LT_EQ(vi.flags & LX_MAP_TYPE, LX_MAP_SHARED);
    LT_STREQ(vi.name, "/memfd:wine-kuser");
    LT_EQ(host_readable(A), 1);
    /* the server updates the object through another mapping, in another process, and through its fd */
    lkd_fdobj *o2 = o;
    int64_t w = SMAP(A + 0x100000, 0x1000, RW, LX_MAP_SHARED, o2, 0);   /* the server's own writable mapping */
    LT_CHECK(w > 0);
    H(w)[0x10] = 0x5c;                                       /* system time tick, say */
    LT_EQ(H(A)[0x10], 0x5c);                                 /* the reader sees it at once */
    H(w)[0x10] = 0x5d;
    LT_EQ(H(A)[0x10], 0x5d);
    pwrite(o->host_fd, "\x77", 1, 0x20);
    LT_EQ(H(A)[0x20], 0x77);
    pid_t p = fork();
    if (p == 0) { /* another process: its own host mapping of the same file */
        uint8_t *m = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, o->host_fd, 0);
        if (m == MAP_FAILED) _exit(1);
        m[0x30] = 0x99;
        _exit(0);
    }
    int st = -1;
    for (int i = 0; i < 1000 && waitpid(p, &st, WNOHANG) != p; i++) usleep(10000);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    LT_EQ(H(A)[0x30], 0x99);
    /* guest side: the RO page cannot be written, the kernel copy is refused */
    uint8_t v = 1;
    LT_ERRNO(lkd_user_write(A + 8, &v, 1), LX_EFAULT);
    LT_EQ(host_readable(A), 1);
    LT_EQ(lkd_mm_find(T->mm, A, 0, &vi), 0);
    LT_EQ(vi.prot, LX_PROT_READ);
    lkd_fdobj_put(o);
    down();
}

LT_TEST(siblings_rules)
{
    up(1);
    lkd_fdobj *o = mk_obj(0x4000, "/memfd:sib");
    LT_EQ(SMAP(A, 0x1000, LX_PROT_READ, SHF, o, 0), A);
    /* creation-time rule: an accessible private mapping in a sibling page is refused, visibly */
    LT_ERRNO(sc(LKD_NR_mmap, A + 0x1000, 0x1000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), LX_EINVAL);
    LT_ERRNO(sc(LKD_NR_mmap, A + 0x3000, 0x1000, LX_PROT_READ, PA | LX_MAP_FIXED, (uint64_t)-1, 0), LX_EINVAL);
    /* a PROT_NONE sibling is fine */
    LT_EQ(sc(LKD_NR_mmap, A + 0x1000, 0x3000, LX_PROT_NONE, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A + 0x1000);
    /* ... and stays unreachable: kernel copies see the exact VMA view */
    uint8_t v;
    LT_ERRNO(lkd_user_read(A + 0x1000, &v, 1), LX_EFAULT);
    LT_EQ(lkd_user_read(A, &v, 1), 0);
    LT_EQ(v, 0xa0);
    /* later rule: making a sibling accessible is refused (it would alias the object's bytes) */
    LT_ERRNO(MPROTECT(A + 0x1000, 0x1000, LX_PROT_READ), LX_ENOMEM);
    LT_ERRNO(MPROTECT(A + 0x2000, 0x2000, RW), LX_ENOMEM);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, A + 0x1000, 0, &vi), 0);
    LT_EQ(vi.prot, 0);
    /* the shared VMA itself can change protection; the host page follows the union rule */
    LT_EQ(MPROTECT(A, 0x1000, LX_PROT_NONE), 0);
    LT_EQ(host_readable(A), 0);
    LT_EQ(MPROTECT(A, 0x1000, LX_PROT_READ), 0);
    LT_EQ(host_readable(A), 1);
    /* writes to PROT_NONE siblings never reach the object: no accessible private mapping can be made, and
     * the file bytes at 0x1000 stay what they were */
    uint8_t fb;
    pread(o->host_fd, &fb, 1, 0x1000);
    LT_EQ(fb, 0xa1);
    /* two pieces of the SAME object at the same relative position share the host page: allowed */
    LT_EQ(SMAP(A + 0x1000, 0x1000, LX_PROT_READ, SHF, o, 0x1000), A + 0x1000);
    LT_EQ(H(A + 0x1000)[0], 0xa1);
    /* a different object cannot join the host page */
    lkd_fdobj *o2 = mk_obj(0x4000, "/memfd:other");
    LT_ERRNO(SMAP(A + 0x2000, 0x1000, LX_PROT_READ, SHF, o2, 0x2000), LX_EINVAL);
    /* a piece of the same object at another relative position (offset 0 at A+0x2000) cannot either */
    LT_ERRNO(SMAP(A + 0x2000, 0x1000, LX_PROT_READ, SHF, o, 0), LX_EINVAL);
    lkd_fdobj_put(o2);
    lkd_fdobj_put(o);
    down();
}

LT_TEST(sizes_and_offsets)
{
    up(2);
    lkd_fdobj *o = mk_obj(0x8000, "/memfd:sizes");
    /* 4 KiB, 8 KiB, 12 KiB at 16 KiB aligned addresses, offset 0 */
    LT_EQ(SMAP(A, 0x1000, LX_PROT_READ, SHF, o, 0), A);
    LT_EQ(SMAP(A + 0x10000, 0x2000, LX_PROT_READ, SHF, o, 0), A + 0x10000);
    LT_EQ(SMAP(A + 0x20000, 0x3000, RW, SHF, o, 0), A + 0x20000);
    for (int k = 0; k < 3; k++) LT_EQ(H(A + 0x20000 + (uint64_t)k * 0x1000)[0], 0xa0 + k);
    LT_EQ(H(A + 0x10000 + 0x1000)[0], 0xa1);
    /* 4 KiB at file offset 4 KiB placed at an address with the same position in the host page */
    LT_EQ(SMAP(A + 0x31000, 0x1000, LX_PROT_READ, SHF, o, 0x1000), A + 0x31000);
    LT_EQ(H(A + 0x31000)[0], 0xa1);
    /* offset 4 KiB at a 16 KiB aligned address cannot be mapped by whole host pages: EINVAL */
    LT_ERRNO(SMAP(A + 0x40000, 0x1000, LX_PROT_READ, SHF, o, 0x1000), LX_EINVAL);
    /* offset smaller than the position in the host page (nothing before it in the object): EINVAL */
    LT_ERRNO(SMAP(A + 0x52000, 0x1000, LX_PROT_READ, SHF, o, 0), LX_EINVAL);
    /* non-fixed shared mappings pick an address with the right position */
    int64_t q = SMAP(0, 0x1000, LX_PROT_READ, LX_MAP_SHARED, o, 0x2000);
    LT_CHECK(q > 0);
    LT_EQ(q & 0x3fff, 0x2000);
    LT_EQ(H(q)[0], 0xa2);
    /* the pieces of a 12 KiB mapping split by munmap and mprotect */
    LT_EQ(MUNMAP(A + 0x21000, 0x1000), 0);
    LT_EQ(T->mm->vt.n >= 2, 1);
    LT_EQ(H(A + 0x20000)[0], 0xa0);
    LT_EQ(H(A + 0x22000)[0], 0xa2);
    uint8_t v;
    LT_ERRNO(lkd_user_read(A + 0x21000, &v, 1), LX_EFAULT);
    LT_EQ(MPROTECT(A + 0x22000, 0x1000, LX_PROT_READ), 0);
    uint8_t ro = 1;
    LT_ERRNO(lkd_user_write(A + 0x22000, &ro, 1), LX_EFAULT);
    LT_EQ(lkd_user_write(A + 0x20000, &ro, 1), 0);           /* the first page is still writable */
    LT_EQ(H(A + 0x20000)[0], 1);
    ro = 0xa0;
    lkd_user_write(A + 0x20000, &ro, 1);
    lkd_fdobj_put(o);
    down();
}

LT_TEST(munmap_gives_back_private_memory)
{
    up(3);
    lkd_fdobj *o = mk_obj(0x4000, "/memfd:giveback");
    LT_EQ(SMAP(A, 0x1000, RW, SHF, o, 0), A);
    LT_EQ(sc(LKD_NR_mmap, A + 0x1000, 0x3000, LX_PROT_NONE, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A + 0x1000);
    LT_EQ(MUNMAP(A, 0x1000), 0);
    LT_EQ(host_readable(A), 0);                              /* only PROT_NONE siblings are left */
    /* the object must not stay visible behind them: they are fresh private zero memory now */
    LT_EQ(MPROTECT(A + 0x1000, 0x3000, RW), 0);
    LT_EQ(H(A + 0x1000)[0], 0);                              /* not the 0xa1 of the object's page 1 */
    LT_EQ(H(A + 0x2000)[0], 0);
    H(A + 0x1000)[0] = 0x33;
    uint8_t fb;
    pread(o->host_fd, &fb, 1, 0x1000);
    LT_EQ(fb, 0xa1);                                          /* and writes there never reach the object */
    /* the whole range can be mapped privately again */
    LT_EQ(sc(LKD_NR_mmap, A, 0x4000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A);
    LT_EQ(H(A)[0], 0);
    lkd_fdobj_put(o);
    down();
}

static void write_through_second_mapping(void *arg)
{
    uint64_t w = *(uint64_t *)arg;
    H(w)[0x40] = 0xee;       /* in the child, through the inherited writable shared mapping */
    _exit(0);
}

LT_TEST(fork_keeps_shared_mapping)
{
    up(4);
    lkd_fdobj *o = mk_obj(0x1000, "/memfd:fork");
    LT_EQ(SMAP(A, 0x1000, LX_PROT_READ, SHF, o, 0), A);
    uint64_t w = SMAP(A + 0x100000, 0x1000, RW, SHF, o, 0);
    LT_EQ(w, A + 0x100000);
    LT_EQ(sc(LKD_NR_mmap, A + 0x1000, 0x1000, LX_PROT_NONE, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A + 0x1000);
    fflush(NULL);
    pid_t p = lkd_fork_host(T, 0);
    if (p == 0) {
        alarm(10);
        int bad = 0;
        struct lkd_vma_info vi;
        if (lkd_mm_find(T->mm, A, 0, &vi) || (vi.flags & LX_MAP_TYPE) != LX_MAP_SHARED) bad |= 1;
        if (H(A)[0] != 0xa0) bad |= 2;
        H(w)[0x40] = 0xee;                                   /* the child writes the shared object */
        if (H(A)[0x40] != 0xee) bad |= 4;
        if (sc(LKD_NR_mprotect, A + 0x1000, 0x1000, LX_PROT_READ, 0, 0, 0) != -LX_ENOMEM) bad |= 8; /* rule kept */
        _exit(bad);
    }
    int st = -1;
    for (int i = 0; i < 1500 && waitpid(p, &st, WNOHANG) != p; i++) usleep(10000);
    LT_CHECK(WIFEXITED(st));
    LT_EQ(WEXITSTATUS(st), 0);
    LT_EQ(H(A)[0x40], 0xee);                                  /* the parent sees the child's write */
    H(w)[0x41] = 0xef;                                        /* and the child would see the parent's */
    LT_EQ(H(A)[0x41], 0xef);
    (void)write_through_second_mapping;
    lkd_fdobj_put(o);
    down();
}

LT_TEST(anonymous_shared_partial_page)
{
    up(5);
    /* MAP_SHARED|MAP_ANONYMOUS of 4 KiB at a 4 KiB (not 16 KiB) aligned fixed address */
    int64_t s = sc(LKD_NR_mmap, A + 0x1000, 0x1000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED, (uint64_t)-1, 0);
    LT_EQ(s, A + 0x1000);
    H(s)[0] = 9;
    LT_ERRNO(sc(LKD_NR_mmap, A + 0x2000, 0x1000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), LX_EINVAL);
    LT_EQ(sc(LKD_NR_mmap, A + 0x2000, 0x1000, LX_PROT_NONE, PA | LX_MAP_FIXED, (uint64_t)-1, 0), A + 0x2000);
    LT_ERRNO(MPROTECT(A + 0x2000, 0x1000, RW), LX_ENOMEM);
    fflush(NULL);
    pid_t p = fork();
    if (p == 0) { H(s)[1] = 7; _exit(0); }
    int st;
    waitpid(p, &st, 0);
    LT_EQ(H(s)[1], 7);                                        /* shared with the (plain host) child */
    LT_EQ(MUNMAP(s, 0x1000), 0);
    down();
}

int main(void)
{
    LT_RUN(wine_kuser_shared_data);
    LT_RUN(siblings_rules);
    LT_RUN(sizes_and_offsets);
    LT_RUN(munmap_gives_back_private_memory);
    LT_RUN(fork_keeps_shared_mapping);
    LT_RUN(anonymous_shared_partial_page);
    return lt_finish();
}
