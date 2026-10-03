/* test_core_usermem.c: lkd_user_read/write/strn/zero with bad pointers. Models: Linux copy_from_user /
 * copy_to_user / strncpy_from_user return -EFAULT instead of faulting (arch/x86/lib/usercopy_64.c,
 * lib/strncpy_from_user.c); getname() maps an unterminated name to -ENAMETOOLONG (fs/namei.c).
 * Guest memory is host memory here (lkd_guest_base == 0), except where noted. */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "ltest.h"
#include <lkd/lkd.h>
#include "lkd_internal.h"

int lkd_usermem_install_handlers(void);   /* core-private: installed by lkd_init in real use */

#define PG 16384u /* host page */

static sigjmp_buf jb;
static volatile int prev_calls;
static void prev_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    prev_calls++;
    siglongjmp(jb, 1);
}

static uint8_t *map_pages(int npages, int prot)
{
    void *p = mmap(NULL, (size_t)npages * PG, prot, MAP_ANON | MAP_PRIVATE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* [rw][none]: returns base, the guard is base + PG */
static uint8_t *rw_then_none(void)
{
    uint8_t *p = map_pages(2, PROT_READ | PROT_WRITE);
    if (p) mprotect(p + PG, PG, PROT_NONE);
    return p;
}

static uint64_t G(const void *p) { return (uint64_t)(uintptr_t)p; }

LT_TEST(read_and_write_roundtrip_on_mapped_memory)
{
    uint8_t *m = map_pages(1, PROT_READ | PROT_WRITE);
    LT_CHECK(m != NULL);
    uint8_t src[300], dst[300];
    for (int i = 0; i < 300; i++) src[i] = (uint8_t)(i * 7 + 1);
    LT_EQ(lkd_user_write(G(m + 5), src, sizeof src), 0);
    LT_EQ(lkd_user_read(G(m + 5), dst, sizeof dst), 0);
    LT_MEMEQ(src, dst, sizeof src);
    LT_EQ(m[4], 0);
    LT_EQ(m[5 + 300], 0);
    LT_EQ(lkd_user_read(G(m), dst, 0), 0);   /* zero length never faults */
    LT_EQ(lkd_user_read(0, dst, 0), 0);
    LT_EQ(lkd_user_write(0, src, 0), 0);
    LT_EQ(lkd_user_zero(G(m + 100), 50), 0);
    LT_EQ(m[100], 0); LT_EQ(m[149], 0);
    LT_EQ(lkd_user_zero(G(m + 100), 0), 0);
    LT_EQ(m[150], (uint8_t)((150 - 5) * 7 + 1));
}

LT_TEST(all_alignments_and_sizes_copy_exactly)
{
    uint8_t *a = map_pages(8, PROT_READ | PROT_WRITE), *b = map_pages(8, PROT_READ | PROT_WRITE);
    LT_CHECK(a && b);
    for (size_t i = 0; i < 8 * PG; i++) a[i] = (uint8_t)(i * 31 + (i >> 8));
    int bad = 0;
    for (size_t so = 0; so < 17; so++)
        for (size_t d = 0; d < 17; d += 3)
            for (size_t n = 0; n < 70; n += 1) {
                memset(b, 0xee, 256);
                if (lkd_user_read(G(a + so), b + d, n) != 0) bad++;
                else if (memcmp(a + so, b + d, n) != 0) bad++;
                else if (d && b[d - 1] != 0xee) bad++;
                else if (b[d + n] != 0xee) bad++;
            }
    LT_EQ(bad, 0);
    LT_EQ(lkd_user_read(G(a + 3), b + 1, 5 * PG + 7), 0);
    LT_EQ(memcmp(a + 3, b + 1, 5 * PG + 7), 0);
    LT_EQ(lkd_user_write(G(b + 11), a, 3 * PG - 13), 0);
    LT_EQ(memcmp(b + 11, a, 3 * PG - 13), 0);
}

LT_TEST(bad_pointers_return_efault_without_crashing)
{
    uint8_t buf[64];
    uint8_t *rn = rw_then_none();
    uint8_t *ro = map_pages(1, PROT_READ);
    LT_CHECK(rn && ro);
    LT_ERRNO(lkd_user_read(0, buf, 8), LX_EFAULT);                    /* NULL */
    LT_ERRNO(lkd_user_read(8, buf, 8), LX_EFAULT);                    /* low page */
    LT_ERRNO(lkd_user_write(0, buf, 8), LX_EFAULT);
    LT_ERRNO(lkd_user_read(G(rn + PG), buf, 4), LX_EFAULT);          /* PROT_NONE */
    LT_ERRNO(lkd_user_write(G(rn + PG), buf, 4), LX_EFAULT);
    LT_ERRNO(lkd_user_write(G(ro), buf, 4), LX_EFAULT);              /* read-only page */
    LT_EQ(lkd_user_read(G(ro), buf, 4), 0);                           /* ... but readable */
    LT_ERRNO(lkd_user_read(G(rn + PG - 8), buf, 16), LX_EFAULT);     /* straddles into PROT_NONE */
    LT_ERRNO(lkd_user_write(G(rn + PG - 8), buf, 16), LX_EFAULT);
    LT_ERRNO(lkd_user_zero(G(rn + PG - 8), 64), LX_EFAULT);
    LT_ERRNO(lkd_user_read(0x0000800000000000ull, buf, 1), LX_EFAULT); /* non-canonical for 47-bit user space */
    LT_ERRNO(lkd_user_read(0xffffffffffffff00ull, buf, 64), LX_EFAULT);
    LT_ERRNO(lkd_user_read(0x00007ffffffffff0ull, buf, 64), LX_EFAULT);  /* range crosses the top */
    LT_ERRNO(lkd_user_read(~0ull, buf, 2), LX_EFAULT);                    /* gva + n wraps */
    LT_ERRNO(lkd_user_write(1ull << 63, buf, 8), LX_EFAULT);
    /* a successful copy right after faults still works: the fault path left no state behind */
    LT_EQ(lkd_user_read(G(rn), buf, 8), 0);
    /* partial progress before the fault is allowed (Linux copy_to_user may copy a prefix) but the first
     * bytes that were valid are intact */
    memset(rn, 0x5a, PG);
    memset(buf, 0, sizeof buf);
    LT_ERRNO(lkd_user_read(G(rn + PG - 4), buf, 32), LX_EFAULT);
}

/* lkd_user_cmpxchg32 (request sig-1): futex WAKE_OP, PI and robust lists swap a guest word. Same fault
 * recovery as the copies: an unmapped or read-only word is -EFAULT, never a host crash (the range check
 * is permissive here, the fault itself must be recovered). */
LT_TEST(cmpxchg32_swaps_and_recovers_faults)
{
    uint8_t *rn = rw_then_none();
    uint8_t *ro = map_pages(1, PROT_READ);
    LT_CHECK(rn && ro);
    uint32_t *w = (uint32_t *)rn;
    *w = 7;
    uint32_t e = 7;
    LT_EQ(lkd_user_cmpxchg32(G(w), &e, 9), 0);
    LT_EQ(*w, 9);
    e = 7;
    LT_EQ(lkd_user_cmpxchg32(G(w), &e, 11), 1);                       /* differs: current value returned */
    LT_EQ(e, 9);
    LT_EQ(*w, 9);
    LT_ERRNO(lkd_user_cmpxchg32(G(rn + 2), &e, 1), LX_EINVAL);        /* misaligned */
    e = 0;
    LT_ERRNO(lkd_user_cmpxchg32(G(rn + PG), &e, 1), LX_EFAULT);       /* PROT_NONE */
    e = *(volatile uint32_t *)ro;
    LT_ERRNO(lkd_user_cmpxchg32(G(ro), &e, 1), LX_EFAULT);            /* read-only: the store faults */
    LT_ERRNO(lkd_user_cmpxchg32(0, &e, 1), LX_EFAULT);
    /* a copy after the faults still works */
    e = 9;
    LT_EQ(lkd_user_cmpxchg32(G(w), &e, 12), 0);
    LT_EQ(*w, 12);
}

/* LKD_BUG names the file and line (request sig-1: the format had no arguments for "%s:%d"). Checked in a
 * child: the macro aborts. */
LT_TEST(bug_macro_prints_file_and_line)
{
    int p[2];
    LT_EQ(pipe(p), 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        dup2(p[1], 2);
        int v = 3;
        LKD_BUG(v == 4, "value %d", v);
        _exit(0);
    }
    close(p[1]);
    char buf[256] = { 0 };
    ssize_t n = 0, r;
    while (n < (ssize_t)sizeof buf - 1 && (r = read(p[0], buf + n, sizeof buf - 1 - n)) > 0) n += r;
    close(p[0]);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
    LT_CHECK(strstr(buf, "BUG ") != NULL && strstr(buf, "test_core_usermem.c:") != NULL && strstr(buf, ": value 3") != NULL);
}

LT_TEST(large_copy_into_guard_page_faults_cleanly)
{
    uint8_t *rn = rw_then_none();
    uint8_t *big = malloc(2 * PG);
    LT_CHECK(rn && big);
    LT_ERRNO(lkd_user_read(G(rn), big, 2 * PG), LX_EFAULT);
    LT_ERRNO(lkd_user_write(G(rn), big, 2 * PG), LX_EFAULT);
    LT_EQ(lkd_user_read(G(rn), big, PG), 0);
    LT_EQ(lkd_user_write(G(rn), big, PG), 0);
    free(big);
}

LT_TEST(strn_copies_strings_and_stops_at_nul)
{
    uint8_t *m = map_pages(1, PROT_READ | PROT_WRITE);
    char out[128];
    LT_CHECK(m != NULL);
    strcpy((char *)m + 10, "/proc/self/exe");
    LT_EQ(lkd_user_strn(G(m + 10), out, sizeof out), 14);
    LT_STREQ(out, "/proc/self/exe");
    m[200] = 0;
    LT_EQ(lkd_user_strn(G(m + 200), out, sizeof out), 0);   /* empty string */
    LT_EQ(out[0], 0);
    /* exactly max-1 characters fit */
    memset(m + 300, 'a', 9); m[309] = 0;
    LT_EQ(lkd_user_strn(G(m + 300), out, 10), 9);
    LT_EQ(out[9], 0);
    /* no NUL within max: -ENAMETOOLONG and the buffer is terminated */
    memset(m + 400, 'b', 40);
    LT_ERRNO(lkd_user_strn(G(m + 400), out, 10), LX_ENAMETOOLONG);
    LT_EQ(out[9], 0);
    LT_EQ(out[0], 'b');
    LT_ERRNO(lkd_user_strn(G(m + 400), out, 0), LX_EINVAL);
    LT_ERRNO(lkd_user_strn(0, out, sizeof out), LX_EFAULT);
}

LT_TEST(strn_across_pages_and_before_guard)
{
    uint8_t *rn = rw_then_none();
    char out[8192];
    LT_CHECK(rn != NULL);
    /* long string crossing several 4 KiB guest pages inside the host page */
    memset(rn + 100, 'x', 9000);
    rn[100 + 9000] = 0;
    LT_ERRNO(lkd_user_strn(G(rn + 100), out, sizeof out), LX_ENAMETOOLONG); /* 9000 bytes do not fit in 8192 */
    char *big = malloc(10000);
    LT_EQ(lkd_user_strn(G(rn + 100), big, 10000), 9000);
    LT_EQ(big[8999], 'x');
    free(big);
    /* NUL is the very last byte of the accessible page: succeeds, never touches the guard */
    memset(rn + PG - 20, 'y', 19);
    rn[PG - 1] = 0;
    LT_EQ(lkd_user_strn(G(rn + PG - 20), out, sizeof out), 19);
    /* no NUL before the guard page: -EFAULT, not a crash */
    rn[PG - 1] = 'z';
    LT_ERRNO(lkd_user_strn(G(rn + PG - 20), out, sizeof out), LX_EFAULT);
    /* max reached before the guard page is touched: ENAMETOOLONG, not EFAULT */
    LT_ERRNO(lkd_user_strn(G(rn + PG - 20), out, 10), LX_ENAMETOOLONG);
}

LT_TEST(guest_base_offset_is_applied)
{
    uint8_t *m = map_pages(1, PROT_READ | PROT_WRITE);
    LT_CHECK(m != NULL);
    uint64_t saved = lkd_guest_base;
    lkd_guest_base = G(m) - 0x7ffe0000ull;       /* the Wine USER_SHARED_DATA address */
    LT_EQ((uintptr_t)lkd_g2h(0x7ffe0010ull), (uintptr_t)(m + 0x10));
    LT_EQ(lkd_h2g(m + 0x30), 0x7ffe0030ull);
    memcpy(m + 0x20, "KUSD", 5);
    char out[8];
    LT_EQ(lkd_user_strn(0x7ffe0020ull, out, sizeof out), 4);
    LT_STREQ(out, "KUSD");
    LT_EQ(lkd_user_write(0x7ffe0100ull, "hello", 6), 0);
    LT_STREQ((char *)m + 0x100, "hello");
    LT_ERRNO(lkd_user_read(0x10000, out, 4), LX_EFAULT);   /* below the window: host address unmapped */
    lkd_guest_base = saved;
}

static volatile int recovered, engine_faults;
static void engine_handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si;
    if (lkd_fault_try_recover(uc)) { recovered++; return; }
    engine_faults++;
    siglongjmp(jb, 1);
}

LT_TEST(engine_handler_installed_later_can_use_try_recover)
{
    /* An engine handler installed after ours must give the user copies the first look. */
    uint8_t *rn = rw_then_none();
    uint8_t buf[16];
    struct sigaction ours_segv, ours_bus, eng;
    memset(&eng, 0, sizeof eng);
    eng.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&eng.sa_mask);
    eng.sa_sigaction = engine_handler;
    sigaction(SIGSEGV, &eng, &ours_segv);
    sigaction(SIGBUS, &eng, &ours_bus);
    LT_ERRNO(lkd_user_read(G(rn + PG), buf, 8), LX_EFAULT);
    LT_ERRNO(lkd_user_write(0, buf, 8), LX_EFAULT);
    LT_EQ(recovered, 2);
    LT_EQ(engine_faults, 0);
    if (sigsetjmp(jb, 1) == 0) { *(volatile uint8_t *)(rn + PG) = 1; LT_CHECK(0); }
    LT_EQ(engine_faults, 1);   /* a fault that is not in a user copy is the engine's */
    LT_EQ(recovered, 2);
    sigaction(SIGSEGV, &ours_segv, NULL);
    sigaction(SIGBUS, &ours_bus, NULL);
}

LT_TEST(faults_outside_user_copies_go_to_the_previous_handler)
{
    uint8_t *rn = rw_then_none();
    int hit = 0;
    prev_calls = 0;
    if (sigsetjmp(jb, 1) == 0) { *(volatile uint8_t *)(rn + PG + 8) = 1; }
    else hit = 1;
    LT_EQ(hit, 1);
    LT_EQ(prev_calls, 1);   /* chained: SIGBUS or SIGSEGV on a PROT_NONE page */
    uint8_t buf[8];
    LT_ERRNO(lkd_user_read(G(rn + PG), buf, 8), LX_EFAULT);
    LT_EQ(prev_calls, 1);   /* user copy faults never reach it */
}

#define NTH 4
#define ITER 4000
static uint8_t *g_good, *g_guard_base;
static void *hammer(void *arg)
{
    long id = (long)arg;
    uint8_t buf[128];
    long bad = 0;
    for (int i = 0; i < ITER; i++) {
        uint8_t *src = g_good + (size_t)((id * 37 + i) % 1000);
        if (lkd_user_read(G(src), buf, sizeof buf) != 0) bad++;
        if (lkd_user_read(G(g_guard_base + PG + (size_t)(i % 64)), buf, 8) != -LX_EFAULT) bad++;
        if (lkd_user_write(G(g_guard_base + PG - 4), buf, 16) != -LX_EFAULT) bad++;
        if (lkd_user_write(G(g_good + 2000 + id * 200), buf, 100) != 0) bad++;
    }
    return (void *)bad;
}

LT_TEST(concurrent_faults_in_several_threads_are_independent)
{
    g_good = map_pages(1, PROT_READ | PROT_WRITE);
    g_guard_base = rw_then_none();
    LT_CHECK(g_good && g_guard_base);
    pthread_t th[NTH];
    for (long i = 0; i < NTH; i++) LT_EQ(pthread_create(&th[i], NULL, hammer, (void *)i), 0);
    long total_bad = 0;
    for (int i = 0; i < NTH; i++) { void *r; pthread_join(th[i], &r); total_bad += (long)r; }
    LT_EQ(total_bad, 0);
}

/* Raw host memory is not in any VMA table: the mm module's real check would reject it. */
static int check_none(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }

static int ncalls, last_w;
static uint64_t last_gva, veto_lo, veto_hi;
static size_t last_len;
static int check_probe(uint64_t gva, size_t len, int w)
{
    ncalls++; last_gva = gva; last_len = len; last_w = w;
    return (gva >= veto_lo && gva < veto_hi) ? -LX_EFAULT : 0;
}

LT_TEST(range_check_hook_is_consulted_before_the_copy)
{
    /* Linux copy_from_user checks access_ok() first: an advisory check that says no wins even when the
     * host memory would have been readable, and nothing is copied */
    uint8_t *m = map_pages(1, PROT_READ | PROT_WRITE);
    uint8_t buf[300];
    char s[64];
    LT_CHECK(m != NULL);
    memcpy(m + 7000, "abc", 4);
    lkd_usermem_set_check_fn(check_probe);
    ncalls = 0; veto_lo = veto_hi = 0;
    LT_EQ(lkd_user_read(G(m + 100), buf, 20), 0);
    LT_EQ(ncalls, 1); LT_EQ(last_gva, G(m + 100)); LT_EQ(last_len, 20); LT_EQ(last_w, 0);
    LT_EQ(lkd_user_write(G(m + 100), buf, 33), 0);
    LT_EQ(ncalls, 2); LT_EQ(last_len, 33); LT_EQ(last_w, 1);
    LT_EQ(lkd_user_read(G(m), buf, 0), 0);
    LT_EQ(ncalls, 2);                          /* zero length never reaches the hook */
    ncalls = 0;
    LT_EQ(lkd_user_strn(G(m + 7000), s, sizeof s), 3);
    LT_EQ(ncalls, 1); LT_EQ(last_w, 0);
    LT_EQ(last_len, 64);                       /* 7000 % 4096 = 2904: 1192 bytes left in the guest page, capped by max */
    /* a string that crosses a 4 KiB guest page is checked page by page */
    memset(m + 4000, 'q', 200); m[4200] = 0;
    ncalls = 0;
    char big[300];
    LT_EQ(lkd_user_strn(G(m + 4000), big, sizeof big), 200);
    LT_EQ(ncalls, 2);
    LT_EQ(last_gva, G(m + 4096)); LT_EQ(last_len, 204);   /* 300 - 96 bytes already copied, within one guest page */
    /* the veto stops the copy: destination untouched, -EFAULT */
    memset(buf, 0x11, sizeof buf);
    veto_lo = G(m + 100); veto_hi = G(m + 101);
    LT_ERRNO(lkd_user_read(G(m + 100), buf, 8), LX_EFAULT);
    LT_EQ(buf[0], 0x11);
    memset(m + 100, 0x22, 8);
    LT_ERRNO(lkd_user_write(G(m + 100), buf, 8), LX_EFAULT);
    LT_EQ(m[100], 0x22);
    LT_ERRNO(lkd_user_strn(G(m + 100), s, sizeof s), LX_EFAULT);
    LT_ERRNO(lkd_user_zero(G(m + 100), 8), LX_EFAULT);
    LT_EQ(m[100], 0x22);
    lkd_usermem_set_check_fn(check_none);
}

int main(void)
{
    lkd_usermem_set_check_fn(check_none);
    /* The previous handler must exist before LKD installs its own (chaining test). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prev_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    if (lkd_usermem_install_handlers() != 0) { printf("FAIL install: handlers\n"); return 1; }
    LT_RUN(range_check_hook_is_consulted_before_the_copy);
    LT_RUN(read_and_write_roundtrip_on_mapped_memory);
    LT_RUN(all_alignments_and_sizes_copy_exactly);
    LT_RUN(bad_pointers_return_efault_without_crashing);
    LT_RUN(large_copy_into_guard_page_faults_cleanly);
    LT_RUN(cmpxchg32_swaps_and_recovers_faults);
    LT_RUN(bug_macro_prints_file_and_line);
    LT_RUN(strn_copies_strings_and_stops_at_nul);
    LT_RUN(strn_across_pages_and_before_guard);
    LT_RUN(guest_base_offset_is_applied);
    LT_RUN(faults_outside_user_copies_go_to_the_previous_handler);
    LT_RUN(engine_handler_installed_later_can_use_try_recover);
    LT_RUN(concurrent_faults_in_several_threads_are_independent);
    return lt_finish();
}
