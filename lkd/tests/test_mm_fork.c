/* test_mm_fork.c: the address space across a real host fork through lkd_fork_host (module hooks).
 * Linux references: fork(2) ("the child ... separate copies of ... memory", MAP_SHARED stays shared,
 * "memory locks (mlock(2), mlockall(2)) are not inherited"), madvise(2) MADV_DONTFORK ("not made
 * available to the child") and MADV_WIPEONFORK ("zero-filled memory in the child"), kernel/fork.c
 * dup_mmap of v6.12. */
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "ltest.h"
#include "fake_engine.h"
#include "../src/mm/mm_priv.h"
#include "../src/mm/lkd_mm_ext.h"
#include "../src/mm/lkd_mm_watch.h"
#include <pthread.h>

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

static int wait_child(pid_t p, int timeout_s)
{
    for (int i = 0; i < timeout_s * 100; i++) {
        int st;
        if (waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        usleep(10000);
    }
    kill(p, SIGKILL);
    waitpid(p, NULL, 0);
    return -1;
}

LT_TEST(fork_semantics)
{
    fake_engine_init();
    T = fake_make_task();
    T->mm = lkd_mm_create();
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
    int64_t priv = MMAP(0, 0x4000, RW, PA);
    int64_t shr = MMAP(0, 0x4000, RW, LX_MAP_SHARED | LX_MAP_ANONYMOUS);
    int64_t dfork = MMAP(0, 0x4000, RW, PA);
    int64_t wipe = MMAP(0, 0x4000, RW, PA);
    int64_t locked = MMAP(0, 0x4000, RW, PA | LX_MAP_LOCKED);
    LT_CHECK(priv > 0 && shr > 0 && dfork > 0 && wipe > 0 && locked > 0);
    memset(H(priv), 1, 0x4000);
    memset(H(shr), 2, 0x4000);
    memset(H(dfork), 3, 0x4000);
    memset(H(wipe), 4, 0x4000);
    LT_EQ(sc(LKD_NR_madvise, dfork, 0x4000, LX_MADV_DONTFORK, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_madvise, wipe, 0x4000, LX_MADV_WIPEONFORK, 0, 0, 0), 0);
    LT_EQ(sc(LKD_NR_mlockall, LX_MCL_FUTURE, 0, 0, 0, 0, 0), 0);
    uint64_t parent_id = lkd_mm_identity(T->mm);
    size_t nparent = T->mm->vt.n;

    fflush(NULL);
    pid_t p = lkd_fork_host(T, 0);
    if (p == 0) {
        alarm(10);
        int bad = 0;
        struct lkd_vma_info vi;
        if (lkd_mm_identity(T->mm) == parent_id) bad |= 1;                 /* re-identified */
        if (lkd_mm_find(T->mm, dfork, 0, &vi) == 0) bad |= 2;               /* DONTFORK absent */
        if (H(wipe)[0] != 0 || H(wipe)[0x3fff] != 0) bad |= 4;              /* WIPEONFORK zeroed */
        if (H(priv)[7] != 1) bad |= 8;                                      /* private copy */
        H(priv)[7] = 0x71;                                                  /* not seen by the parent */
        if (H(shr)[7] != 2) bad |= 16;
        H(shr)[7] = 0x72;                                                   /* seen by the parent */
        if (lkd_mm_find(T->mm, locked, 0, &vi) || (vi.flags & LX_MAP_LOCKED)) bad |= 32; /* locks not inherited */
        int64_t n = MMAP(0, 0x1000, RW, PA);                                /* mlockall(MCL_FUTURE) not inherited */
        if (n <= 0 || lkd_mm_find(T->mm, n, 0, &vi) || (vi.flags & LX_MAP_LOCKED)) bad |= 64;
        struct lkd_mm_stats st;
        if (lkd_mm_stats(T->mm, &st) || st.locked_vm) bad |= 128;
        _exit(bad);
    }
    LT_CHECK(p > 0);
    LT_EQ(wait_child(p, 15), 0);
    LT_EQ(H(priv)[7], 1);
    LT_EQ(H(shr)[7], 0x72);
    LT_EQ(H(dfork)[0], 3);
    LT_EQ(H(wipe)[0], 4);
    LT_EQ(lkd_mm_identity(T->mm), parent_id);
    LT_EQ(T->mm->vt.n, nparent);
    /* the parent's locks were released by fork_parent: the address space is still usable */
    int64_t after = MMAP(0, 0x1000, RW, PA);
    LT_CHECK(after > 0);
    LT_EQ(fake_fork_prepare_count, 1);
    LT_EQ(fake_fork_parent_count, 1);
    sc(LKD_NR_munlockall, 0, 0, 0, 0, 0, 0);
    lkd_mm_put(T->mm);
    T->mm = NULL;
}

LT_TEST(exec_reset)
{
    T->mm = lkd_mm_create();
    int64_t a = MMAP(0, 0x4000, RW, PA);
    lkd_mm_set_brk_start(T->mm, lkd_mm_window_guest_lo() + 0x1000000);
    LT_CHECK(a > 0);
    /* execve(2): "the ... memory mappings are not preserved" */
    lkd_mm_exec_commit(T);
    LT_EQ(T->mm->vt.n, 0);
    LT_EQ(T->mm->brk, 0);
    struct lkd_vma_info vi;
    LT_ERRNO(lkd_mm_find(T->mm, a, 0, &vi), LX_ENOENT);
    /* the window is reusable: the same address can be mapped again */
    LT_EQ(MMAP(a, 0x4000, RW, PA | LX_MAP_FIXED), a);
    LT_EQ(H(a)[0], 0);
    /* thread exit drops the thread's reference; the last one frees the address space */
    lkd_mm_task_exit(T);
    LT_CHECK(T->mm == NULL);
    T->mm = lkd_mm_create(); /* a new address space can be created once the old one is gone */
    LT_CHECK(T->mm != NULL);
    lkd_mm_put(T->mm);
    T->mm = NULL;
}

/* The watch lock is taken by fork_prepare, rebuilt by the child, and used there at once (mm_host_refresh,
 * mmap, munmap, a watched page). A hang here is the regression this guards (no unlock of a recursive
 * mutex by the child's thread). Watch state is also cleared in the child. */
static void child_uses_watch_lock(lkd_task *t, int *bad)
{
    int64_t w = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)w);
    if (lkd_mm_code_watched(hp)) *bad |= 1;
    if (lkd_mm_watch_code(hp, 16) != 0 || !lkd_mm_code_watched(hp)) *bad |= 2;
    if (sc(LKD_NR_mprotect, hp, 0x4000, RW, 0, 0, 0)) *bad |= 4;
    if (sc(LKD_NR_madvise, w, 0x8000, LX_MADV_DONTNEED, 0, 0, 0)) *bad |= 16; /* watched page keeps protection */
    if (sc(LKD_NR_munmap, hp, 0x4000, 0, 0, 0, 0) || lkd_mm_code_watched(hp)) *bad |= 8;
    (void)t;
}

static void *forker_thread(void *arg)
{
    int *result = arg;
    lkd_set_current(T);
    fflush(NULL);
    pid_t p = lkd_fork_host(T, 0);
    if (p == 0) {
        alarm(10);
        int bad = 0;
        child_uses_watch_lock(T, &bad);
        _exit(bad);
    }
    *result = p > 0 ? wait_child(p, 15) : -2;
    return NULL;
}

LT_TEST(fork_watch_lock_in_child)
{
    T->mm = lkd_mm_create();
    T->proc->rlimits[LX_RLIMIT_MEMLOCK].rlim_cur = LX_RLIM_INFINITY;
    int64_t a = MMAP(0, 0x8000, RW, PA);
    int64_t wipe = MMAP(0, 0x8000, RW, PA);
    uint64_t hp = HP_UP((uint64_t)a);
    LT_EQ(lkd_mm_watch_code(hp, 16), 0);
    LT_EQ(sc(LKD_NR_madvise, wipe, 0x8000, LX_MADV_WIPEONFORK, 0, 0, 0), 0);
    for (int round = 0; round < 3; round++) {          /* watch held, then fork, repeatedly */
        fflush(NULL);
        pid_t p = lkd_fork_host(T, 0);
        if (p == 0) {
            alarm(10);
            int bad = 0;
            child_uses_watch_lock(T, &bad);
            _exit(bad);
        }
        LT_CHECK(p > 0);
        LT_EQ(wait_child(p, 15), 0);                   /* -1 would be a hang */
    }
    LT_EQ(lkd_mm_code_watched(hp), 1);                  /* the parent keeps its watch and its lock works */
    /* fork from a secondary thread (the engine forks from guest worker threads) */
    int result = -3;
    pthread_t th;
    LT_EQ(pthread_create(&th, NULL, forker_thread, &result), 0);
    pthread_join(th, NULL);
    LT_EQ(result, 0);
    LT_EQ(lkd_mm_watch_code(hp + 0x4000, 16), 0);       /* the parent's locks are free again */
    lkd_mm_put(T->mm);
    T->mm = NULL;
}

/* A JIT thread keeps faulting on watched code (lkd_mm_code_write_fault takes the watch lock and the VMA
 * lock) while another thread forks (witness w18_forkjit). The child must be able to take every mm lock:
 * a lock the forking thread held across fork with a waiter queued in the kernel cannot be unlocked in the
 * child, it is rebuilt (lkd_fork_reinit_*). Before: the child blocked in mm_wlock. 20 forks, one child at
 * a time, each child mmaps, munmaps and watches code under a 10 s alarm. */
static _Atomic int churn_stop;
static uint64_t churn_page;
static void *churn_thread(void *arg)
{
    lkd_task *t = arg;
    lkd_set_current(t);
    while (!atomic_load(&churn_stop)) {
        (void)lkd_mm_watch_code(churn_page, 16);
        (void)lkd_mm_code_write_fault(churn_page + 8);
        (void)lkd_mm_check_range(churn_page, 8, 0);
    }
    return NULL;
}
LT_TEST(fork_while_a_thread_churns_the_mm_locks)
{
    T->mm = lkd_mm_create();
    int64_t a = MMAP(0, 0x10000, RW, PA);
    LT_CHECK(a > 0);
    churn_page = HP_UP((uint64_t)a);
    lkd_task *t2 = fake_make_thread(T);
    t2->mm = lkd_mm_get(T->mm);
    atomic_store(&churn_stop, 0);
    pthread_t th;
    LT_EQ(pthread_create(&th, NULL, churn_thread, t2), 0);
    int bad = 0;
    for (int round = 0; round < 20 && !bad; round++) {
        fflush(NULL);
        pid_t p = lkd_fork_host(T, 0);
        if (p == 0) {
            alarm(10);
            int64_t m = MMAP(0, 0x8000, RW, PA);
            int ok = m > 0 && sc(LKD_NR_munmap, (uint64_t)m, 0x8000, 0, 0, 0, 0) == 0 &&
                     lkd_mm_watch_code(churn_page, 16) == 0 && lkd_mm_code_write_fault(churn_page + 8) >= 0;
            _exit(ok ? 0 : 3);
        }
        if (p < 0 || wait_child(p, 15) != 0) bad = round + 1;
    }
    LT_EQ(bad, 0);
    atomic_store(&churn_stop, 1);
    pthread_join(th, NULL);
    lkd_mm_put(t2->mm);
    t2->mm = NULL;
    lkd_mm_put(T->mm);
    T->mm = NULL;
}

int main(void)
{
    LT_RUN(fork_semantics);
    LT_RUN(exec_reset);
    LT_RUN(fork_watch_lock_in_child);
    LT_RUN(fork_while_a_thread_churns_the_mm_locks);
    return lt_finish();
}
