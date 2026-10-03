/* test_mm_window.c: the production guest window (lkd_guest_base != 0), as the engine configures it.
 * Facts used: reports/fexcore-darwin-port.md (2^46 PROT_NONE reservation succeeds, free space near the
 * top, the prototype engine ran with a base of 8 TiB); Wine needs guest 0x10000 and 0x7ffe0000
 * (reports/00-architecture-decision.md point 3); vm.mmap_min_addr = 65536. Guest address g lives at host
 * address lkd_guest_base + g (include/lkd/lkd.h section 1). */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include <fcntl.h>
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"

#define BASE 0x80000000000ull /* 8 TiB */
static lkd_task *T;
static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)

static int host_readable(const void *p)
{
    int fd[2];
    if (pipe(fd)) return -1;
    int r = write(fd[1], p, 1) == 1;
    close(fd[0]); close(fd[1]);
    return r;
}

LT_TEST(production_window)
{
    LT_EQ(lkd_guest_base, BASE);
    LT_EQ(lkd_mm_window_guest_lo(), 0);
    LT_EQ(lkd_mm_window_guest_hi(), 1ull << 46);
    LT_EQ(host_readable((void *)(uintptr_t)(BASE + 0x10000)), 0); /* reserved, inaccessible */
    T = fake_make_task();
    T->mm = lkd_mm_create();
    LT_CHECK(T->mm != NULL);
    /* Wine's preloader ranges: 0x10000.. and USER_SHARED_DATA at 0x7ffe0000 */
    LT_EQ(sc(LKD_NR_mmap, 0x10000, 0x10000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x10000);
    LT_EQ(sc(LKD_NR_mmap, 0x7ffe0000, 0x1000, LX_PROT_READ, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x7ffe0000);
    LT_ERRNO(sc(LKD_NR_mmap, 0xf000, 0x1000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), LX_EPERM); /* below mmap_min_addr */
    uint8_t *h = (uint8_t *)(uintptr_t)(BASE + 0x10000);
    h[5] = 0x3c;
    uint8_t v = 0;
    LT_EQ(lkd_user_read(0x10005, &v, 1), 0);       /* guest pointer, translated by lkd_g2h */
    LT_EQ(v, 0x3c);
    LT_ERRNO(lkd_user_write(0x7ffe0000, &v, 1), LX_EFAULT);
    /* a non-fixed mapping lands top-down below mmap_base = 2^46 - 128 MiB */
    int64_t a = sc(LKD_NR_mmap, 0, 0x2000, RW, PA, (uint64_t)-1, 0);
    LT_CHECK(a > 0 && (uint64_t)a + 0x2000 <= (1ull << 46) - (128ull << 20));
    LT_CHECK((uint64_t)a > (1ull << 45));
    LT_EQ(host_readable((void *)(uintptr_t)(BASE + a)), 1);
    size_t len;
    char *m = lkd_mm_render_maps(T->mm, &len);
    LT_CHECK(m && !strncmp(m, "00010000-00020000 rw-p 00000000 00:00 0 \n7ffe0000-7ffe1000 r--p 00000000 00:00 0 \n", 82));
    free(m);
    /* the cross-process service runs for a production address space */
    char path[200];
    const char *d = getenv("TMPDIR");
    size_t l = d ? strlen(d) : 0;
    snprintf(path, sizeof path, "%s%slkdmm.%d.sock", d && l <= 70 ? d : "/tmp",
             d && l <= 70 && l && d[l - 1] == '/' ? "" : "/", (int)getpid());
    LT_EQ(access(path, F_OK), 0);
    lkd_mm_put(T->mm);
    T->mm = NULL;
    LT_ERRNO(access(path, F_OK) == 0 ? 0 : -LX_ENOENT, LX_ENOENT); /* removed with the address space */
    LT_EQ(host_readable(h), 0);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* docs/requests/task-4.md: exec and exit walked the 2^32 host pages of the window one by one (20 s per
 * execve under the real engine). Every whole-window operation must cost O(VMAs). Wine reserves
 * terabyte ranges PROT_NONE, so a 1 TiB VMA is exercised too. */
LT_TEST(whole_window_operations_are_fast)
{
    LT_EQ(lkd_mm_window_size(), 1ull << 46);
    T->mm = lkd_mm_create();
    double t0 = now_s();
    LT_EQ(sc(LKD_NR_mmap, 0x10000, 0x4000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x10000);
    int64_t big = sc(LKD_NR_mmap, 0x100000000000ull, 1ull << 40, LX_PROT_NONE, PA | LX_MAP_NORESERVE, (uint64_t)-1, 0);
    LT_EQ(big, 0x100000000000ull);
    LT_EQ(sc(LKD_NR_mprotect, big + 0x40000000, 0x10000, RW, 0, 0, 0), 0);
    ((uint8_t *)(uintptr_t)(BASE + big + 0x40000000))[1] = 7;
    int64_t top = sc(LKD_NR_mmap, 0, 0x8000, RW, PA, (uint64_t)-1, 0);
    LT_CHECK(top > 0);
    size_t len;
    char *sm = lkd_mm_render_smaps(T->mm, &len);
    LT_CHECK(sm != NULL);
    free(sm);
    struct lkd_mm_stats st;
    LT_EQ(lkd_mm_stats(T->mm, &st), 0);
    LT_EQ(st.total_vm, 0x4000 + (1ull << 40) + 0x8000);
    LT_CHECK(st.rss_anon >= 0x4000);
    LT_EQ(sc(LKD_NR_madvise, big, 1ull << 40, LX_MADV_DONTNEED, 0, 0, 0), 0);
    LT_EQ(((uint8_t *)(uintptr_t)(BASE + big + 0x40000000))[1], 0);
    LT_EQ(sc(LKD_NR_munmap, 0, 1ull << 46, 0, 0, 0, 0), 0);      /* munmap of the whole address space */
    LT_EQ(T->mm->vt.n, 0);
    LT_EQ(sc(LKD_NR_mmap, 0x7ffe0000, 0x1000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x7ffe0000);
    lkd_mm_exec_commit(T);                       /* reset for exec: whole window */
    LT_EQ(T->mm->vt.n, 0);
    LT_EQ(sc(LKD_NR_mmap, 0x10000, 0x4000, RW, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x10000);
    lkd_mm_task_exit(T);                         /* last reference: whole-window teardown */
    LT_CHECK(T->mm == NULL);
    double dt = now_s() - t0;
    LT_MSG(dt < 2.0, "whole-window operations took %.2f s", dt);
}

static int w_mmap_fd(lkd_fdobj *o) { return o->host_fd; }
static const lkd_fd_ops w_ops = { .name = "memfd", .mmap_fd = w_mmap_fd };

/* Wine, exact addresses (docs/status/mm.md): the server's 4 KiB KUSER_SHARED_DATA memfd mapped
 * MAP_FIXED|MAP_SHARED|PROT_READ over the client's 4 KiB private reservation at 0x7ffe0000, in the
 * production window (guest base 8 TiB). The server's updates must be seen. */
LT_TEST(wine_kuser_shared_data_production_window)
{
    T->mm = lkd_mm_create();
    char path[300];
    snprintf(path, sizeof path, "%s/lkd_kuser_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(path);
    unlink(path);
    LT_EQ(ftruncate(fd, 0x1000), 0);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_MEMFD, &w_ops, fd, LX_O_RDWR, NULL);
    LT_EQ(sc(LKD_NR_mmap, 0x7ffe0000, 0x1000, LX_PROT_READ, PA | LX_MAP_FIXED, (uint64_t)-1, 0), 0x7ffe0000);
    LT_EQ(lkd_mm_map(T->mm, 0x7ffe0000, 0x1000, LX_PROT_READ, LX_MAP_SHARED | LX_MAP_FIXED, o, 0, "/memfd:kuser"), 0x7ffe0000);
    uint8_t *client = (uint8_t *)(uintptr_t)(BASE + 0x7ffe0000);
    int64_t server = lkd_mm_map(T->mm, 0, 0x1000, RW, LX_MAP_SHARED, o, 0, NULL);   /* the server's view */
    LT_CHECK(server > 0);
    uint8_t *sv = (uint8_t *)(uintptr_t)(BASE + server);
    for (int i = 1; i <= 50; i++) { sv[0] = (uint8_t)i; if (client[0] != i) break; }
    LT_EQ(client[0], 50);
    uint8_t v;
    LT_EQ(lkd_user_read(0x7ffe0000, &v, 1), 0);
    LT_EQ(v, 50);
    LT_ERRNO(lkd_user_write(0x7ffe0000, &v, 1), LX_EFAULT);
    LT_ERRNO(lkd_user_read(0x7ffe1000, &v, 1), LX_EFAULT);       /* sibling pages are not mapped */
    size_t len;
    char *m = lkd_mm_render_maps(T->mm, &len);
    LT_CHECK(m && strstr(m, "7ffe0000-7ffe1000 r--s 00000000"));
    free(m);
    lkd_fdobj_put(o);
    lkd_mm_put(T->mm);
    T->mm = NULL;
}

/* The configured base is occupied: the window goes to the next candidate of the measured free map. */
static void occupied_child(void *arg)
{
    (void)arg;
    munmap((void *)(uintptr_t)BASE, 1ull << 46); /* drop the window inherited from the parent */
    mach_vm_address_t a = BASE;
    if (mach_vm_allocate(mach_task_self(), &a, 0x100000, VM_FLAGS_FIXED) != KERN_SUCCESS) _exit(1);
    lkd_config c = { .size = sizeof c, .guest_window_bytes = 1ull << 40, .guest_window_base = BASE };
    /* this child has inherited an initialized LKD: reserve through the module function directly */
    mm_win.reserved = 0;
    if (mm_window_reserve(c.guest_window_base, c.guest_window_bytes, 0) != 0) _exit(2);
    if (lkd_guest_base == BASE) _exit(3);
    if (lkd_guest_base != 0x100000000000ull) _exit(4); /* 16 TiB, the next candidate */
    _exit(0);
}

LT_TEST(fallback_when_occupied)
{
    LT_EQ(lt_run_child(occupied_child, NULL, 10), 0);
}

int main(void)
{
    lkd_config c = { .size = sizeof c, .guest_window_bytes = 1ull << 46, .guest_window_base = BASE };
    if (lkd_init_config(&c) || fake_engine_init()) { printf("FAIL init\n"); return 1; }
    LT_RUN(production_window);
    LT_RUN(wine_kuser_shared_data_production_window);
    LT_RUN(whole_window_operations_are_fast);
    LT_RUN(fallback_when_occupied);
    return lt_finish();
}
