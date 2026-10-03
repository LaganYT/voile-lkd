/* test_mm_file.c: file mappings with a real host file, through lkd_mm_map (the engine's ELF loader
 * path) and through the mmap syscall. Linux references: mmap(2) (MAP_PRIVATE copy-on-write,
 * MAP_SHARED write-through, EACCES rules), mm/mmap.c do_mmap v6.12, msync(2), proc(5) maps format.
 * The 4 KiB-aligned segment pattern is the one of glibc ld.so loading Debian x86-64 libraries. */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

static lkd_task *T;
static char path[512];
static int hostfd = -1;
#define FSIZE (0x10000 + 0x800) /* 64 KiB + 2 KiB: the last page is partial */
#define RW (LX_PROT_READ | LX_PROT_WRITE)

static uint8_t pat(uint64_t off) { return (uint8_t)((off * 7 + (off >> 12) * 13) & 0xff); }

static int t_mmap_fd(lkd_fdobj *o) { return o->host_fd; }
static int t_describe(lkd_fdobj *o, char *buf, size_t cap) { return snprintf(buf, cap, "%s", o->gpath); }
static const lkd_fd_ops file_ops = { .name = "file", .mmap_fd = t_mmap_fd, .describe = t_describe };
static const lkd_fd_ops nommap_ops = { .name = "pipe" };

static lkd_fdobj *open_obj(int oflags, uint32_t lxacc)
{
    int fd = open(path, oflags);
    if (fd < 0) return NULL;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_FILE, &file_ops, fd, lxacc, NULL);
    o->gpath = strdup("/usr/lib/x86_64-linux-gnu/libtest.so.1");
    return o;
}

static void up(void)
{
    if (!T) {
        fake_engine_init();
        T = fake_make_task();
        const char *d = getenv("TMPDIR");
        snprintf(path, sizeof path, "%s/lkd_mm_file_XXXXXX", d && *d ? d : "/tmp");
        hostfd = mkstemp(path);
        uint8_t *b = malloc(FSIZE);
        for (uint64_t i = 0; i < FSIZE; i++) b[i] = pat(i);
        write(hostfd, b, FSIZE);
        free(b);
    }
    T->mm = lkd_mm_create();
    fake_engine_reset();
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }

static int content_ok(uint64_t g, uint64_t off, uint64_t n)
{
    for (uint64_t i = 0; i < n; i++) {
        uint8_t want = off + i < FSIZE ? pat(off + i) : 0;
        if (H(g)[i] != want) return 0;
    }
    return 1;
}

LT_TEST(private_direct_and_identity)
{
    up();
    lkd_fdobj *o = open_obj(O_RDONLY, LX_O_RDONLY);
    int64_t a = lkd_mm_map(T->mm, 0, FSIZE, LX_PROT_READ, LX_MAP_PRIVATE, o, 0, NULL);
    LT_CHECK(a > 0);
    LT_EQ(a & 0x3fff, 0);            /* offset 0: a 16 KiB aligned address so the file maps directly */
    LT_CHECK(content_ok(a, 0, FSIZE));
    LT_EQ(H(a)[FSIZE], 0);           /* "the remaining bytes in the partial page ... are zeroed" mmap(2) */
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_EQ(vi.end - vi.start, 0x11000);
    LT_STREQ(vi.name, "/usr/lib/x86_64-linux-gnu/libtest.so.1");
    struct stat st;
    fstat(o->host_fd, &st);
    LT_EQ(vi.ino, st.st_ino);
    uint64_t ld = lkd_dev_to_linux(st.st_dev);
    LT_EQ(vi.dev_major, (ld & 0xfff00) >> 8);
    LT_EQ(vi.dev_minor, (ld & 0xff) | ((ld >> 12) & 0xfff00));
    LT_EQ(vi.file_offset, 0);
    lkd_fdobj_put(o);
    down();
}

LT_TEST(private_offset_not_host_aligned)
{
    up();
    lkd_fdobj *o = open_obj(O_RDONLY, LX_O_RDONLY);
    int64_t a = lkd_mm_map(T->mm, 0, 0x9000, LX_PROT_READ, LX_MAP_PRIVATE, o, 0x1000, NULL);
    LT_CHECK(a > 0);
    LT_EQ(a & 0x3fff, 0x1000);       /* congruent with the offset modulo the host page */
    LT_CHECK(content_ok(a, 0x1000, 0x9000));
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_EQ(vi.file_offset, 0x1000);
    /* a fixed mapping at an address that is not congruent: copied, same content */
    uint64_t f = lkd_mm_window_guest_lo() + 0x10000000;
    int64_t b = lkd_mm_map(T->mm, f + 0x2000, 0x5000, LX_PROT_READ, LX_MAP_PRIVATE | LX_MAP_FIXED, o, 0x3000, NULL);
    LT_EQ(b, f + 0x2000);
    LT_CHECK(content_ok(b, 0x3000, 0x5000));
    lkd_fdobj_put(o);
    down();
}

/* glibc ld.so (elf/dl-map-segments.h): map the whole span with the first segment, then MAP_FIXED each
 * following segment at its 4 KiB aligned address and offset, then zero-fill .bss. */
LT_TEST(ld_so_segment_pattern)
{
    up();
    lkd_fdobj *o = open_obj(O_RDONLY, LX_O_RDONLY);
    int64_t base = lkd_mm_map(T->mm, 0, 0x16000, LX_PROT_READ, LX_MAP_PRIVATE | LX_MAP_DENYWRITE, o, 0, NULL);
    LT_CHECK(base > 0);
    int64_t text = lkd_mm_map(T->mm, base + 0x1000, 0x6000, LX_PROT_READ | LX_PROT_EXEC,
                              LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_DENYWRITE, o, 0x1000, NULL);
    LT_EQ(text, base + 0x1000);
    int64_t data = lkd_mm_map(T->mm, base + 0x9000, 0x3000, RW, LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_DENYWRITE, o, 0x8000, NULL);
    LT_EQ(data, base + 0x9000);
    int64_t bss = lkd_mm_map(T->mm, base + 0xc000, 0x2000, RW, LX_MAP_PRIVATE | LX_MAP_FIXED | LX_MAP_ANONYMOUS, NULL, 0, NULL);
    LT_EQ(bss, base + 0xc000);
    LT_EQ(lkd_mm_protect(T->mm, base + 0x7000, 0x2000, LX_PROT_NONE), 0); /* the gap between segments */
    LT_CHECK(content_ok(base, 0, 0x1000));
    LT_CHECK(content_ok(base + 0x1000, 0x1000, 0x6000));
    LT_CHECK(content_ok(base + 0x9000, 0x8000, 0x3000));
    LT_EQ(H(base + 0xc000)[0], 0);
    LT_EQ(H(base + 0xdfff)[0], 0);
    /* MAP_PRIVATE writes are copy-on-write: the file keeps its content */
    H(base + 0x9000)[5] = 0xee;
    uint8_t fb;
    pread(hostfd, &fb, 1, 0x8005);
    LT_EQ(fb, pat(0x8005));
    LT_EQ(H(base + 0x9000)[5], 0xee);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, base + 0x1000, 0, &vi), 0);
    LT_EQ(vi.prot, LX_PROT_READ | LX_PROT_EXEC);
    LT_EQ(vi.file_offset, 0x1000);
    LT_EQ(vi.end, base + 0x7000);
    LT_EQ(lkd_mm_find(T->mm, base + 0x9000, 0, &vi), 0);
    LT_EQ(vi.file_offset, 0x8000);
    LT_EQ(lkd_mm_find(T->mm, base + 0x7000, 0, &vi), 0);
    LT_EQ(vi.prot, 0);
    LT_EQ(vi.file_offset, 0x7000);  /* the PROT_NONE gap is still the first mapping, at its offset */
    LT_EQ(lkd_mm_find(T->mm, base + 0xc000, 0, &vi), 0);
    LT_EQ(vi.name[0], 0);           /* .bss is anonymous */
    /* code mapped from a file is announced to the JIT */
    LT_CHECK(fake_invalidate_count >= 1);
    LT_EQ(T->mm->vt.n, 6);
    lkd_fdobj_put(o);
    down();
}

LT_TEST(shared_file_mapping)
{
    up();
    lkd_fdobj *o = open_obj(O_RDWR, LX_O_RDWR);
    int64_t a = lkd_mm_map(T->mm, 0, 0x8000, RW, LX_MAP_SHARED, o, 0x4000, NULL);
    LT_CHECK(a > 0);
    LT_EQ(a & 0x3fff, 0);
    LT_CHECK(content_ok(a, 0x4000, 0x8000));
    H(a)[3] = 0xab; /* MAP_SHARED: "Updates to the mapping are visible to other processes ... and are carried
                     * through to the underlying file" */
    lkd_sysreq rq = { LKD_NR_msync, { (uint64_t)a, 0x1000, LX_MS_SYNC } };
    LT_EQ(lkd_dispatch(T, &rq), 0);
    uint8_t fb;
    pread(hostfd, &fb, 1, 0x4003);
    LT_EQ(fb, 0xab);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_EQ(vi.flags & LX_MAP_TYPE, LX_MAP_SHARED);
    LT_EQ(lkd_mm_has_writable_shared_mapping(T->mm, o), 1);
    /* restore the byte for the other cases */
    H(a)[3] = pat(0x4003);
    LT_EQ(lkd_dispatch(T, &rq), 0);
    /* a shared mapping at a 4 KiB file offset is placed at an address with the same position in its host
     * page (docs/status/mm.md, partial host pages): it maps the right bytes */
    int64_t q = lkd_mm_map(T->mm, 0, 0x1000, LX_PROT_READ, LX_MAP_SHARED, o, 0x1000, NULL);
    LT_CHECK(q > 0);
    LT_EQ(q & 0x3fff, 0x1000);
    LT_CHECK(content_ok(q, 0x1000, 0x1000));
    /* but a FIXED address that is 16 KiB aligned with that offset cannot be: honest EINVAL */
    uint64_t fx = lkd_mm_window_guest_lo() + 0x14000000;
    LT_ERRNO(lkd_mm_map(T->mm, fx, 0x1000, LX_PROT_READ, LX_MAP_SHARED | LX_MAP_FIXED, o, 0x1000, NULL), LX_EINVAL);
    LT_EQ(lkd_mm_unmap(T->mm, q, 0x1000), 0);
    lkd_fdobj_put(o);
    LT_EQ(lkd_mm_unmap(T->mm, a, 0x8000), 0);
    down();
}

LT_TEST(access_mode_rules)
{
    up();
    lkd_fdobj *ro = open_obj(O_RDONLY, LX_O_RDONLY), *wo = open_obj(O_WRONLY, LX_O_WRONLY);
    /* mmap(2) EACCES: "MAP_SHARED was requested and PROT_WRITE is set, but fd is not open in O_RDWR" */
    LT_ERRNO(lkd_mm_map(T->mm, 0, 4096, RW, LX_MAP_SHARED, ro, 0, NULL), LX_EACCES);
    /* "a file mapping was requested, but fd is not open for reading" */
    LT_ERRNO(lkd_mm_map(T->mm, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, wo, 0, NULL), LX_EACCES);
    /* MAP_PRIVATE with PROT_WRITE on a read-only fd is allowed (copy-on-write) */
    int64_t p = lkd_mm_map(T->mm, 0, 4096, RW, LX_MAP_PRIVATE, ro, 0, NULL);
    LT_CHECK(p > 0);
    /* mprotect(2) EACCES: PROT_WRITE on a MAP_SHARED mapping of a read-only fd */
    int64_t s = lkd_mm_map(T->mm, 0, 4096, LX_PROT_READ, LX_MAP_SHARED, ro, 0, NULL);
    LT_CHECK(s > 0);
    LT_ERRNO(lkd_mm_protect(T->mm, s, 4096, RW), LX_EACCES);
    LT_EQ(lkd_mm_has_writable_shared_mapping(T->mm, ro), 0);
    /* MAP_SHARED_VALIDATE rejects unknown flags with EOPNOTSUPP (MAP_SYNC is unknown here) */
    LT_ERRNO(lkd_mm_map(T->mm, 0, 4096, LX_PROT_READ, LX_MAP_SHARED_VALIDATE | LX_MAP_SYNC, ro, 0, NULL), LX_EOPNOTSUPP);
    /* an object without mmap support: ENODEV */
    lkd_fdobj *np = lkd_fdobj_new(LKD_FD_PIPE, &nommap_ops, -1, LX_O_RDONLY, NULL);
    LT_ERRNO(lkd_mm_map(T->mm, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, np, 0, NULL), LX_ENODEV);
    lkd_fdobj_put(np);
    lkd_fdobj_put(ro);
    lkd_fdobj_put(wo);
    down();
}

LT_TEST(syscall_path_and_deleted)
{
    up();
    T->fdt = lkd_fdtable_new();
    lkd_fdobj *o = open_obj(O_RDONLY, LX_O_RDONLY);
    int fd = lkd_fd_alloc(T, o, 3, 0);
    LT_CHECK(fd >= 3);
    lkd_sysreq rq = { LKD_NR_mmap, { 0, 0x3000, LX_PROT_READ, LX_MAP_PRIVATE, (uint64_t)fd, 0x2000 } };
    int64_t a = lkd_dispatch(T, &rq);
    LT_CHECK(a > 0);
    LT_CHECK(content_ok(a, 0x2000, 0x3000));
    /* the mapping keeps the file alive after close(fd) (mmap(2): "closing the file descriptor does not
     * unmap the region") */
    LT_EQ(lkd_fd_close(T, fd), 0);
    lkd_fdobj_put(o);
    LT_CHECK(content_ok(a, 0x2000, 0x3000));
    /* d_path prints " (deleted)" once the file is unlinked (a separate file, the test file stays) */
    char p2[600];
    snprintf(p2, sizeof p2, "%s.del", path);
    int f2 = open(p2, O_RDWR | O_CREAT | O_TRUNC, 0600);
    write(f2, "x", 1);
    lkd_fdobj *o2 = lkd_fdobj_new(LKD_FD_FILE, &file_ops, f2, LX_O_RDONLY, NULL);
    o2->gpath = strdup("/tmp/victim");
    int64_t b = lkd_mm_map(T->mm, 0, 0x1000, LX_PROT_READ, LX_MAP_PRIVATE, o2, 0, NULL);
    lkd_fdobj_put(o2);
    unlink(p2);
    size_t len;
    char *maps = lkd_mm_render_maps(T->mm, &len);
    LT_CHECK(maps && strstr(maps, " /tmp/victim (deleted)\n"));
    LT_EQ(len, strlen(maps));
    free(maps);
    LT_EQ(lkd_mm_unmap(T->mm, b, 0x1000), 0);
    lkd_fdtable_put(T->fdt);
    T->fdt = NULL;
    down();
}

int main(void)
{
    LT_RUN(private_direct_and_identity);
    LT_RUN(private_offset_not_host_aligned);
    LT_RUN(ld_so_segment_pattern);
    LT_RUN(shared_file_mapping);
    LT_RUN(access_mode_rules);
    LT_RUN(syscall_path_and_deleted);
    unlink(path);
    return lt_finish();
}
