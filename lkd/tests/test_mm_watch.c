/* test_mm_watch.c: host pages holding translated code (FEX mtrack-style self-modifying-code detection,
 * src/mm/watch.c). The model is FEX's own Linux behavior: the JIT write-protects pages it translated,
 * the first guest store faults, the translations of the page are dropped and the store is retried
 * (FEX Source/Tools/LinuxEmulation/LinuxSyscalls/SignalDelegator, SMCChecks=MTrack). The handler below
 * is what the engine does: lkd_fault_try_recover first, then lkd_mm_code_write_fault for any store fault
 * (ESR EC 0x24, WnR = 1). The fault status is not filtered: a store to a read-only page that is not
 * resident yet is a TRANSLATION fault (DFSC 0x06, MESURE below), a resident one a permission fault. */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <sys/wait.h>

#include "ltest.h"
#include "fake_engine.h"
#include "lkd_fs.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"
#include "../src/mm/lkd_mm_watch.h"

static lkd_task *T;
static int64_t sc(unsigned nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    lkd_sysreq rq = { nr, { a, b, c, d, e, f } };
    return lkd_dispatch(T, &rq);
}
#define MMAP(a, l, p, f) sc(LKD_NR_mmap, (a), (l), (p), (f), (uint64_t)-1, 0)
#define RW (LX_PROT_READ | LX_PROT_WRITE)
#define RX (LX_PROT_READ | LX_PROT_EXEC)
#define PA (LX_MAP_PRIVATE | LX_MAP_ANONYMOUS)
static uint8_t *H(uint64_t g) { return lkd_g2h(g); }

static sigjmp_buf jb;
static volatile int armed, faults, handled;

static void handler(int sig, siginfo_t *si, void *ctx)
{
    (void)si;
    if (lkd_fault_try_recover(ctx)) return;
    ucontext_t *uc = ctx;
    uint32_t esr = uc->uc_mcontext->__es.__esr;
    uint64_t far = uc->uc_mcontext->__es.__far;
    uint32_t ec = esr >> 26, wnr = (esr >> 6) & 1;
    faults++;
    if ((ec == 0x24 || ec == 0x25) && wnr && lkd_mm_code_write_fault(lkd_h2g((void *)far)) == 1) {
        handled++;
        return; /* retry the store */
    }
    if (armed) siglongjmp(jb, 1);
    signal(sig, SIG_DFL);
}

/* A guest store: 0 when it completed, -1 when it was a real fault. */
static int store(uint64_t g, uint8_t v)
{
    armed = 1;
    if (sigsetjmp(jb, 1)) { armed = 0; return -1; }
    *(volatile uint8_t *)H(g) = v;
    armed = 0;
    return 0;
}

static int host_writable(uint64_t g)
{
    char c = *(volatile char *)H(g);
    int p[2];
    if (pipe(p)) return -1;
    if (write(p[1], &c, 1) != 1) return -1;
    int r = read(p[0], H(g), 1) == 1; /* the host kernel answers EFAULT without a signal */
    close(p[0]); close(p[1]);
    return r;
}

static void up(void)
{
    if (!T) {
        fake_engine_init();
        T = fake_make_task();
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
    }
    T->mm = lkd_mm_create();
    fake_engine_reset();
    faults = handled = 0;
}
static void down(void) { lkd_mm_put(T->mm); T->mm = NULL; }

LT_TEST(watch_then_store)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    H(hp)[0] = 1;
    LT_EQ(host_writable(hp), 1);
    LT_EQ(lkd_mm_watch_code(hp + 0x100, 16), 0);
    LT_EQ(lkd_mm_code_watched(hp + 0x3fff), 1);       /* the whole 16 KiB host page */
    LT_EQ(lkd_mm_code_watched(hp + 0x4000), 0);
    LT_EQ(host_writable(hp + 0x2000), 0);             /* PROT_WRITE removed though the VMA is writable */
    LT_EQ(H(hp)[0], 1);                               /* still readable */
    LT_EQ(lkd_mm_watch_code(hp, 0x4000), 0);          /* idempotent */
    int n = fake_invalidate_count;
    LT_EQ(store(hp + 0x200, 5), 0);                   /* faults once, translations dropped, retried */
    LT_EQ(faults, 1);
    LT_EQ(handled, 1);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], hp);
    LT_EQ(fake_inval_len[n % 16], 0x4000);
    LT_EQ(H(hp)[0x200], 5);
    LT_EQ(lkd_mm_code_watched(hp), 0);
    LT_EQ(host_writable(hp), 1);
    LT_EQ(store(hp + 0x300, 6), 0);                   /* no second fault */
    LT_EQ(faults, 1);
    LT_EQ(fake_invalidate_count, n + 1);
    /* a page already restored by another thread: retry without a second invalidation */
    LT_EQ(lkd_mm_code_write_fault(hp + 8), 1);        /* writable already (another thread won): retry */
    LT_EQ(fake_invalidate_count, n + 1);
    LT_ERRNO(lkd_mm_watch_code(lkd_mm_window_guest_hi(), 16), LX_EFAULT);
    LT_EQ(lkd_mm_watch_code(hp, 0), 0);
    down();
}

LT_TEST(endless_retry_is_bounded)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    /* An unwatched, writable page answers "retry" (another thread restored it) a bounded number of times
     * in a row, then reports a real fault: a store that keeps failing must not loop forever. */
    LT_EQ(lkd_mm_code_write_fault(lkd_mm_window_guest_hi() - 0x1000), 0); /* a real fault resets the count */
    int ones = 0;
    for (int i = 0; i < 20 && lkd_mm_code_write_fault(hp + 8) == 1; i++) ones++;
    LT_EQ(ones, 4);
    LT_EQ(lkd_mm_code_write_fault(hp + 0x4008) , 1);   /* a different page starts a new count */
    down();
}

LT_TEST(store_to_nonresident_watched_page)
{
    up();
    /* never touched before the watch: the host fault is a translation fault, not a permission fault */
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(lkd_mm_watch_code(hp, 0x10), 0);
    int n = fake_invalidate_count;
    LT_EQ(store(hp + 0x20, 8), 0);
    LT_EQ(handled, 1);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(H(hp)[0x20], 8);
    down();
}

LT_TEST(real_faults_stay_faults)
{
    up();
    int64_t r = MMAP(0, 0x4000, LX_PROT_READ, PA);
    int n = fake_invalidate_count;
    LT_EQ(lkd_mm_code_write_fault(r), 0);             /* not watched, VMA read-only */
    LT_EQ(store(r, 1), -1);                           /* a real guest fault */
    LT_EQ(handled, 0);
    int64_t code = MMAP(0, 0x4000, RX, PA);           /* translated code in a read-only VMA */
    n = fake_invalidate_count;                        /* (mapping code announces one invalidation) */
    LT_EQ(lkd_mm_watch_code(code, 0x4000), 0);
    LT_EQ(lkd_mm_code_write_fault(code + 4), 0);      /* the VMA forbids the write */
    LT_EQ(lkd_mm_code_watched(code), 1);
    LT_EQ(store(code + 4, 1), -1);
    LT_EQ(fake_invalidate_count, n);
    LT_EQ(lkd_mm_code_write_fault(lkd_mm_window_guest_hi() + 0x1000), 0); /* outside the window */
    down();
}

LT_TEST(kernel_copy_into_watched_page)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(lkd_mm_watch_code(hp, 0x10), 0);
    int n = fake_invalidate_count;
    uint32_t v = 0x11223344;
    /* read() into a buffer on a code page, ELF loader writes: lkd_user_write succeeds */
    LT_EQ(lkd_user_write(hp + 0x40, &v, 4), 0);
    LT_EQ(*(uint32_t *)H(hp + 0x40), 0x11223344);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], hp);
    LT_EQ(faults, 0);                                  /* resolved before the copy */
    LT_EQ(lkd_mm_code_watched(hp), 0);
    /* process_vm_writev into a watched page */
    LT_EQ(lkd_mm_watch_code(hp, 0x10), 0);
    n = fake_invalidate_count;
    uint8_t buf[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    struct lx_iovec l = { (uint64_t)(uintptr_t)buf, 8 }, rv = { hp + 0x80, 8 };
    LT_EQ(sc(LKD_NR_process_vm_writev, T->proc->pid, (uint64_t)(uintptr_t)&l, 1, (uint64_t)(uintptr_t)&rv, 1, 0), 8);
    LT_EQ(H(hp + 0x80)[7], 9);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(lkd_mm_code_watched(hp), 0);
    /* ptrace POKE (FOLL_FORCE) into watched read-only code */
    int64_t code = MMAP(0, 0x8000, RX, PA);
    uint64_t cp = HP_UP((uint64_t)code);
    LT_EQ(lkd_mm_watch_code(cp, 0x4000), 0);
    n = fake_invalidate_count;
    uint8_t cc = 0xcc;
    LT_EQ(lkd_mm_remote_write(T->proc->pid, cp + 0x10, &cc, 1), 1);
    LT_EQ(H(cp)[0x10], 0xcc);
    LT_CHECK(fake_invalidate_count >= n + 1);
    LT_EQ(lkd_mm_code_watched(cp), 0);
    LT_EQ(faults, 0);
    down();
}

/* The code VMA and a data VMA share one 16 KiB host page: a store to the data faults too (host page
 * granularity), and is resolved the same way. */
LT_TEST(host_page_shared_with_data)
{
    up();
    uint64_t hp = lkd_mm_window_guest_lo() + 0x20000000;
    LT_EQ(MMAP(hp, 0x1000, RX, PA | LX_MAP_FIXED), hp);
    LT_EQ(MMAP(hp + 0x1000, 0x3000, RW, PA | LX_MAP_FIXED), hp + 0x1000);
    LT_EQ(host_writable(hp + 0x1000), 1);
    LT_EQ(lkd_mm_watch_code(hp, 0x1000), 0);
    LT_EQ(host_writable(hp + 0x1000), 0);
    int n = fake_invalidate_count;
    LT_EQ(store(hp + 0x2000, 3), 0);
    LT_EQ(handled, 1);
    LT_EQ(fake_invalidate_count, n + 1);
    LT_EQ(fake_inval_start[n % 16], hp);
    LT_EQ(fake_inval_len[n % 16], 0x4000);
    LT_EQ(H(hp + 0x2000)[0], 3);
    LT_EQ(lkd_mm_code_watched(hp), 0);
    /* ptrace POKE (FOLL_FORCE) into the read-only code of that page while it is watched: the VMA union of
     * the page is RW but the host page is read-only until the watch is dropped. Linux writes the INT3
     * (access_remote_vm with FOLL_FORCE); the EAC probe cold_INT3_POKEDATA got EIO here. */
    LT_EQ(lkd_mm_watch_code(hp, 0x1000), 0);
    n = fake_invalidate_count;
    uint8_t cc = 0xcc;
    LT_EQ(lkd_mm_remote_write(T->proc->pid, hp + 0x40, &cc, 1), 1);
    LT_EQ(H(hp)[0x40], 0xcc);
    LT_CHECK(fake_invalidate_count >= n + 1);          /* the watch drop, then the written code range */
    LT_EQ(lkd_mm_code_watched(hp), 0);
    LT_EQ(host_writable(hp + 0x1000), 1);              /* the data VMA is writable again */
    down();
}

LT_TEST(mprotect_keeps_munmap_drops)
{
    up();
    int64_t a = MMAP(0, 0xc000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(lkd_mm_watch_code(hp, 0x4000), 0);
    /* mprotect recomputes protections without giving the write permission back */
    LT_EQ(sc(LKD_NR_mprotect, hp + 0x1000, 0x1000, LX_PROT_READ, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_mprotect, hp, 0x4000, RW, 0, 0, 0), 0);
    LT_EQ(lkd_mm_code_watched(hp), 1);
    LT_EQ(host_writable(hp), 0);
    /* a partial munmap keeps the watch of a page still in use (remaining translations stay protected) */
    LT_EQ(sc(LKD_NR_munmap, hp + 0x3000, 0x1000, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_code_watched(hp), 1);
    /* munmap of the whole host page drops it; a new mapping there is writable */
    LT_EQ(sc(LKD_NR_munmap, hp, 0x4000, 0, 0, 0, 0), 0);
    LT_EQ(lkd_mm_code_watched(hp), 0);
    LT_EQ(MMAP(hp, 0x4000, RW, PA | LX_MAP_FIXED), hp);
    LT_EQ(host_writable(hp), 1);
    LT_EQ(faults, 0);
    /* mremap: the moved range is not watched at its new address */
    LT_EQ(lkd_mm_watch_code(hp, 0x4000), 0);
    uint64_t dst = lkd_mm_window_guest_lo() + 0x21000000;
    LT_EQ(sc(LKD_NR_mremap, hp, 0x4000, 0x4000, LX_MREMAP_MAYMOVE | LX_MREMAP_FIXED, dst, 0), dst);
    LT_EQ(lkd_mm_code_watched(dst), 0);
    LT_EQ(lkd_mm_code_watched(hp), 0);
    LT_EQ(host_writable(dst), 1);
    /* exec drops every watch */
    LT_EQ(lkd_mm_watch_code(dst, 0x10), 0);
    lkd_mm_exec_commit(T);
    LT_EQ(lkd_mm_code_watched(dst), 0);
    down();
}

LT_TEST(shared_mapping_and_madvise)
{
    up();
    int64_t s = MMAP(0, 0x4000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);
    LT_EQ(lkd_mm_watch_code(s, 0x10), 0);
    LT_EQ(host_writable(s), 0);
    LT_EQ(store(s + 1, 4), 0);                        /* same rule inside this process */
    LT_EQ(handled, 1);
    LT_EQ(H(s)[1], 4);
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    H(hp)[5] = 1;
    LT_EQ(lkd_mm_watch_code(hp, 0x10), 0);
    LT_EQ(sc(LKD_NR_madvise, hp, 0x4000, LX_MADV_POPULATE_WRITE, 0, 0, 0), 0); /* no store into the code page */
    LT_EQ(sc(LKD_NR_madvise, hp, 0x4000, LX_MADV_DONTNEED, 0, 0, 0), 0);
    LT_EQ(H(hp)[5], 0);
    LT_EQ(host_writable(hp), 0);                       /* still protected */
    LT_EQ(faults, 1);
    down();
}

LT_TEST(fork_child_has_no_watch)
{
    up();
    int64_t a = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(lkd_mm_watch_code(hp, 0x10), 0);
    fflush(NULL);
    pid_t p = lkd_fork_host(T, 0);
    if (p == 0) {
        alarm(10);
        int bad = 0;
        if (lkd_mm_code_watched(hp)) bad |= 1;
        if (host_writable(hp) != 1) bad |= 2;      /* the child inherits no translations */
        _exit(bad);
    }
    int st = -1;
    for (int i = 0; i < 1000 && waitpid(p, &st, WNOHANG) != p; i++) usleep(10000);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    LT_EQ(lkd_mm_code_watched(hp), 1);              /* the parent keeps its watch */
    LT_EQ(host_writable(hp), 0);
    down();
}

/* Request wine-7: one shared object, two views in this process. Code is translated from the RX view B and
 * rewritten through the RW view A; the store through A must drop B's translations (FEX would run stale
 * code otherwise). A view mapped later inherits the watch, and a write() through the descriptor counts too. */
LT_TEST(aliased_views_share_the_watch)
{
    up();
    char path[] = "/tmp/lkd-alias-XXXXXX";
    int hfd = mkstemp(path);
    LT_CHECK(hfd >= 0);
    unlink(path);
    LT_EQ(ftruncate(hfd, 0x8000), 0);
    lkd_fdobj *o = NULL;
    LT_EQ(lkd_fs_wrap_host_fd(hfd, LX_O_RDWR, NULL, &o), 0);
    int64_t a = lkd_mm_map(T->mm, 0, 0x4000, RW, LX_MAP_SHARED, o, 0, NULL);     /* view A: RW */
    int64_t b = lkd_mm_map(T->mm, 0, 0x4000, RX, LX_MAP_SHARED, o, 0, NULL);     /* view B: RX */
    LT_CHECK(a > 0 && b > 0 && a != b);
    LT_EQ(lkd_mm_watch_code((uint64_t)b, 0x10), 0);                              /* code translated from B */
    LT_EQ(lkd_mm_code_watched((uint64_t)a), 1);                                   /* A is a view of the same bytes */
    LT_EQ(host_writable((uint64_t)a), 0);
    int n = fake_invalidate_count;
    LT_EQ(store((uint64_t)a + 8, 0x90), 0);                                       /* rewrite through A */
    LT_EQ(handled, 1);
    LT_EQ(H((uint64_t)b)[8], 0x90);
    int sawb = 0;
    for (int k = n; k < fake_invalidate_count; k++) if (fake_inval_start[k % 16] == (uint64_t)b) sawb = 1;
    LT_CHECK(sawb);                                                               /* B's translations dropped */
    LT_EQ(lkd_mm_code_watched((uint64_t)b), 0);
    LT_EQ(lkd_mm_code_watched((uint64_t)a), 0);
    /* watch again, then a third view C: it starts write-protected */
    LT_EQ(lkd_mm_watch_code((uint64_t)b, 0x10), 0);
    int64_t c = lkd_mm_map(T->mm, 0, 0x4000, RW, LX_MAP_SHARED, o, 0, NULL);
    LT_CHECK(c > 0);
    LT_EQ(lkd_mm_code_watched((uint64_t)c), 1);
    LT_EQ(host_writable((uint64_t)c), 0);
    /* a pwrite through the descriptor drops the translations of every view */
    n = fake_invalidate_count;
    lkd_mm_file_written(o, 0x10, 4);
    sawb = 0;
    for (int k = n; k < fake_invalidate_count; k++) if (fake_inval_start[k % 16] == (uint64_t)b) sawb = 1;
    LT_CHECK(sawb);
    LT_EQ(lkd_mm_code_watched((uint64_t)b), 0);
    LT_EQ(host_writable((uint64_t)a), 1);
    lkd_mm_unmap(T->mm, (uint64_t)a, 0x4000);
    lkd_mm_unmap(T->mm, (uint64_t)b, 0x4000);
    lkd_mm_unmap(T->mm, (uint64_t)c, 0x4000);
    lkd_fdobj_put(o);
    down();
}

int main(void)
{
    LT_RUN(watch_then_store);
    LT_RUN(aliased_views_share_the_watch);
    LT_RUN(endless_retry_is_bounded);
    LT_RUN(store_to_nonresident_watched_page);
    LT_RUN(real_faults_stay_faults);
    LT_RUN(kernel_copy_into_watched_page);
    LT_RUN(host_page_shared_with_data);
    LT_RUN(mprotect_keeps_munmap_drops);
    LT_RUN(shared_mapping_and_madvise);
    LT_RUN(fork_child_has_no_watch);
    return lt_finish();
}
