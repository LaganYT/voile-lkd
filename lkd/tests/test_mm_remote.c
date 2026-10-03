/* test_mm_remote.c: process_vm_readv/process_vm_writev and the cross-process memory service used by
 * ptrace PEEK/POKE and /proc/<pid>/mem. Linux references: process_vm_readv(2) (partial transfers,
 * EINVAL/ESRCH/EFAULT/EPERM), mm/process_vm_access.c v6.12, ptrace(2) PTRACE_POKEDATA (FOLL_FORCE writes
 * read-only text, which is why the target invalidates its translated code). */
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

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
#define MMAP(a, l, p, f) sc(LKD_NR_mmap, (a), (l), (p), (f), (uint64_t)-1, 0)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }

static void up(void)
{
    if (!T) { fake_engine_init(); T = fake_make_task(); }
    T->mm = lkd_mm_create();
    fake_engine_reset();
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }

LT_TEST(process_vm_self)
{
    up();
    int64_t a = MMAP(0, 0x3000, RW, PA);
    for (int i = 0; i < 0x3000; i++) H(a)[i] = (uint8_t)(i * 3);
    uint8_t buf[0x2000];
    struct lx_iovec l[1] = { { P(buf), sizeof buf } };
    struct lx_iovec r[2] = { { (uint64_t)a + 0x10, 0x100 }, { (uint64_t)a + 0x1000, 0x100 } };
    int32_t pid = T->proc->pid;
    LT_EQ(sc(LKD_NR_process_vm_readv, pid, P(l), 1, P(r), 2, 0), 0x200);
    LT_EQ(buf[0], (uint8_t)(0x10 * 3));
    LT_EQ(buf[0x100], 0);            /* 0x1000 * 3 mod 256 */
    LT_EQ(buf[0x101], 3);
    /* a remote iovec that reaches an unmapped page: partial count (bytes before the hole) */
    struct lx_iovec r2[2] = { { (uint64_t)a + 0x2f00, 0x100 }, { (uint64_t)a + 0x3000, 0x100 } };
    LT_EQ(sc(LKD_NR_process_vm_readv, pid, P(l), 1, P(r2), 2, 0), 0x100);
    struct lx_iovec r3[1] = { { (uint64_t)a + 0x2f80, 0x100 } };
    LT_EQ(sc(LKD_NR_process_vm_readv, pid, P(l), 1, P(r3), 1, 0), 0x80);
    struct lx_iovec r4[1] = { { (uint64_t)a - 0x100000, 0x10 } };
    LT_ERRNO(sc(LKD_NR_process_vm_readv, pid, P(l), 1, P(r4), 1, 0), LX_EFAULT); /* nothing copied */
    /* writev */
    memset(buf, 0xab, 0x40);
    struct lx_iovec wl[1] = { { P(buf), 0x40 } };
    struct lx_iovec wr[1] = { { (uint64_t)a + 0x20, 0x40 } };
    LT_EQ(sc(LKD_NR_process_vm_writev, pid, P(wl), 1, P(wr), 1, 0), 0x40);
    LT_EQ(H(a)[0x20], 0xab);
    LT_EQ(H(a)[0x5f], 0xab);
    /* no FOLL_FORCE: a read-only target page is EFAULT */
    LT_EQ(lkd_mm_protect(T->mm, a, 0x1000, LX_PROT_READ), 0);
    LT_ERRNO(sc(LKD_NR_process_vm_writev, pid, P(wl), 1, P(wr), 1, 0), LX_EFAULT);
    /* argument errors */
    LT_ERRNO(sc(LKD_NR_process_vm_readv, pid, P(l), 1, P(r), 2, 1), LX_EINVAL);     /* flags */
    LT_ERRNO(sc(LKD_NR_process_vm_readv, pid, P(l), 1025, P(r), 2, 0), LX_EINVAL);  /* > UIO_MAXIOV */
    struct lx_iovec bad[1] = { { P(buf), (uint64_t)-5 } };
    LT_ERRNO(sc(LKD_NR_process_vm_readv, pid, P(bad), 1, P(r), 1, 0), LX_EINVAL);   /* negative length */
    LT_EQ(sc(LKD_NR_process_vm_readv, pid, P(l), 0, P(r), 2, 0), 0);                /* nothing to do */
    LT_ERRNO(sc(LKD_NR_process_vm_readv, 0x3ffffff0, P(l), 1, P(r), 2, 0), LX_ESRCH); /* no such process */
    LT_ERRNO(sc(LKD_NR_process_vm_readv, pid, (uint64_t)a - 0x100000, 1, P(r), 2, 0), LX_EFAULT); /* bad iov array */
    down();
}

LT_TEST(forced_access_and_invalidation)
{
    up();
    int64_t code = MMAP(0, 0x2000, LX_PROT_READ | LX_PROT_EXEC, PA);
    int64_t none = MMAP(0, 0x4000, LX_PROT_NONE, PA);
    int32_t pid = T->proc->pid;
    int n = fake_invalidate_count;
    uint8_t cc = 0xcc, rd = 0;
    /* PTRACE_POKEDATA into read-only code succeeds (FOLL_FORCE) and invalidates the translation */
    LT_EQ(lkd_mm_remote_write(pid, code + 0x10, &cc, 1), 1);
    LT_EQ(H(code)[0x10], 0xcc);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], code + 0x10);
    /* the page is read-only again for the host afterwards */
    LT_ERRNO(lkd_user_write(code + 0x10, &cc, 1), LX_EFAULT);
    /* FOLL_FORCE also reads PROT_NONE memory */
    LT_EQ(lkd_mm_remote_read(pid, none + 0x100, &rd, 1), 1);
    LT_EQ(rd, 0);
    LT_EQ(lkd_mm_remote_read(pid, none - 0x100000, &rd, 1), -LX_EFAULT);
    size_t len;
    int err;
    char *m = lkd_mm_remote_render_maps(pid, &len, &err);
    LT_CHECK(m && err == 0 && strstr(m, "r-xp"));
    free(m);
    LT_CHECK(lkd_mm_remote_render_maps(0x3ffffff0, &len, &err) == NULL);
    LT_EQ(err, -LX_ESRCH);
    down();
}

/* Another host process: the child runs its own service and the parent reads and writes its memory. */
LT_TEST(cross_process_service)
{
    up();
    int64_t a = MMAP(0, 0x4000, RW, PA);
    int to_child[2], to_parent[2];
    LT_EQ(pipe(to_child), 0);
    LT_EQ(pipe(to_parent), 0);
    LT_EQ(mm_remote_start(), 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm(20);
        mm_remote_fork_child();               /* the service thread does not survive fork */
        for (int i = 0; i < 0x4000; i++) H(a)[i] = (uint8_t)(i ^ 0x5a);
        lkd_mm_protect(T->mm, a, 0x1000, LX_PROT_READ);
        char go = 1, done;
        write(to_parent[1], &go, 1);
        if (read(to_child[0], &done, 1) != 1) _exit(10);
        int bad = 0;
        if (H(a)[0x20] != 0x77) bad |= 1;     /* forced write landed in the read-only page */
        if (H(a)[0x1000] != (uint8_t)(0x1000 ^ 0x5a)) bad |= 2;
        mm_remote_stop();
        _exit(bad);
    }
    LT_CHECK(c > 0);
    char go;
    LT_EQ(read(to_parent[0], &go, 1), 1);
    uint8_t buf[0x100];
    LT_EQ(mm_remote_rw_host(c, a + 0x1f00, buf, 0x100, 0, 0), 0x100);
    LT_EQ(buf[0], (uint8_t)(0x1f00 ^ 0x5a));
    LT_EQ(buf[0xff], (uint8_t)(0x1fff ^ 0x5a));
    uint8_t v = 0x77;
    LT_EQ(mm_remote_rw_host(c, a + 0x20, &v, 1, 1, 0), -LX_EFAULT);  /* process_vm_writev rules */
    LT_EQ(mm_remote_rw_host(c, a + 0x20, &v, 1, 1, 1), 1);           /* ptrace POKE rules */
    LT_EQ(mm_remote_rw_host(c, a + 0x3f80, buf, 0x100, 0, 0), 0x80);  /* stops at the end of the VMA */
    size_t len;
    int err;
    char *m = mm_remote_maps_host(c, &len, &err);
    LT_CHECK(m != NULL);
    LT_EQ(err, 0);
    char want[64];
    snprintf(want, sizeof want, "%08llx-%08llx r--p", (unsigned long long)a, (unsigned long long)a + 0x1000);
    LT_CHECK(m && strstr(m, want));
    free(m);
    char done = 1;
    write(to_child[1], &done, 1);
    int st = -1;
    for (int i = 0; i < 1500; i++) {
        if (waitpid(c, &st, WNOHANG) == c) break;
        usleep(10000);
        if (i == 1499) { kill(c, SIGKILL); waitpid(c, &st, 0); }
    }
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* the child is gone: its service is unreachable */
    LT_EQ(mm_remote_rw_host(c, a, buf, 1, 0, 0), -LX_ESRCH);
    mm_remote_stop();
    close(to_child[0]); close(to_child[1]); close(to_parent[0]); close(to_parent[1]);
    down();
}

int main(void)
{
    LT_RUN(process_vm_self);
    LT_RUN(forced_access_and_invalidation);
    LT_RUN(cross_process_service);
    return lt_finish();
}
