/* test_sig_futex.c: futex(2), futex_waitv(2), futex_wake/futex_wait/futex_requeue (futex2), robust
 * lists, PI subset, interruption by signals, shared futexes across a fork. Linux references:
 * futex(2) man page, kernel/futex/{syscalls,waitwake,requeue,pi,core}.c at v6.12. */
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "futex.h"
#include "sig.h"

void lkd_futex_set_key_fn(int (*fn)(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key));

#define P(x) ((uint64_t)(uintptr_t)(x))
#define FPRIV LX_FUTEX_PRIVATE_FLAG
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    (((op & 0xf) << 28) | ((cmp & 0xf) << 24) | ((oparg & 0xfff) << 12) | (cmparg & 0xfff))

static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq r = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &r);
}
static int64_t fx(lkd_task *t, _Atomic uint32_t *w, int op, uint32_t val, uint64_t ts_or_val2, void *w2, uint32_t val3)
{
    return sc(t, LKD_NR_futex, P(w), (uint64_t)(uint32_t)op, val, ts_or_val2, P(w2), val3);
}

static lkd_task *g_main;

/* ---- waiter threads --------------------------------------------------------------------------- */
struct waiter {
    lkd_task *t;
    _Atomic uint32_t *word;
    uint32_t val;
    int op;
    uint32_t bitset;
    struct lx_timespec ts;
    int has_ts;
    _Atomic int started;
    _Atomic int done;
    int64_t ret;
    pthread_t th;
};
static void *waiter_main(void *p)
{
    struct waiter *w = p;
    lkd_set_current(w->t);
    atomic_store(&w->started, 1);
    w->ret = fx(w->t, w->word, w->op, w->val, w->has_ts ? P(&w->ts) : 0, NULL, w->bitset);
    atomic_store(&w->done, 1);
    return NULL;
}
static void start_waiter(struct waiter *w, lkd_task *leader, _Atomic uint32_t *word, uint32_t val, int op, uint32_t bitset)
{
    memset(w, 0, sizeof *w);
    w->t = fake_make_thread(leader);
    w->word = word;
    w->val = val;
    w->op = op;
    w->bitset = bitset;
    pthread_create(&w->th, NULL, waiter_main, w);
    while (!atomic_load(&w->started)) usleep(100);
}
static void settle(void) { usleep(60000); } /* lets started waiters reach their sleep */
static int count_done(struct waiter *w, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++) c += atomic_load(&w[i].done);
    return c;
}
static void wait_done(struct waiter *w, int n, int want)
{
    for (int i = 0; i < 3000 && count_done(w, n) < want; i++) usleep(1000);
}

/* FUTEX_WAIT: EAGAIN when the word differs, ETIMEDOUT after a relative timeout, EINVAL for a
 * misaligned word or bitset 0, ENOSYS for FUTEX_CLOCK_REALTIME with FUTEX_WAIT (do_futex), EFAULT for
 * an unreadable timeout. */
LT_TEST(wait_errors)
{
    lkd_task *t = g_main;
    static _Atomic uint32_t w = 5;
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 4, 0, NULL, 0), LX_EAGAIN);
    struct lx_timespec ts = { 0, 30000000 };
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 5, P(&ts), NULL, 0), LX_ETIMEDOUT);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 29000000 && dt < 400000000);
    LT_ERRNO(sc(t, LKD_NR_futex, P(&w) + 1, LX_FUTEX_WAIT, 5, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT_BITSET | FPRIV, 5, 0, NULL, 0), LX_EINVAL);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAKE_BITSET | FPRIV, 1, 0, NULL, 0), LX_EINVAL);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | LX_FUTEX_CLOCK_REALTIME, 5, P(&ts), NULL, 0), LX_ENOSYS);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 5, 8, NULL, 0), LX_EFAULT);
    struct lx_timespec bad = { 0, 1000000000 };
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 5, P(&bad), NULL, 0), LX_EINVAL);
    LT_ERRNO(fx(t, &w, 99, 5, 0, NULL, 0), LX_ENOSYS);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT_REQUEUE_PI, 5, 0, NULL, 0), LX_ENOSYS);
    LT_EQ(fx(t, &w, LX_FUTEX_WAKE | FPRIV, 10, 0, NULL, 0), 0); /* nobody waits */
}

/* FUTEX_WAKE wakes at most nr waiters and returns how many; nr = 0 still wakes one
 * (futex_wake: "if (++ret >= nr_wake) break"). */
LT_TEST(wait_wake_threads)
{
    static _Atomic uint32_t w = 0;
    struct waiter ws[8];
    for (int i = 0; i < 8; i++) start_waiter(&ws[i], g_main, &w, 0, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_EQ(count_done(ws, 8), 0);
    LT_EQ(fx(g_main, &w, LX_FUTEX_WAKE | FPRIV, 3, 0, NULL, 0), 3);
    wait_done(ws, 8, 3);
    LT_EQ(count_done(ws, 8), 3);
    LT_EQ(fx(g_main, &w, LX_FUTEX_WAKE | FPRIV, 0, 0, NULL, 0), 1);
    wait_done(ws, 8, 4);
    LT_EQ(count_done(ws, 8), 4);
    LT_EQ(fx(g_main, &w, LX_FUTEX_WAKE | FPRIV, 0x7fffffff, 0, NULL, 0), 4);
    wait_done(ws, 8, 8);
    for (int i = 0; i < 8; i++) { pthread_join(ws[i].th, NULL); LT_EQ(ws[i].ret, 0); }
}

/* FUTEX_WAIT_BITSET/FUTEX_WAKE_BITSET: a wake only reaches waiters whose bitset intersects; the
 * absolute timeout is on CLOCK_MONOTONIC unless FUTEX_CLOCK_REALTIME. */
LT_TEST(bitset_and_absolute_timeouts)
{
    static _Atomic uint32_t w = 1;
    struct waiter a;
    start_waiter(&a, g_main, &w, 1, LX_FUTEX_WAIT_BITSET | FPRIV, 0x1);
    settle();
    LT_EQ(fx(g_main, &w, LX_FUTEX_WAKE_BITSET | FPRIV, 1, 0, NULL, 0x2), 0);
    LT_EQ(fx(g_main, &w, LX_FUTEX_WAKE_BITSET | FPRIV, 1, 0, NULL, 0x3), 1);
    pthread_join(a.th, NULL);
    LT_EQ(a.ret, 0);
    /* the deadline is on the guest CLOCK_MONOTONIC as clock_gettime(2) gives it to the guest (task module,
     * Darwin CLOCK_UPTIME_RAW), request sig-2 point 3 */
    struct timespec now;
    int64_t gm = lkd_guest_clock_ns(LX_CLOCK_MONOTONIC);
    now.tv_sec = gm / 1000000000; now.tv_nsec = gm % 1000000000;
    struct lx_timespec abs = { now.tv_sec, now.tv_nsec + 30000000 };
    if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(g_main, &w, LX_FUTEX_WAIT_BITSET | FPRIV, 1, P(&abs), NULL, LX_FUTEX_BITSET_MATCH_ANY), LX_ETIMEDOUT);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 25000000);
    LT_CHECK(lkd_host_mono_ns() - t0 < 1000000000);
    clock_gettime(CLOCK_REALTIME, &now);
    struct lx_timespec rabs = { now.tv_sec, now.tv_nsec + 30000000 };
    if (rabs.tv_nsec >= 1000000000) { rabs.tv_sec++; rabs.tv_nsec -= 1000000000; }
    t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(g_main, &w, LX_FUTEX_WAIT_BITSET | FPRIV | LX_FUTEX_CLOCK_REALTIME, 1, P(&rabs), NULL, LX_FUTEX_BITSET_MATCH_ANY), LX_ETIMEDOUT);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 25000000);
    /* a deadline in the past times out at once after the value check */
    struct lx_timespec past = { 1, 0 };
    LT_ERRNO(fx(g_main, &w, LX_FUTEX_WAIT_BITSET | FPRIV, 1, P(&past), NULL, LX_FUTEX_BITSET_MATCH_ANY), LX_ETIMEDOUT);
    LT_ERRNO(fx(g_main, &w, LX_FUTEX_WAIT_BITSET | FPRIV, 2, P(&past), NULL, LX_FUTEX_BITSET_MATCH_ANY), LX_EAGAIN);
}

/* FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: wake nr_wake, move up to nr_requeue to uaddr2, return the total;
 * CMP_REQUEUE fails with EAGAIN when *uaddr != val3; negative counts are EINVAL. */
LT_TEST(requeue)
{
    static _Atomic uint32_t a = 0, b = 0;
    struct waiter ws[4];
    for (int i = 0; i < 4; i++) start_waiter(&ws[i], g_main, &a, 0, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_ERRNO(fx(g_main, &a, LX_FUTEX_CMP_REQUEUE | FPRIV, 1, 2, (void *)&b, 7), LX_EAGAIN);
    LT_ERRNO(fx(g_main, &a, LX_FUTEX_CMP_REQUEUE | FPRIV, (uint32_t)-1, 2, (void *)&b, 0), LX_EINVAL);
    LT_EQ(fx(g_main, &a, LX_FUTEX_CMP_REQUEUE | FPRIV, 1, 2, (void *)&b, 0), 3);
    wait_done(ws, 4, 1);
    LT_EQ(count_done(ws, 4), 1);
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE | FPRIV, 10, 0, NULL, 0), 1);  /* one left on a */
    LT_EQ(fx(g_main, &b, LX_FUTEX_WAKE | FPRIV, 10, 0, NULL, 0), 2);  /* two moved to b */
    wait_done(ws, 4, 4);
    for (int i = 0; i < 4; i++) { pthread_join(ws[i].th, NULL); LT_EQ(ws[i].ret, 0); }
    /* plain REQUEUE does not compare */
    start_waiter(&ws[0], g_main, &a, 0, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_EQ(fx(g_main, &a, LX_FUTEX_REQUEUE | FPRIV, 0, 1, (void *)&b, 0), 1);
    LT_EQ(fx(g_main, &b, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0), 1);
    pthread_join(ws[0].th, NULL);
    LT_EQ(ws[0].ret, 0);
}

/* FUTEX_WAKE_OP: atomically apply op to *uaddr2, wake uaddr, and wake uaddr2 if the comparison of the
 * OLD value holds (futex_atomic_op_inuser); OPARG_SHIFT uses 1 << oparg; unknown op ENOSYS. */
LT_TEST(wake_op)
{
    static _Atomic uint32_t a = 0, b = 0;
    struct waiter wa, wb;
    start_waiter(&wa, g_main, &a, 0, LX_FUTEX_WAIT | FPRIV, 0);
    start_waiter(&wb, g_main, &b, 0, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(0, 5, 0, 0)), 2); /* SET 5, old == 0 */
    LT_EQ(atomic_load(&b), 5);
    pthread_join(wa.th, NULL);
    pthread_join(wb.th, NULL);
    LT_EQ(wa.ret, 0);
    LT_EQ(wb.ret, 0);
    start_waiter(&wb, g_main, &b, 5, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(1, 3, 1, 5)), 0); /* ADD 3, old != 5 false */
    LT_EQ(atomic_load(&b), 8);
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(8 | 2, 4, 4, 1)), 1); /* OR 1<<4, old > 1 */
    LT_EQ(atomic_load(&b), 24);
    pthread_join(wb.th, NULL);
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(3, 8, 0, 0)), 0); /* ANDN 8 */
    LT_EQ(atomic_load(&b), 16);
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(4, 0xfff, 0, 0)), 0); /* XOR -1 */
    LT_EQ(atomic_load(&b), (uint32_t)~16u);
    LT_ERRNO(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(5, 1, 0, 0)), LX_ENOSYS);
    LT_ERRNO(fx(g_main, &a, LX_FUTEX_WAKE_OP | FPRIV, 1, 1, (void *)&b, FUTEX_OP(0, 1, 9, 0)), LX_ENOSYS);
    LT_EQ(atomic_load(&b), 1); /* the op is applied before the comparison is rejected */
}

/* PI futexes, ownership protocol (futex(2) "Priority-inheritance futexes"): LOCK_PI on 0 stores the
 * tid; relocking is EDEADLK; TRYLOCK on a held lock EWOULDBLOCK; UNLOCK_PI by a non-owner EPERM;
 * unlock with a waiter hands the lock over (word = new tid | FUTEX_WAITERS); a dead owner tid is ESRCH;
 * FUTEX_WAKE on a PI waiter is EINVAL. */
LT_TEST(pi_lock_protocol)
{
    static _Atomic uint32_t w = 0;
    lkd_task *t = g_main;
    struct lx_timespec ts;
    LT_EQ(fx(t, &w, LX_FUTEX_LOCK_PI | FPRIV, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&w), (uint32_t)t->tid);
    LT_ERRNO(fx(t, &w, LX_FUTEX_LOCK_PI | FPRIV, 0, 0, NULL, 0), LX_EDEADLK);
    struct waiter o;
    start_waiter(&o, t, &w, 0, LX_FUTEX_TRYLOCK_PI | FPRIV, 0);
    pthread_join(o.th, NULL);
    LT_ERRNO(o.ret, LX_EWOULDBLOCK);
    LT_ERRNO(fx(o.t, &w, LX_FUTEX_UNLOCK_PI | FPRIV, 0, 0, NULL, 0), LX_EPERM);
    /* a contender blocks, the owner unlocks, ownership moves */
    start_waiter(&o, t, &w, 0, LX_FUTEX_LOCK_PI | FPRIV, 0);
    settle();
    LT_EQ(atomic_load(&o.done), 0);
    LT_EQ(atomic_load(&w), (uint32_t)t->tid | LX_FUTEX_WAITERS);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0), LX_EINVAL);
    LT_EQ(fx(t, &w, LX_FUTEX_UNLOCK_PI | FPRIV, 0, 0, NULL, 0), 0);
    pthread_join(o.th, NULL);
    LT_EQ(o.ret, 0);
    LT_EQ(atomic_load(&w), (uint32_t)o.t->tid | LX_FUTEX_WAITERS);
    LT_EQ(fx(o.t, &w, LX_FUTEX_UNLOCK_PI | FPRIV, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&w), 0);
    /* a tid that does not exist holds the word: ESRCH (handle_exit_race) */
    atomic_store(&w, 0x3ffff000u);
    LT_ERRNO(fx(t, &w, LX_FUTEX_LOCK_PI | FPRIV, 0, 0, NULL, 0), LX_ESRCH);
    /* LOCK_PI with an absolute CLOCK_REALTIME timeout while held by a live thread */
    atomic_store(&w, (uint32_t)o.t->tid);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ts.tv_sec = now.tv_sec;
    ts.tv_nsec = now.tv_nsec + 30000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    LT_ERRNO(fx(t, &w, LX_FUTEX_LOCK_PI | FPRIV, 0, P(&ts), NULL, 0), LX_ETIMEDOUT);
    LT_EQ(atomic_load(&w), (uint32_t)o.t->tid | LX_FUTEX_WAITERS);
    atomic_store(&w, LX_FUTEX_OWNER_DIED);
    LT_EQ(fx(t, &w, LX_FUTEX_TRYLOCK_PI | FPRIV, 0, 0, NULL, 0), 0); /* keeps OWNER_DIED */
    LT_EQ(atomic_load(&w), (uint32_t)t->tid | LX_FUTEX_OWNER_DIED);
    LT_EQ(fx(t, &w, LX_FUTEX_UNLOCK_PI | FPRIV, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&w), 0);
}

/* A futex wait is interrupted by a signal: -ERESTARTSYS without timeout (restarted with SA_RESTART,
 * EINTR otherwise), -ERESTART_RESTARTBLOCK with a timeout, whose restart_syscall continues to the
 * same deadline (futex_wait_restart). */
struct intr_arg { lkd_task *target; int sig; int delay_ms; };
static void *intr_sender(void *p)
{
    struct intr_arg *a = p;
    usleep((useconds_t)a->delay_ms * 1000);
    lkd_sig_send_tid(a->target, a->target->proc->pid, a->target->tid, a->sig, NULL);
    return NULL;
}
LT_TEST(interrupted_by_signal)
{
    lkd_task *t = fake_make_task();
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER, 0x2000, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&h), 0, 8, 0, 0), 0);
    static _Atomic uint32_t w = 0;
    pthread_t th;
    struct intr_arg a = { t, LX_SIGUSR1, 30 };
    pthread_create(&th, NULL, intr_sender, &a);
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 0, 0, NULL, 0), LX_ERESTARTSYS);
    pthread_join(th, NULL);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTARTSYS), LKD_RESTART_NONE);
    LT_EQ(lkd_signal_next(t), 1);
    lkd_sig_set_mask(t, 0); /* the handler returned (rt_sigreturn restores the mask) */
    /* with a relative timeout: restart block with the absolute deadline */
    struct lx_timespec ts = { 0, 300000000 };
    pthread_create(&th, NULL, intr_sender, &a);
    int64_t t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(t, &w, LX_FUTEX_WAIT | FPRIV, 0, P(&ts), NULL, 0), LX_ERESTART_RESTARTBLOCK);
    pthread_join(th, NULL);
    LT_CHECK(t->restart.fn != NULL);
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTART_RESTARTBLOCK), LKD_RESTART_NONE); /* handler: EINTR */
    lkd_sig_set_mask(t, LX_SIGBIT(LX_SIGUSR1)); /* pretend no handler runs: the wait resumes */
    LT_EQ(lkd_sig_restart_decision(t, LX_ERESTART_RESTARTBLOCK), LKD_RESTART_BLOCK);
    lkd_sysreq r = { LKD_NR_restart_syscall, { 0 } };
    int64_t rax;
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_ETIMEDOUT);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 290000000 && dt < 900000000); /* the original deadline, not a new 300 ms */
    lkd_sig_set_mask(t, 0);
    LT_EQ(lkd_signal_next(t), 1);
}

/* futex_waitv(2): returns the index of the woken futex, EAGAIN when a value differs, EINVAL for
 * flags, count, reserved field, futex size; timeout absolute on the given clock. */
LT_TEST(waitv)
{
    static _Atomic uint32_t a = 1, b = 2;
    struct lx_futex_waitv v[2] = { { 1, P(&a), 2 | FPRIV, 0 }, { 2, P(&b), 2 | FPRIV, 0 } };
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    struct lx_timespec abs = { now.tv_sec + 5, now.tv_nsec };
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 1, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 0, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 129, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, 0, 1, 0, 0, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 0, P(&abs), 7, 0), LX_EINVAL);
    v[1].flags = 3 | FPRIV;
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 0, 0, 0, 0), LX_EINVAL);
    v[1].flags = 2 | FPRIV;
    v[1]._reserved = 1;
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 0, 0, 0, 0), LX_EINVAL);
    v[1]._reserved = 0;
    v[1].val = 3;
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 0, 0, 0, 0), LX_EAGAIN);
    v[1].val = 2;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    struct lx_timespec soon = { now.tv_sec, now.tv_nsec + 20000000 };
    if (soon.tv_nsec >= 1000000000) { soon.tv_sec++; soon.tv_nsec -= 1000000000; }
    LT_ERRNO(sc(g_main, LKD_NR_futex_waitv, P(v), 2, 0, P(&soon), LX_CLOCK_MONOTONIC, 0), LX_ETIMEDOUT);
}

struct wv_thread { lkd_task *t; struct lx_futex_waitv *v; int n; int64_t ret; _Atomic int done; };
static void *wv_main(void *p)
{
    struct wv_thread *w = p;
    lkd_set_current(w->t);
    w->ret = sc(w->t, LKD_NR_futex_waitv, P(w->v), (uint64_t)w->n, 0, 0, 0, 0);
    atomic_store(&w->done, 1);
    return NULL;
}
LT_TEST(waitv_wakes_index)
{
    static _Atomic uint32_t a = 1, b = 2, c = 3;
    struct lx_futex_waitv v[3] = { { 1, P(&a), 2 | FPRIV, 0 }, { 2, P(&b), 2 | FPRIV, 0 }, { 3, P(&c), 2 | FPRIV, 0 } };
    struct wv_thread w = { fake_make_thread(g_main), v, 3, -1, 0 };
    pthread_t th;
    pthread_create(&th, NULL, wv_main, &w);
    settle();
    LT_EQ(atomic_load(&w.done), 0);
    LT_EQ(fx(g_main, &c, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0), 1);
    pthread_join(th, NULL);
    LT_EQ(w.ret, 2);
    /* all entries were removed: nothing left to wake */
    LT_EQ(fx(g_main, &a, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0), 0);
    LT_EQ(fx(g_main, &b, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0), 0);
}

/* futex_wake(2), futex_wait(2), futex_requeue(2) (futex2 calls 454-456): FUTEX2_SIZE_U32 required,
 * NUMA and unknown flags EINVAL, values above 32 bits EINVAL, futex_wake with nr 0 returns 0
 * (FLAGS_STRICT), futex_requeue takes two futex_waitv. */
LT_TEST(futex2_calls)
{
    static _Atomic uint32_t a = 4, b = 0;
    LT_ERRNO(sc(g_main, LKD_NR_futex_wake, P(&a), 1, 1, 3, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_wake, P(&a), 1, 1, 2 | 4, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_wake, P(&a), 1ull << 32, 1, 2, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_wait, P(&a), 1ull << 32, 1, 2, 0, 0), LX_EINVAL);
    LT_ERRNO(sc(g_main, LKD_NR_futex_wait, P(&a), 3, 1, 2 | FPRIV, 0, 0), LX_EAGAIN);
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    struct lx_timespec soon = { now.tv_sec, now.tv_nsec + 20000000 };
    if (soon.tv_nsec >= 1000000000) { soon.tv_sec++; soon.tv_nsec -= 1000000000; }
    LT_ERRNO(sc(g_main, LKD_NR_futex_wait, P(&a), 4, 0xffffffff, 2 | FPRIV, P(&soon), LX_CLOCK_REALTIME), LX_ETIMEDOUT);
    LT_ERRNO(sc(g_main, LKD_NR_futex_wait, P(&a), 4, 0xffffffff, 2 | FPRIV, P(&soon), 5), LX_EINVAL);
    struct waiter w;
    start_waiter(&w, g_main, &a, 4, LX_FUTEX_WAIT | FPRIV, 0);
    settle();
    LT_EQ(sc(g_main, LKD_NR_futex_wake, P(&a), 0xffffffff, 0, 2 | FPRIV, 0, 0), 0);
    struct lx_futex_waitv rv[2] = { { 4, P(&a), 2 | FPRIV, 0 }, { 0, P(&b), 2 | FPRIV, 0 } };
    LT_ERRNO(sc(g_main, LKD_NR_futex_requeue, P(rv), 1, 0, 1, 0, 0), LX_EINVAL);
    LT_EQ(sc(g_main, LKD_NR_futex_requeue, P(rv), 0, 0, 1, 0, 0), 1);
    LT_EQ(sc(g_main, LKD_NR_futex_wake, P(&b), 0xffffffff, 5, 2 | FPRIV, 0, 0), 1);
    pthread_join(w.th, NULL);
    LT_EQ(w.ret, 0);
}

/* Robust list at thread exit (handle_futex_death, exit_robust_list): a held futex gets
 * FUTEX_OWNER_DIED with its WAITERS bit and one waiter is woken; entries owned by another tid are left;
 * list_op_pending with a zero word only wakes; a cyclic list stops at ROBUST_LIST_LIMIT. */
struct rlock { uint64_t next; uint32_t word; uint32_t pad; };
LT_TEST(robust_list_owner_died)
{
    lkd_task *t = fake_make_thread(g_main);
    static struct lx_robust_list_head head;
    static struct rlock l1, l2, l3;
    head.list = P(&l1);
    head.futex_offset = (int64_t)offsetof(struct rlock, word);
    l1.next = P(&l2);
    l2.next = P(&head);
    l1.word = (uint32_t)t->tid | LX_FUTEX_WAITERS;
    l2.word = 12345;
    l3.word = 0;
    head.list_op_pending = P(&l3);
    t->robust_list_head = P(&head);
    t->robust_list_len = sizeof head;
    struct waiter w, w3;
    start_waiter(&w, g_main, (_Atomic uint32_t *)&l1.word, l1.word, LX_FUTEX_WAIT, 0);
    start_waiter(&w3, g_main, (_Atomic uint32_t *)&l3.word, 0, LX_FUTEX_WAIT, 0);
    settle();
    lkd_futex_exit_robust_list(t);
    pthread_join(w.th, NULL);
    pthread_join(w3.th, NULL);
    LT_EQ(w.ret, 0);
    LT_EQ(w3.ret, 0);
    LT_EQ(l1.word, LX_FUTEX_WAITERS | LX_FUTEX_OWNER_DIED);
    LT_EQ(l2.word, 12345);
    /* a cycle that never returns to the head ends at the limit */
    l1.next = P(&l2);
    l2.next = P(&l1);
    l1.word = (uint32_t)t->tid;
    head.list_op_pending = 0;
    lkd_futex_exit_robust_list(t);
    LT_EQ(l1.word, LX_FUTEX_OWNER_DIED);
    /* lkd_futex_wake_addr (CLONE_CHILD_CLEARTID) */
    static _Atomic uint32_t tidw = 77;
    start_waiter(&w, g_main, &tidw, 77, LX_FUTEX_WAIT, 0);
    settle();
    atomic_store(&tidw, 0);
    LT_EQ(lkd_futex_wake_addr(t, P(&tidw), 1, 0), 1);
    pthread_join(w.th, NULL);
    LT_EQ(w.ret, 0);
}

/* Many threads using a futex-based mutex: no lost wakeups (Drepper, "Futexes Are Tricky", mutex 2). */
static _Atomic uint32_t mtx;
static int64_t counter;
static _Atomic int64_t nwaits;
struct cont { lkd_task *t; };
static void lock_m(lkd_task *t)
{
    uint32_t c = 0;
    if (atomic_compare_exchange_strong(&mtx, &c, 1)) return;
    if (c != 2) c = atomic_exchange(&mtx, 2);
    while (c != 0) {
        atomic_fetch_add(&nwaits, 1);
        fx(t, &mtx, LX_FUTEX_WAIT | FPRIV, 2, 0, NULL, 0);
        c = atomic_exchange(&mtx, 2);
    }
}
static void unlock_m(lkd_task *t)
{
    if (atomic_fetch_sub(&mtx, 1) != 1) {
        atomic_store(&mtx, 0);
        fx(t, &mtx, LX_FUTEX_WAKE | FPRIV, 1, 0, NULL, 0);
    }
}
static void *cont_main(void *p)
{
    struct cont *c = p;
    lkd_set_current(c->t);
    for (int i = 0; i < 3000; i++) {
        lock_m(c->t);
        counter++;
        if ((i & 7) == 0) sched_yield(); /* hold the lock across a reschedule: real contention */
        unlock_m(c->t);
    }
    return NULL;
}
LT_TEST(mutex_contention)
{
    struct cont cs[16];
    pthread_t th[16];
    int64_t t0 = lkd_host_mono_ns();
    for (int i = 0; i < 16; i++) { cs[i].t = fake_make_thread(g_main); pthread_create(&th[i], NULL, cont_main, &cs[i]); }
    for (int i = 0; i < 16; i++) pthread_join(th[i], NULL);
    LT_EQ(counter, 48000);
    LT_EQ(atomic_load(&mtx), 0);
    printf("MEASURE futex mutex 16 threads x 3000: %.1f ms, %lld FUTEX_WAIT calls\n", (lkd_host_mono_ns() - t0) / 1e6,
           (long long)atomic_load(&nwaits));
}

/* Shared futexes across processes: a key of kind SHARED (as mm returns for MAP_SHARED memory) is one
 * queue in every process of the family (get_futex_key on a shared mapping). The test key function
 * maps the shared page to a SHARED key. */
static uint64_t shm_base;
static int shared_key(lkd_task *t, uint64_t gva, int ph, struct lkd_futex_key *k)
{
    if (gva >= shm_base && gva < shm_base + 16384) {
        k->kind = LKD_FK_SHARED;
        k->a = 0x5eed;
        k->b = gva - shm_base;
        return 0;
    }
    return lkd_mm_futex_key(t, gva, ph, k);
}
static void *trap_waker(void *p)
{
    lkd_task *t = p;
    usleep(20000);
    atomic_fetch_or(&t->flags, LKD_TF_TRACE_STOP);   /* what ptrace does for PTRACE_INTERRUPT */
    lkd_task_wake(t);
    return NULL;
}
LT_TEST(shared_futex_across_fork)
{
    _Atomic uint32_t *pg = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    LT_CHECK(pg != MAP_FAILED);
    shm_base = P(pg);
    lkd_futex_set_key_fn(shared_key);
    lkd_task *t = fake_make_task();
    atomic_store(&pg[0], 0);
    atomic_store(&pg[1], 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(t, 0);
    if (c == 0) {
        alarm(20);
        usleep(80000);
        atomic_store(&pg[0], 1);
        int64_t r = fx(t, &pg[0], LX_FUTEX_WAKE, 1, 0, NULL, 0);
        /* now wait on pg[1] for the parent */
        int64_t r2 = fx(t, &pg[1], LX_FUTEX_WAIT, 0, 0, NULL, 0);
        _exit(r == 1 && r2 == 0 ? 0 : 10 + (int)r);
    }
    LT_CHECK(c > 0);
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(fx(t, &pg[0], LX_FUTEX_WAIT, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&pg[0]), 1);
    LT_CHECK(lkd_host_mono_ns() - t0 < 2000000000ll);
    int woken = 0;
    for (int i = 0; i < 200 && !woken; i++) {
        usleep(10000);
        atomic_store(&pg[1], 1);
        woken = (int)fx(t, &pg[1], LX_FUTEX_WAKE, 1, 0, NULL, 0);
    }
    LT_EQ(woken, 1);
    int st = 0;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* a signal ends a shared wait at once (the sleeper is rung, no 50 ms slice) */
    struct lx_sigaction h = { 0x6000, LX_SA_RESTORER, 0x2000, 0 };
    LT_EQ(sc(t, LKD_NR_rt_sigaction, LX_SIGUSR1, P(&h), 0, 8, 0, 0), 0);
    atomic_store(&pg[2], 0);
    pthread_t th;
    struct intr_arg a = { t, LX_SIGUSR1, 20 };
    pthread_create(&th, NULL, intr_sender, &a);
    t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(t, &pg[2], LX_FUTEX_WAIT, 0, 0, NULL, 0), LX_ERESTARTSYS);
    int64_t dt = lkd_host_mono_ns() - t0;
    pthread_join(th, NULL);
    LT_CHECK(dt < 45000000);
    printf("MEASURE shared futex wait interrupted after %.2f ms (signal sent at 20 ms)\n", dt / 1e6);
    LT_EQ(lkd_signal_next(t), 1);
    /* a wake from another module (ptrace INTERRUPT, task) ends a shared wait at once too: lkd_task_wake
     * rings the sleeper through lkd_sig_wake_hook (request sig-1); before, it waited for the 50 ms slice */
    atomic_store(&pg[4], 0);
    pthread_create(&th, NULL, trap_waker, t);
    t0 = lkd_host_mono_ns();
    LT_ERRNO(fx(t, &pg[4], LX_FUTEX_WAIT, 0, 0, NULL, 0), LX_ERESTARTSYS);
    dt = lkd_host_mono_ns() - t0;
    pthread_join(th, NULL);
    atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP);
    LT_CHECK(dt < 40000000);
    printf("MEASURE shared futex wait ended by lkd_task_wake after %.2f ms (wake at 20 ms)\n", dt / 1e6);
    /* PI on a shared word: handed over across the table too */
    atomic_store(&pg[3], 0);
    LT_EQ(fx(t, &pg[3], LX_FUTEX_LOCK_PI, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&pg[3]), (uint32_t)t->tid);
    LT_EQ(fx(t, &pg[3], LX_FUTEX_UNLOCK_PI, 0, 0, NULL, 0), 0);
    LT_EQ(atomic_load(&pg[3]), 0);
    lkd_futex_set_key_fn(NULL);
    munmap((void *)pg, 16384);
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    g_main = fake_make_task();
    LT_RUN(wait_errors);
    LT_RUN(wait_wake_threads);
    LT_RUN(bitset_and_absolute_timeouts);
    LT_RUN(requeue);
    LT_RUN(wake_op);
    LT_RUN(pi_lock_protocol);
    LT_RUN(interrupted_by_signal);
    LT_RUN(waitv);
    LT_RUN(waitv_wakes_index);
    LT_RUN(futex2_calls);
    LT_RUN(robust_list_owner_died);
    LT_RUN(mutex_contention);
    LT_RUN(shared_futex_across_fork);
    return lt_finish();
}
