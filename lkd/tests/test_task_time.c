/* test_task_time.c: clocks, sleeps and interval timers of the task module. Models
 * kernel/time/posix-timers.c, posix-cpu-timers.c, hrtimer.c (nanosleep restart rules), itimer.c and
 * time.c of Linux v6.12. Durations are checked with wide margins (the machine may be loaded). */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "task.h"
#include "sig.h"

static int allow_all(uint64_t gva, size_t len, int w) { (void)gva; (void)len; (void)w; return 0; }

static lkd_task *T;

static int64_t sc6(unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, 0, 0 } };
    return lkd_dispatch(T, &rq);
}
#define SC0(n) sc6(LKD_NR_##n, 0, 0, 0, 0)
#define SC1(n, a) sc6(LKD_NR_##n, (uint64_t)(a), 0, 0, 0)
#define SC2(n, a, b) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), 0, 0)
#define SC3(n, a, b, c) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0)
#define SC4(n, a, b, c, d) sc6(LKD_NR_##n, (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d))
#define P(x) ((uint64_t)(uintptr_t)(x))

/* Pending signals of T as the sig module reports them (thread set or process set). */
static uint64_t pend(void)
{
    struct lkd_sig_status ss;
    memset(&ss, 0, sizeof ss);
    lkd_sig_status(T, &ss);
    return ss.pnd | ss.shdpnd | atomic_load(&T->pending.bits);
}

static int64_t now_ns(int clk)
{
    struct lx_timespec ts;
    if (SC2(clock_gettime, clk, P(&ts)) != 0) return -1;
    return ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* clock_gettime(2): every static clock id of v6.12 except 10 (removed CLOCK_SGI_CYCLE) works. */
LT_TEST(clock_ids)
{
    int ok[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11 };
    struct lx_timespec ts;
    for (unsigned i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        ts.tv_nsec = -1;
        LT_EQ(SC2(clock_gettime, ok[i], P(&ts)), 0);
        LT_CHECK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);
    }
    LT_ERRNO(SC2(clock_gettime, 10, P(&ts)), LX_EINVAL);
    LT_ERRNO(SC2(clock_gettime, 12, P(&ts)), LX_EINVAL);
    LT_ERRNO(SC2(clock_gettime, LX_CLOCK_MONOTONIC, 8), LX_EFAULT);
}

/* Semantics of the mapping: REALTIME is wall time, MONOTONIC never goes back, BOOTTIME >= MONOTONIC
 * (it also counts suspend), TAI == REALTIME (offset 0 until set). */
LT_TEST(clock_semantics)
{
    int64_t rt = now_ns(LX_CLOCK_REALTIME);
    LT_CHECK(rt / 1000000000ll - (int64_t)time(NULL) <= 1 && (int64_t)time(NULL) - rt / 1000000000ll <= 1);
    int64_t m1 = now_ns(LX_CLOCK_MONOTONIC), m2 = now_ns(LX_CLOCK_MONOTONIC);
    LT_CHECK(m2 >= m1);
    int64_t boot = now_ns(LX_CLOCK_BOOTTIME), mono = now_ns(LX_CLOCK_MONOTONIC);
    LT_CHECK(boot + 1000000 >= mono);
    int64_t tai = now_ns(LX_CLOCK_TAI), rt2 = now_ns(LX_CLOCK_REALTIME);
    LT_CHECK(rt2 - tai < 50000000 && rt2 >= tai);
    int64_t c1 = now_ns(LX_CLOCK_PROCESS_CPUTIME_ID), th1 = now_ns(LX_CLOCK_THREAD_CPUTIME_ID);
    volatile uint64_t x = 0;
    for (int i = 0; i < 30000000; i++) x += (uint64_t)i;
    int64_t c2 = now_ns(LX_CLOCK_PROCESS_CPUTIME_ID), th2 = now_ns(LX_CLOCK_THREAD_CPUTIME_ID);
    LT_CHECK(c2 - c1 > 1000000);
    LT_CHECK(th2 - th1 > 1000000);
    LT_CHECK(c2 >= th2 - 1000000);
}

/* Dynamic CPU clocks (posix-cpu-timers.c): ~pid << 3 | which, bit 2 = per thread, which 3 = fd clock. */
LT_TEST(cpu_clock_ids)
{
    int self_proc_sched = (int)((~0u << 3) | 2), self_thread_sched = (int)((~0u << 3) | 4 | 2);
    int self_proc_prof = (int)((~0u << 3) | 0), self_thread_virt = (int)((~0u << 3) | 4 | 1);
    int pid_proc_sched = (int)((~(uint32_t)T->proc->pid << 3) | 2);
    struct lx_timespec ts;
    LT_EQ(SC2(clock_gettime, self_proc_sched, P(&ts)), 0);
    LT_CHECK(ts.tv_sec > 0 || ts.tv_nsec > 0);
    LT_EQ(SC2(clock_gettime, self_thread_sched, P(&ts)), 0);
    LT_EQ(SC2(clock_gettime, self_proc_prof, P(&ts)), 0);
    LT_EQ(SC2(clock_gettime, self_thread_virt, P(&ts)), 0);
    LT_EQ(SC2(clock_gettime, pid_proc_sched, P(&ts)), 0);
    int fdclock = (int)((~3u << 3) | 3);
    LT_ERRNO(SC2(clock_gettime, fdclock, P(&ts)), LX_EINVAL);
    LT_ERRNO(SC2(clock_gettime, (int)((~0x3ffffff0u << 3) | 2), P(&ts)), LX_EINVAL);
    LT_EQ(SC2(clock_getres, self_proc_sched, P(&ts)), 0);
    LT_EQ(ts.tv_nsec, 1);
    LT_EQ(SC2(clock_getres, self_proc_prof, P(&ts)), 0);
    LT_EQ(ts.tv_nsec, 4000000);                      /* one tick at HZ=250 */
}

/* clock_getres(2): a NULL pointer only validates the id. */
LT_TEST(clock_getres_values)
{
    struct lx_timespec ts = { 9, 9 };
    LT_EQ(SC2(clock_getres, LX_CLOCK_REALTIME, P(&ts)), 0);
    LT_EQ(ts.tv_sec, 0);
    LT_CHECK(ts.tv_nsec >= 1 && ts.tv_nsec <= 1000);
    LT_EQ(SC2(clock_getres, LX_CLOCK_MONOTONIC, 0), 0);
    LT_ERRNO(SC2(clock_getres, 10, P(&ts)), LX_EINVAL);
}

/* gettimeofday(2), time(2): wall time, zero timezone; settimeofday/clock_settime need CAP_SYS_TIME
 * after argument validation (do_sys_settimeofday64). */
LT_TEST(wall_time_and_setting)
{
    struct lx_timeval tv;
    struct lx_timezone tz = { 5, 5 };
    LT_EQ(SC2(gettimeofday, P(&tv), P(&tz)), 0);
    LT_CHECK(tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    LT_EQ(tz.tz_minuteswest, 0);
    LT_EQ(tz.tz_dsttime, 0);
    int64_t s = 0;
    int64_t r = SC1(time, P(&s));
    LT_EQ(r, s);
    LT_CHECK(r - tv.tv_sec <= 1);
    LT_ERRNO(SC2(settimeofday, P(&tv), 0), LX_EPERM);
    tv.tv_usec = 1000000;
    LT_ERRNO(SC2(settimeofday, P(&tv), 0), LX_EINVAL);
    LT_ERRNO(SC2(settimeofday, 0, 0), LX_EPERM);
    struct lx_timespec ts = { 1, 0 };
    LT_ERRNO(SC2(clock_settime, LX_CLOCK_REALTIME, P(&ts)), LX_EPERM);
    LT_ERRNO(SC2(clock_settime, LX_CLOCK_MONOTONIC, P(&ts)), LX_EINVAL);
    ts.tv_nsec = -1;
    LT_ERRNO(SC2(clock_settime, LX_CLOCK_REALTIME, P(&ts)), LX_EINVAL);
}

/* adjtimex(2): modes == 0 reads the NTP state (any user), any change needs CAP_SYS_TIME. */
LT_TEST(adjtimex_read_only)
{
    uint8_t tx[208];
    memset(tx, 0, sizeof tx);
    int64_t st = SC1(adjtimex, P(tx));
    LT_CHECK(st >= 0 && st <= 5);                    /* TIME_OK .. TIME_ERROR */
    int64_t tick;
    memcpy(&tick, tx + 88, 8);                       /* offsetof(struct __kernel_timex, tick) == 88 */
    LT_EQ(tick, 10000);
    int64_t sec;
    memcpy(&sec, tx + 72, 8);                        /* time.tv_sec at 72 */
    LT_CHECK(sec > 1700000000);
    uint32_t modes = 1;                              /* ADJ_OFFSET */
    memcpy(tx, &modes, 4);
    LT_ERRNO(SC1(adjtimex, P(tx)), LX_EPERM);
    memset(tx, 0, sizeof tx);
    LT_CHECK(SC2(clock_adjtime, LX_CLOCK_REALTIME, P(tx)) >= 0);
    LT_ERRNO(SC2(clock_adjtime, LX_CLOCK_MONOTONIC, P(tx)), LX_EOPNOTSUPP);
    LT_ERRNO(SC2(clock_adjtime, 10, P(tx)), LX_EINVAL);
}

/* nanosleep(2): sleeps at least the requested time; invalid timespec is EINVAL. */
LT_TEST(nanosleep_duration)
{
    struct lx_timespec req = { 0, 20000000 }, rem = { 7, 7 };
    int64_t a = now_ns(LX_CLOCK_MONOTONIC);
    LT_EQ(SC2(nanosleep, P(&req), P(&rem)), 0);
    int64_t d = now_ns(LX_CLOCK_MONOTONIC) - a;
    LT_CHECK(d >= 20000000 && d < 500000000);
    req.tv_nsec = 300000;                            /* sub-millisecond: precise, not rounded to 1 ms */
    a = now_ns(LX_CLOCK_MONOTONIC);
    LT_EQ(SC2(nanosleep, P(&req), 0), 0);
    d = now_ns(LX_CLOCK_MONOTONIC) - a;
    LT_CHECK(d >= 300000 && d < 100000000);
    req.tv_nsec = 1000000000;
    LT_ERRNO(SC2(nanosleep, P(&req), 0), LX_EINVAL);
    req.tv_sec = -1; req.tv_nsec = 0;
    LT_ERRNO(SC2(nanosleep, P(&req), 0), LX_EINVAL);
    LT_ERRNO(SC2(nanosleep, 8, 0), LX_EFAULT);
}

/* clock_nanosleep(2): TIMER_ABSTIME wakes at the deadline; alarm clocks need CAP_WAKE_ALARM; clocks
 * without nsleep are EOPNOTSUPP; the thread CPU clock is EINVAL. */
LT_TEST(clock_nanosleep_rules)
{
    int64_t dl = now_ns(LX_CLOCK_MONOTONIC) + 30000000;
    struct lx_timespec abs = { dl / 1000000000ll, dl % 1000000000ll };
    LT_EQ(SC4(clock_nanosleep, LX_CLOCK_MONOTONIC, LX_TIMER_ABSTIME, P(&abs), 0), 0);
    LT_CHECK(now_ns(LX_CLOCK_MONOTONIC) >= dl);
    struct lx_timespec past = { 0, 1 };
    LT_EQ(SC4(clock_nanosleep, LX_CLOCK_REALTIME, LX_TIMER_ABSTIME, P(&past), 0), 0);
    struct lx_timespec rel = { 0, 10000000 };
    int64_t a = now_ns(LX_CLOCK_BOOTTIME);
    LT_EQ(SC4(clock_nanosleep, LX_CLOCK_BOOTTIME, 0, P(&rel), 0), 0);
    LT_CHECK(now_ns(LX_CLOCK_BOOTTIME) - a >= 10000000);
    LT_ERRNO(SC4(clock_nanosleep, LX_CLOCK_REALTIME_ALARM, 0, P(&rel), 0), LX_EPERM);
    LT_ERRNO(SC4(clock_nanosleep, LX_CLOCK_MONOTONIC_RAW, 0, P(&rel), 0), LX_EOPNOTSUPP);
    LT_ERRNO(SC4(clock_nanosleep, LX_CLOCK_THREAD_CPUTIME_ID, 0, P(&rel), 0), LX_EINVAL);
    LT_ERRNO(SC4(clock_nanosleep, 12, 0, P(&rel), 0), LX_EINVAL);
}

static void *poke(void *arg)
{
    lkd_task *t = arg;
    usleep(40000);
    atomic_fetch_or(&t->flags, LKD_TF_SIGPENDING);
    lkd_task_wake(t);
    return NULL;
}

/* A signal ends a relative sleep: the remaining time is written and the syscall is restartable through
 * restart_syscall(2) with the same absolute expiry (ERESTART_RESTARTBLOCK). An absolute sleep returns
 * ERESTARTNOHAND without touching rem. Through lkd_syscall with a handler pending: -EINTR. */
LT_TEST(sleep_interrupt_and_restart)
{
    pthread_t th;
    struct lx_timespec req = { 1, 0 }, rem = { 0, 0 };
    int64_t a = now_ns(LX_CLOCK_MONOTONIC);
    pthread_create(&th, NULL, poke, T);
    int64_t r = SC2(nanosleep, P(&req), P(&rem));
    pthread_join(th, NULL);
    LT_EQ(r, -LX_ERESTART_RESTARTBLOCK);
    LT_CHECK(rem.tv_sec == 0 && rem.tv_nsec > 500000000);
    LT_CHECK(T->restart.fn != NULL);
    atomic_fetch_and(&T->flags, ~LKD_TF_SIGPENDING);
    /* restart_syscall continues to the original deadline (about 1 s after a) */
    lkd_sysreq rs = { LKD_NR_restart_syscall, { 0 } };
    LT_EQ(lkd_dispatch(T, &rs), 0);
    int64_t d = now_ns(LX_CLOCK_MONOTONIC) - a;
    LT_CHECK(d >= 1000000000 && d < 3000000000ll);
    /* absolute */
    int64_t dl = now_ns(LX_CLOCK_MONOTONIC) + 1000000000;
    struct lx_timespec abs = { dl / 1000000000ll, dl % 1000000000ll };
    rem.tv_sec = 9; rem.tv_nsec = 9;
    pthread_create(&th, NULL, poke, T);
    r = SC4(clock_nanosleep, LX_CLOCK_MONOTONIC, LX_TIMER_ABSTIME, P(&abs), P(&rem));
    pthread_join(th, NULL);
    LT_EQ(r, -LX_ERESTARTNOHAND);
    LT_EQ(rem.tv_sec, 9);
    /* through the dispatcher entry point the sig module decides: -EINTR when a handler runs, or a
     * restart through restart_syscall when nothing is delivered (arch_do_signal_or_restart) */
    pthread_create(&th, NULL, poke, T);
    lkd_sysreq ns = { LKD_NR_nanosleep, { P(&req), P(&rem), 0, 0, 0, 0 } };
    int64_t rax = 0;
    lkd_disp disp = lkd_syscall(T, &ns, &rax);
    pthread_join(th, NULL);
    LT_CHECK((disp == LKD_RETURN && rax == -LX_EINTR) || (disp == LKD_RESTART && rax == LKD_NR_restart_syscall));
    atomic_fetch_and(&T->flags, ~LKD_TF_SIGPENDING);
}

/* setitimer(2)/getitimer(2)/alarm(2): ITIMER_REAL sends SIGALRM (SI_KERNEL) to the process when it
 * expires; getitimer shows the remaining time; alarm returns the previous remainder rounded. */
LT_TEST(interval_timers)
{
    /* blocked, so that a default-fatal signal stays pending (complete_signal kills only if unblocked) */
    lkd_sig_set_mask(T, LX_SIGBIT(LX_SIGALRM) | LX_SIGBIT(LX_SIGVTALRM) | LX_SIGBIT(LX_SIGPROF));
    LT_CHECK(!(pend() & LX_SIGBIT(LX_SIGALRM)));
    struct lx_itimerval v = { { 0, 0 }, { 0, 30000 } }, o;
    LT_EQ(SC3(setitimer, LX_ITIMER_REAL, P(&v), P(&o)), 0);
    LT_EQ(o.it_value.tv_sec, 0);
    LT_EQ(o.it_value.tv_usec, 0);
    LT_EQ(SC2(getitimer, LX_ITIMER_REAL, P(&o)), 0);
    LT_CHECK(o.it_value.tv_usec > 0 && o.it_value.tv_usec <= 30000);
    for (int i = 0; i < 100 && !(pend() & LX_SIGBIT(LX_SIGALRM)); i++) usleep(10000);
    LT_CHECK(pend() & LX_SIGBIT(LX_SIGALRM));
    LT_EQ(SC2(getitimer, LX_ITIMER_REAL, P(&o)), 0);
    LT_EQ(o.it_value.tv_sec, 0);
    LT_EQ(o.it_value.tv_usec, 0);                    /* one-shot: disarmed after expiry */
    LT_EQ(SC1(alarm, 10), 0);
    LT_EQ(SC1(alarm, 0), 10);                         /* 9.99 s rounds to 10 */
    v.it_value.tv_usec = 1000000;
    LT_ERRNO(SC3(setitimer, LX_ITIMER_REAL, P(&v), 0), LX_EINVAL);
    LT_ERRNO(SC3(setitimer, 3, 0, 0), LX_EINVAL);
    LT_ERRNO(SC2(getitimer, 3, P(&o)), LX_EINVAL);
    /* periodic: after several periods it is still armed (reloaded from it_interval) */
    struct lx_itimerval per = { { 0, 20000 }, { 0, 20000 } };
    LT_EQ(SC3(setitimer, LX_ITIMER_REAL, P(&per), 0), 0);
    usleep(70000);
    LT_EQ(SC2(getitimer, LX_ITIMER_REAL, P(&o)), 0);
    LT_EQ(o.it_interval.tv_usec, 20000);
    LT_CHECK(o.it_value.tv_usec > 0 && o.it_value.tv_usec <= 20000);
    LT_EQ(SC3(setitimer, LX_ITIMER_REAL, 0, 0), 0);  /* NULL new value disarms */
    LT_EQ(SC2(getitimer, LX_ITIMER_REAL, P(&o)), 0);
    LT_EQ(o.it_interval.tv_usec, 0);
    /* ITIMER_PROF counts process CPU time */
    LT_CHECK(!(pend() & LX_SIGBIT(LX_SIGPROF)));
    struct lx_itimerval prof = { { 0, 0 }, { 0, 10000 } };
    LT_EQ(SC3(setitimer, LX_ITIMER_PROF, P(&prof), 0), 0);
    volatile uint64_t x = 0;
    int64_t until = now_ns(LX_CLOCK_MONOTONIC) + 2000000000ll;
    while (!(pend() & LX_SIGBIT(LX_SIGPROF)) && now_ns(LX_CLOCK_MONOTONIC) < until)
        for (int i = 0; i < 100000; i++) x += (uint64_t)i;
    LT_CHECK(pend() & LX_SIGBIT(LX_SIGPROF));
}

int main(void)
{
    LT_EQ(fake_engine_init(), 0);
    lkd_usermem_set_check_fn(allow_all);
    T = fake_make_task();
    LT_RUN(clock_ids);
    LT_RUN(clock_semantics);
    LT_RUN(cpu_clock_ids);
    LT_RUN(clock_getres_values);
    LT_RUN(wall_time_and_setting);
    LT_RUN(adjtimex_read_only);
    LT_RUN(nanosleep_duration);
    LT_RUN(clock_nanosleep_rules);
    LT_RUN(sleep_interrupt_and_restart);
    LT_RUN(interval_timers);
    return lt_finish();
}
