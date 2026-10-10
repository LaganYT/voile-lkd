/* test_mm_map.c: mmap, munmap, mprotect on real host memory (identity window, lkd_guest_base == 0).
 * Linux references: mmap(2), munmap(2), mprotect(2) man pages and mm/mmap.c, mm/mprotect.c of v6.12.
 * Host access is probed with write(2)/read(2) on a pipe: the host kernel answers EFAULT for a page the
 * process may not read or write, which is the host page protection itself. */
#include <signal.h>
#include <sys/mman.h>

#include "ltest.h"
#include "fake_engine.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

static lkd_task *T;

static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define MMAP(a, l, p, f, fd, o) sc(LKD_NR_mmap, (a), (l), (p), (f), (uint64_t)(int64_t)(fd), (o))
#define MUNMAP(a, l) sc(LKD_NR_munmap, (a), (l), 0, 0, 0, 0)
#define MPROTECT(a, l, p) sc(LKD_NR_mprotect, (a), (l), (p), 0, 0, 0)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)

static void up(void)
{
    if (!T) { fake_engine_init(); T = fake_make_task(); }
    T->mm = lkd_mm_create();
    fake_engine_reset();
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }

static int host_readable(uint64_t g)
{
    int p[2];
    if (pipe(p)) return -1;
    int r = write(p[1], lkd_g2h(g), 1) == 1;
    close(p[0]); close(p[1]);
    return r;
}

static int host_writable(uint64_t g)
{
    if (!host_readable(g)) return 0;
    char c = *(volatile char *)lkd_g2h(g);
    int p[2];
    if (pipe(p)) return -1;
    if (write(p[1], &c, 1) != 1) return -1;
    int r = read(p[0], lkd_g2h(g), 1) == 1;
    close(p[0]); close(p[1]);
    return r;
}

static size_t nvma(void) { return T->mm->vt.n; }
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }

LT_TEST(anon_private_basic)
{
    up();
    /* mmap(2): "MAP_ANONYMOUS ... its contents are initialized to zero" */
    int64_t a = MMAP(0, 8192, RW, PA, -1, 0);
    LT_CHECK(a > 0);
    LT_EQ(a & 0xfff, 0);
    LT_CHECK((uint64_t)a >= lkd_mm_window_guest_lo() && (uint64_t)a + 8192 <= lkd_mm_window_guest_hi());
    LT_EQ(H(a)[0], 0);
    LT_EQ(H(a)[8191], 0);
    H(a)[100] = 0x5a;
    LT_EQ(H(a)[100], 0x5a);
    uint32_t w = 0xdeadbeef, rd = 0;
    LT_EQ(lkd_user_write(a + 8, &w, 4), 0);
    LT_EQ(lkd_user_read(a + 8, &rd, 4), 0);
    LT_EQ(rd, 0xdeadbeef);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a + 4096, 0, &vi), 0);
    LT_EQ(vi.start, a);
    LT_EQ(vi.end, a + 8192);
    LT_EQ(vi.prot, RW);
    LT_ERRNO(lkd_mm_find(T->mm, a + 8192 + 0x100000, 0, &vi), LX_ENOENT);
    /* top-down: the next mapping goes below the previous one (arch_get_unmapped_area_topdown) */
    int64_t b = MMAP(0, 4096, RW, PA, -1, 0);
    LT_CHECK(b > 0 && b + 4096 <= a);
    down();
}

LT_TEST(mmap_argument_errors)
{
    up();
    LT_ERRNO(MMAP(0, 0, RW, PA, -1, 0), LX_EINVAL);                       /* len 0 */
    LT_ERRNO(MMAP(0, 4096, RW, PA, -1, 0x800), LX_EINVAL);                /* offset not page aligned */
    LT_ERRNO(MMAP(0, 4096, RW, LX_MAP_ANONYMOUS, -1, 0), LX_EINVAL);      /* neither SHARED nor PRIVATE */
    LT_ERRNO(MMAP(0, 4096, RW, LX_MAP_SHARED_VALIDATE | LX_MAP_ANONYMOUS, -1, 0), LX_EINVAL); /* do_mmap anon switch */
    LT_ERRNO(MMAP(0, 4096, RW, LX_MAP_PRIVATE, 977, 0), LX_EBADF);        /* not an open fd */
    LT_ERRNO(MMAP(0, ~0ull - 100, RW, PA, -1, 0), LX_ENOMEM);             /* PAGE_ALIGN overflows to 0 */
    LT_ERRNO(MMAP(0, 1ull << 45, RW, PA, -1, 0), LX_ENOMEM);              /* larger than the address space */
    LT_ERRNO(MMAP(0, 4096, RW, PA | LX_MAP_HUGETLB, -1, 0), LX_ENOMEM);   /* no huge page pool */
    LT_ERRNO(MMAP(0, 4096, RW, PA | LX_MAP_HUGETLB | (25u << 26), -1, 0), LX_EINVAL); /* unknown huge size */
    LT_ERRNO(MMAP(0, 4096, RW, PA | LX_MAP_GROWSDOWN, -1, 0), LX_EINVAL); /* documented LKD limit */
    uint64_t lo = lkd_mm_window_guest_lo();
    LT_ERRNO(MMAP(lo + 0x1000, 4096, RW, PA | LX_MAP_FIXED, -1, 0), LX_EPERM); /* below vm.mmap_min_addr */
    LT_ERRNO(MMAP(lo + 0x20800, 4096, RW, PA | LX_MAP_FIXED, -1, 0), LX_EINVAL); /* fixed, not aligned */
    LT_EQ(nvma(), 0);
    down();
}

LT_TEST(fixed_replaces_and_merges)
{
    up();
    int64_t a = MMAP(0, 3 * 4096, RW, PA, -1, 0);
    memset(H(a), 0x11, 3 * 4096);
    /* mmap(2) MAP_FIXED: "If the memory region ... overlaps pages of any existing mapping(s), then the
     * overlapped part of the existing mapping(s) will be discarded." The new page reads zeros. */
    int64_t r = MMAP(a + 4096, 4096, RW, PA | LX_MAP_FIXED, -1, 0);
    LT_EQ(r, a + 4096);
    LT_EQ(H(a)[4095], 0x11);
    LT_EQ(H(a + 4096)[0], 0);
    LT_EQ(H(a + 4096)[4095], 0);
    LT_EQ(H(a + 8192)[0], 0x11);
    /* compatible anonymous neighbours merge into one VMA (vma_merge) */
    LT_EQ(nvma(), 1);
    /* a different protection splits */
    r = MMAP(a + 4096, 4096, LX_PROT_READ, PA | LX_MAP_FIXED, -1, 0);
    LT_EQ(r, a + 4096);
    LT_EQ(nvma(), 3);
    /* MAP_FIXED_NOREPLACE: EEXIST when the range is in use, success on a free range */
    LT_ERRNO(MMAP(a, 4096, RW, PA | LX_MAP_FIXED_NOREPLACE, -1, 0), LX_EEXIST);
    LT_ERRNO(MMAP(a - 4096, 8192, RW, PA | LX_MAP_FIXED_NOREPLACE, -1, 0), LX_EEXIST);
    LT_EQ(MMAP(a - 0x10000, 4096, RW, PA | LX_MAP_FIXED_NOREPLACE, -1, 0), a - 0x10000);
    down();
}

LT_TEST(hint_semantics)
{
    up();
    uint64_t lo = lkd_mm_window_guest_lo();
    uint64_t hint = lo + 0x30000000;
    LT_EQ(MMAP(hint, 4096, RW, PA, -1, 0), hint);                          /* free hint is honored */
    int64_t b = MMAP(hint, 4096, RW, PA, -1, 0);                             /* busy: somewhere else */
    LT_CHECK(b > 0 && (uint64_t)b != hint);
    LT_EQ(MMAP(hint + 0x10123, 4096, RW, PA, -1, 0), hint + 0x10000);       /* hint rounded down */
    LT_EQ(MMAP(lo + 0x100, 4096, RW, PA, -1, 0), lo + 0x10000);             /* round_hint_to_min */
    down();
}

LT_TEST(map32bit_and_thp_alignment)
{
    up();
    uint64_t lo = lkd_mm_window_guest_lo();
    /* MAP_32BIT: "placed in the first two gigabytes", bottom-up from 0x40000000 on x86-64 */
    int64_t a = MMAP(0, 8192, RW, PA | LX_MAP_32BIT, -1, 0);
    LT_EQ(a, lo + 0x40000000);
    int64_t b = MMAP(0, 8192, RW, PA | LX_MAP_32BIT, -1, 0);
    LT_EQ(b, lo + 0x40002000);
    /* anonymous mappings whose length is a multiple of 2 MiB are THP aligned (6.12 __get_unmapped_area) */
    int64_t c = MMAP(0, 4ull << 20, RW, PA, -1, 0);
    LT_EQ(c & ((2 << 20) - 1), 0);
    down();
}

LT_TEST(munmap_partial_and_host_pages)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA, -1, 0); /* two 16 KiB host pages if aligned */
    uint64_t hp = HP_UP((uint64_t)a);
    memset(H(a), 0x22, 0x8000);
    /* unmap one 4 KiB guest page inside a 16 KiB host page */
    LT_EQ(MUNMAP(hp + 4096, 4096), 0);
    LT_EQ(nvma(), 2);
    uint8_t x;
    LT_ERRNO(lkd_user_read(hp + 4096, &x, 1), LX_EFAULT);   /* the VMA table forbids it */
    LT_EQ(lkd_user_read(hp, &x, 1), 0);
    LT_EQ(x, 0x22);
    LT_EQ(host_readable(hp + 4096), 1);                      /* the host page persists (documented) */
    LT_EQ(H(hp + 4096)[0], 0);                               /* and the unmapped sub-page was zeroed */
    LT_EQ(H(hp + 8192)[0], 0x22);
    /* a later mapping of that sub-page reads zeros */
    LT_EQ(MMAP(hp + 4096, 4096, RW, PA | LX_MAP_FIXED, -1, 0), hp + 4096);
    LT_EQ(H(hp + 4096)[17], 0);
    /* unmapping a whole host page gives it back to the reservation: no host access at all */
    LT_EQ(MUNMAP(hp, 0x4000), 0);
    LT_EQ(host_readable(hp), 0);
    LT_EQ(host_readable(hp + 0x3fff), 0);
    LT_ERRNO(MUNMAP(a + 1, 4096), LX_EINVAL);                /* munmap(2): addr must be page aligned */
    LT_ERRNO(MUNMAP(a, 0), LX_EINVAL);                       /* len 0 */
    LT_EQ(MUNMAP(lkd_mm_window_guest_lo() + 0x50000000, 4096), 0); /* nothing mapped there: success */
    LT_EQ(MUNMAP((uint64_t)a, 0x100000), 0);                 /* range larger than the mapping */
    LT_ERRNO(MUNMAP((uint64_t)a, 0x10000000), LX_EINVAL);    /* len > TASK_SIZE - start (do_vmi_munmap) */
    LT_EQ(nvma(), 0);
    down();
}

LT_TEST(mprotect_semantics)
{
    up();
    int64_t a = MMAP(0, 0x10000, RW, PA, -1, 0);
    uint64_t hp = HP_UP((uint64_t)a);
    memset(H(a), 0x33, 0x10000);
    /* read-only: host write refused, kernel-side write EFAULT */
    LT_EQ(MPROTECT(hp, 0x4000, LX_PROT_READ), 0);
    LT_EQ(host_writable(hp), 0);
    LT_EQ(host_readable(hp), 1);
    uint8_t v = 1;
    LT_ERRNO(lkd_user_write(hp, &v, 1), LX_EFAULT);
    LT_EQ(H(hp)[5], 0x33);
    /* PROT_NONE on a whole host page: no host access */
    LT_EQ(MPROTECT(hp, 0x4000, LX_PROT_NONE), 0);
    LT_EQ(host_readable(hp), 0);
    /* PROT_NONE on one sub-page of an accessible host page: the host page keeps the union (rw), the
     * VMA table has the exact view (documented limit for guest code, enforced for kernel copies) */
    LT_EQ(MPROTECT(hp + 0x4000, 4096, LX_PROT_NONE), 0);
    LT_EQ(host_writable(hp + 0x4000), 1);
    LT_ERRNO(lkd_user_read(hp + 0x4000, &v, 1), LX_EFAULT);
    LT_EQ(lkd_user_read(hp + 0x5000, &v, 1), 0);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, hp + 0x4000, 0, &vi), 0);
    LT_EQ(vi.start, hp);          /* merged with the PROT_NONE VMA before it */
    LT_EQ(vi.end, hp + 0x5000);
    LT_EQ(vi.prot, 0);
    /* protection restored: VMAs merge again, content intact */
    LT_EQ(MPROTECT((uint64_t)a, 0x10000, RW), 0);
    LT_EQ(nvma(), 1);
    LT_EQ(H(hp)[9], 0x33);
    LT_EQ(host_writable(hp), 1);
    down();
}

LT_TEST(mprotect_errors)
{
    up();
    int64_t a = MMAP(0, 0x3000, RW, PA, -1, 0);
    LT_ERRNO(MPROTECT(a + 1, 4096, LX_PROT_READ), LX_EINVAL);              /* start not aligned */
    LT_ERRNO(MPROTECT(a, 4096, 0x10), LX_EINVAL);                           /* unknown bit (arch_validate_prot) */
    LT_ERRNO(MPROTECT(a, 4096, LX_PROT_READ | LX_PROT_GROWSDOWN | LX_PROT_GROWSUP), LX_EINVAL);
    LT_ERRNO(MPROTECT(a, 4096, LX_PROT_READ | LX_PROT_GROWSDOWN), LX_EINVAL); /* VMA is not VM_GROWSDOWN */
    LT_EQ(MPROTECT(a, 0, LX_PROT_READ), 0);                                  /* len 0 */
    LT_ERRNO(MPROTECT(a - 0x10000, 4096, LX_PROT_READ), LX_ENOMEM);          /* unmapped */
    LT_EQ(MPROTECT(a, 4096, LX_PROT_READ | LX_PROT_SEM), 0);                 /* PROT_SEM accepted */
    /* a hole in the middle: ENOMEM, but the part before the hole was changed (do_mprotect_pkey) */
    LT_EQ(MUNMAP(a + 0x1000, 0x1000), 0);
    LT_ERRNO(MPROTECT(a, 0x3000, LX_PROT_NONE), LX_ENOMEM);
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, a, 0, &vi), 0);
    LT_EQ(vi.prot, LX_PROT_NONE);
    LT_EQ(lkd_mm_find(T->mm, a + 0x2000, 0, &vi), 0);
    LT_EQ(vi.prot, RW);
    down();
}

static void touch_child(void *arg)
{
    volatile uint8_t *p = arg;
    (void)*p; /* must die */
    _exit(0);
}

LT_TEST(real_fault_on_prot_none)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA, -1, 0);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(MPROTECT(hp, 0x4000, LX_PROT_NONE), 0);
    int st = lt_run_child(touch_child, H(hp), 10);
    LT_MSG(st == 128 + SIGBUS || st == 128 + SIGSEGV, "child status %d", st);
    LT_EQ(MPROTECT(hp, 0x4000, LX_PROT_READ), 0);
    st = lt_run_child(touch_child, H(hp), 10);
    LT_EQ(st, 0);
    down();
}

LT_TEST(jit_invalidation)
{
    up();
    /* lkd.h invalidate_code: mmap over code, mprotect and munmap of code drop translations */
    int64_t a = MMAP(0, 0x2000, LX_PROT_READ | LX_PROT_EXEC, PA, -1, 0);
    LT_CHECK(fake_invalidate_count >= 1);
    int n = fake_invalidate_count;
    LT_EQ(MPROTECT(a, 0x1000, RW), 0);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], a);
    LT_EQ(fake_inval_len[n % 16], 0x1000);
    n = fake_invalidate_count;
    LT_EQ(MUNMAP(a + 0x1000, 0x1000), 0);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], a + 0x1000);
    n = fake_invalidate_count;
    int64_t b = MMAP(0, 0x1000, RW, PA, -1, 0); /* no code: no invalidation */
    LT_CHECK(b > 0);
    LT_EQ(fake_invalidate_count, n);
    down();
}

static void write_child(void *arg)
{
    uint8_t *p = arg;
    p[0] = 0x77;
    p[1] = 0x78;
    _exit(0);
}

LT_TEST(shared_anonymous)
{
    up();
    /* MAP_SHARED|MAP_ANONYMOUS: "shared with ... processes that map this same region ... after fork" */
    int64_t s = MMAP(0, 4096, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
    LT_CHECK(s > 0);
    LT_EQ(s & 0x3fff, 0); /* host-shared mappings start on a 16 KiB host page */
    int64_t p = MMAP(0, 4096, RW, PA, -1, 0);
    LT_CHECK(HP_DOWN((uint64_t)p) != HP_DOWN((uint64_t)s)); /* never in the same host page */
    LT_EQ(lt_run_child(write_child, H(s), 10), 0);
    LT_EQ(H(s)[0], 0x77);
    LT_EQ(H(s)[1], 0x78);
    LT_EQ(lt_run_child(write_child, H(p), 10), 0);
    LT_EQ(H(p)[0], 0);   /* private: copy-on-write */
    struct lkd_vma_info vi;
    LT_EQ(lkd_mm_find(T->mm, s, 0, &vi), 0);
    LT_EQ(vi.flags & LX_MAP_TYPE, LX_MAP_SHARED);
    LT_STREQ(vi.name, "/dev/zero");
    LT_EQ(vi.deleted, 1);
    LT_EQ(vi.dev_minor, 1);
    /* a private mapping may not share a host page with it (documented host page limit) */
    LT_ERRNO(MMAP(s + 4096, 4096, RW, PA | LX_MAP_FIXED, -1, 0), LX_EINVAL);
    uint64_t h0 = lkd_mm_window_guest_lo() + 0x20000000;
    LT_EQ(MMAP(h0 + 4096, 4096, RW, PA | LX_MAP_FIXED, -1, 0), h0 + 4096);
    LT_ERRNO(MMAP(h0, 4096, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0), LX_EINVAL);
    /* anonymous shared memory has no offset: any 4 KiB address works, the host page is shared whole */
    LT_EQ(MMAP(h0 + 0x5000, 4096, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0), h0 + 0x5000);
    LT_EQ(MMAP(h0 + 0x8000, 4096, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0), h0 + 0x8000);
    down();
}

LT_TEST(check_range_rules)
{
    up();
    int64_t a = MMAP(0, 0x3000, LX_PROT_READ, PA, -1, 0);
    uint8_t buf[16];
    LT_EQ(lkd_mm_check_range(a, 0x3000, 0), 0);
    LT_ERRNO(lkd_mm_check_range(a, 0x3000, 1), LX_EFAULT);       /* read-only */
    LT_ERRNO(lkd_mm_check_range(a, 0x3001, 0), LX_EFAULT);       /* runs past the end */
    LT_ERRNO(lkd_user_read(a - 4096, buf, 16), LX_EFAULT);
    LT_EQ(MPROTECT(a, 0x1000, LX_PROT_WRITE), 0);                /* x86: write implies read */
    LT_EQ(lkd_mm_check_range(a, 16, 0), 0);
    LT_EQ(MPROTECT(a, 0x1000, LX_PROT_EXEC), 0);                 /* x86: exec implies read */
    LT_EQ(lkd_mm_check_range(a, 16, 0), 0);
    /* host memory outside the identity window is not managed by mm and stays permitted (tests) */
    LT_EQ(lkd_mm_check_range((uint64_t)(uintptr_t)buf, sizeof buf, 1), 0);
    /* a task without address space: permissive, the copy's fault recovery is the safety net */
    lkd_mm *keep = T->mm;
    T->mm = NULL;
    LT_EQ(lkd_mm_check_range(a - 4096, 16, 1), 0);
    T->mm = keep;
    down();
}

LT_TEST(one_address_space_per_process)
{
    up();
    errno = 0;
    LT_CHECK(lkd_mm_create() == NULL);
    LT_EQ(errno, EBUSY);
    lkd_mm *mm = T->mm;
    LT_CHECK(lkd_mm_clone(mm, LX_CLONE_VM) == mm);
    LT_EQ(atomic_load(&mm->refs), 2);
    lkd_mm_put(mm);
    LT_NE(lkd_mm_identity(mm), 0);
    down();
}

/* lkd_mm_find_nowait: the lookup of the fatal fault report, which must not wait for a VMA table the faulting thread
 * may hold. Same answer as lkd_mm_find, -EBUSY while the table is being changed. */
LT_TEST(find_nowait)
{
    up();
    int64_t a = MMAP(0, 8192, RW, PA, -1, 0);
    LT_CHECK(a > 0);
    struct lkd_vma_info vi, vn;
    LT_EQ(lkd_mm_find(T->mm, a + 4096, 0, &vi), 0);
    LT_EQ(lkd_mm_find_nowait(T->mm, a + 4096, &vn), 0);
    LT_EQ(vn.start, vi.start);
    LT_EQ(vn.end, vi.end);
    LT_EQ(vn.prot, vi.prot);
    LT_ERRNO(lkd_mm_find_nowait(T->mm, a + 8192 + 0x100000, &vn), LX_ENOENT);
    LT_ERRNO(lkd_mm_find_nowait(NULL, a, &vn), LX_ENOENT);
    pthread_rwlock_wrlock(&T->mm->vlock); /* a writer holds the table */
    LT_ERRNO(lkd_mm_find_nowait(T->mm, a + 4096, &vn), LX_EBUSY);
    pthread_rwlock_unlock(&T->mm->vlock);
    LT_EQ(lkd_mm_find_nowait(T->mm, a + 4096, &vn), 0);
    down();
}

int main(void)
{
    LT_RUN(anon_private_basic);
    LT_RUN(mmap_argument_errors);
    LT_RUN(fixed_replaces_and_merges);
    LT_RUN(hint_semantics);
    LT_RUN(map32bit_and_thp_alignment);
    LT_RUN(munmap_partial_and_host_pages);
    LT_RUN(mprotect_semantics);
    LT_RUN(mprotect_errors);
    LT_RUN(real_fault_on_prot_none);
    LT_RUN(jit_invalidation);
    LT_RUN(shared_anonymous);
    LT_RUN(check_range_rules);
    LT_RUN(one_address_space_per_process);
    LT_RUN(find_nowait);
    return lt_finish();
}
