/* time.c: Linux clocks, sleeps, interval timers.
 *
 * Clock mapping (MESURE darwin-process-facts-b.md J; LU Apple clock_gettime(3)):
 *   REALTIME, REALTIME_COARSE, TAI, REALTIME_ALARM -> Darwin CLOCK_REALTIME (1 us steps). The TAI
 *       offset is 0 as on a Linux system where nothing set it with adjtimex(ADJ_TAI).
 *   MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE -> Darwin CLOCK_UPTIME_RAW (mach_absolute_time): like
 *       Linux CLOCK_MONOTONIC it stops while the machine sleeps. It is not slewed by NTP (Linux
 *       MONOTONIC is): a documented difference of about 30 ppm.
 *   BOOTTIME, BOOTTIME_ALARM -> Darwin CLOCK_MONOTONIC_RAW (mach_continuous_time): counts sleep.
 *   PROCESS_CPUTIME_ID, THREAD_CPUTIME_ID and the dynamic CPU clocks -> host CPU time.
 * The COARSE clocks are as precise as their fine counterpart (the coarse ones are a speed trade-off
 * on Linux, never a semantic one).
 */
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include "sig.h"
#include "task_priv.h"

#define NSEC 1000000000ll

static int host_clock_of(int clk, clockid_t *out)
{
    switch (clk) {
    case LX_CLOCK_REALTIME: case LX_CLOCK_REALTIME_COARSE: case LX_CLOCK_TAI: case LX_CLOCK_REALTIME_ALARM:
        *out = CLOCK_REALTIME; return 0;
    case LX_CLOCK_MONOTONIC: case LX_CLOCK_MONOTONIC_RAW: case LX_CLOCK_MONOTONIC_COARSE:
        *out = CLOCK_UPTIME_RAW; return 0;
    case LX_CLOCK_BOOTTIME: case LX_CLOCK_BOOTTIME_ALARM:
        *out = CLOCK_MONOTONIC_RAW; return 0;
    case LX_CLOCK_PROCESS_CPUTIME_ID:
        *out = CLOCK_PROCESS_CPUTIME_ID; return 0;
    case LX_CLOCK_THREAD_CPUTIME_ID:
        *out = CLOCK_THREAD_CPUTIME_ID; return 0;
    default:
        return -LX_EINVAL;
    }
}

int64_t lx_clock_now_ns(int clk)
{
    clockid_t h;
    if (host_clock_of(clk, &h) < 0) return -1;
    return (int64_t)clock_gettime_nsec_np(h);
}

int64_t lkd_guest_clock_ns(int clockid) { return lx_clock_now_ns(clockid); }

static int64_t mach_to_ns(uint64_t t)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (int64_t)((__uint128_t)t * tb.numer / tb.denom);
}

/* ---- CPU clocks (kernel/time/posix-cpu-timers.c encoding) -------------------------------------- */

#define CPUCLOCK_PROF 0
#define CPUCLOCK_VIRT 1
#define CPUCLOCK_SCHED 2
#define CPUCLOCK_PERTHREAD_MASK 4
#define CLOCKFD 3

static int64_t tv_ns(struct timeval tv) { return (int64_t)tv.tv_sec * NSEC + (int64_t)tv.tv_usec * 1000; }

static int cpu_clock_ns(lkd_task *t, int clk, int64_t *out)
{
    int32_t pid = ~(clk >> 3);
    int which = clk & 3, perthread = clk & CPUCLOCK_PERTHREAD_MASK;
    if (which == CLOCKFD) return -LX_EINVAL; /* no posix clock (PTP) device exists */
    if (perthread) {
        /* only threads of the caller's group; this host can only measure the calling thread */
        if (pid != 0 && pid != t->tid) {
            lkd_task *x = lkd_task_lookup(pid);
            int same = x && x->proc == t->proc;
            if (x) lkd_task_put(x);
            if (!same) return -LX_EINVAL;
            lkd_log_once("CPU clock of another thread (tid %d) is not measurable on this host", pid);
            return -LX_EINVAL;
        }
        thread_basic_info_data_t bi;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        if (which == CPUCLOCK_SCHED) { *out = (int64_t)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); return 0; }
        if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS)
            return -LX_EINVAL;
        int64_t u = (int64_t)bi.user_time.seconds * NSEC + (int64_t)bi.user_time.microseconds * 1000;
        int64_t s = (int64_t)bi.system_time.seconds * NSEC + (int64_t)bi.system_time.microseconds * 1000;
        *out = which == CPUCLOCK_VIRT ? u : u + s;
        return 0;
    }
    if (pid == 0 || pid == t->proc->pid) {
        if (which == CPUCLOCK_SCHED) { *out = (int64_t)clock_gettime_nsec_np(CLOCK_PROCESS_CPUTIME_ID); return 0; }
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        *out = which == CPUCLOCK_VIRT ? tv_ns(ru.ru_utime) : tv_ns(ru.ru_utime) + tv_ns(ru.ru_stime);
        return 0;
    }
    lkd_proc_info pi;
    if (lkd_proc_lookup(pid, &pi) < 0 || pi.state == LKD_PS_ZOMBIE) return -LX_EINVAL;
    struct proc_taskinfo ti;
    if (proc_pidinfo(pi.host_pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != (int)sizeof ti) return -LX_EINVAL;
    /* proc_taskinfo times are in mach absolute time units on arm64 */
    int64_t u = mach_to_ns(ti.pti_total_user), s = mach_to_ns(ti.pti_total_system);
    *out = which == CPUCLOCK_VIRT ? u : u + s;
    return 0;
}

static int clock_read(lkd_task *t, int clk, int64_t *ns)
{
    if (clk < 0) return cpu_clock_ns(t, clk, ns);
    int64_t v = lx_clock_now_ns(clk);
    if (v < 0) return -LX_EINVAL;
    *ns = v;
    return 0;
}

static struct lx_timespec ts_of(int64_t ns) { struct lx_timespec ts = { ns / NSEC, ns % NSEC }; return ts; }
static int ts_valid(const struct lx_timespec *ts) { return ts->tv_sec >= 0 && ts->tv_nsec >= 0 && ts->tv_nsec < NSEC; }
static int64_t ts_ns(const struct lx_timespec *ts)
{
    if (ts->tv_sec > INT64_MAX / NSEC - 1) return INT64_MAX;
    return ts->tv_sec * NSEC + ts->tv_nsec;
}

LKD_SYSCALL(clock_gettime)
{
    int64_t ns;
    int e = clock_read(t, LKD_ARGI(0), &ns);
    if (e < 0) return e;
    struct lx_timespec ts = ts_of(ns);
    return lkd_user_write(LKD_ARGP(1), &ts, sizeof ts) ? -LX_EFAULT : 0;
}

LKD_SYSCALL(clock_getres)
{
    int clk = LKD_ARGI(0);
    struct lx_timespec ts = { 0, 0 };
    if (clk < 0) {
        int64_t dummy;
        int e = cpu_clock_ns(t, clk, &dummy);
        if (e < 0) return e;
        /* posix_cpu_clock_getres(): 1 ns for SCHED clocks, one tick (HZ=250) for PROF and VIRT */
        ts.tv_nsec = (clk & 3) == CPUCLOCK_SCHED ? 1 : 4000000;
    } else {
        clockid_t h;
        if (host_clock_of(clk, &h) < 0) return -LX_EINVAL;
        struct timespec r;
        if (clock_getres(h, &r) < 0) return LKD_HOST_ERR();
        ts.tv_sec = r.tv_sec;
        ts.tv_nsec = r.tv_nsec ? r.tv_nsec : 1;
    }
    if (LKD_ARGP(1) && lkd_user_write(LKD_ARGP(1), &ts, sizeof ts)) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(gettimeofday)
{
    if (LKD_ARGP(0)) {
        int64_t ns = lx_clock_now_ns(LX_CLOCK_REALTIME);
        struct lx_timeval tv = { ns / NSEC, (ns % NSEC) / 1000 };
        if (lkd_user_write(LKD_ARGP(0), &tv, sizeof tv)) return -LX_EFAULT;
    }
    if (LKD_ARGP(1)) {
        struct lx_timezone tz = { 0, 0 }; /* sys_tz: never set on a UTC system */
        if (lkd_user_write(LKD_ARGP(1), &tz, sizeof tz)) return -LX_EFAULT;
    }
    return 0;
}

LKD_SYSCALL(time)
{
    int64_t s = lx_clock_now_ns(LX_CLOCK_REALTIME) / NSEC;
    if (LKD_ARGP(0) && lkd_user_write(LKD_ARGP(0), &s, sizeof s)) return -LX_EFAULT;
    return s;
}

LKD_SYSCALL(settimeofday)
{
    if (LKD_ARGP(0)) {
        struct lx_timeval tv;
        if (lkd_user_read(LKD_ARGP(0), &tv, sizeof tv)) return -LX_EFAULT;
        if (tv.tv_usec < 0 || tv.tv_usec >= 1000000 || tv.tv_sec < 0) return -LX_EINVAL;
    }
    if (LKD_ARGP(1)) {
        struct lx_timezone tz;
        if (lkd_user_read(LKD_ARGP(1), &tz, sizeof tz)) return -LX_EFAULT;
    }
    return -LX_EPERM; /* security_settime64(): CAP_SYS_TIME */
}

LKD_SYSCALL(clock_settime)
{
    int clk = LKD_ARGI(0);
    if (clk != LX_CLOCK_REALTIME) return -LX_EINVAL; /* only REALTIME has clock_set */
    struct lx_timespec ts;
    if (lkd_user_read(LKD_ARGP(1), &ts, sizeof ts)) return -LX_EFAULT;
    if (!ts_valid(&ts)) return -LX_EINVAL;
    return -LX_EPERM;
}

/* struct __kernel_timex, 208 bytes on x86-64 */
struct lx_timex {
    uint32_t modes; int32_t _p0;
    int64_t offset, freq, maxerror, esterror;
    int32_t status; int32_t _p1;
    int64_t constant, precision, tolerance;
    struct lx_timeval time;
    int64_t tick, ppsfreq, jitter;
    int32_t shift; int32_t _p2;
    int64_t stabil, jitcnt, calcnt, errcnt, stbcnt;
    int32_t tai;
    int32_t _pad[11];
};
_Static_assert(sizeof(struct lx_timex) == 208, "struct timex is 208 bytes on x86-64");

#define ADJ_OFFSET_SS_READ 0xa001

static int64_t do_adjtimex(uint64_t gva)
{
    struct lx_timex tx;
    if (lkd_user_read(gva, &tx, sizeof tx)) return -LX_EFAULT;
    if (tx.modes && tx.modes != ADJ_OFFSET_SS_READ) return -LX_EPERM; /* CAP_SYS_TIME */
    struct timex h;
    memset(&h, 0, sizeof h);
    int state = ntp_adjtime(&h); /* read-only query of the host NTP state */
    uint32_t modes = tx.modes;
    memset(&tx, 0, sizeof tx);
    tx.modes = modes;
    if (state < 0) {
        state = 5; /* TIME_ERROR */
        tx.status = 0x40; /* STA_UNSYNC */
        tx.maxerror = tx.esterror = 16000000;
        tx.constant = 2;
        tx.tolerance = 32768000;
    } else {
        tx.offset = h.offset; tx.freq = h.freq; tx.maxerror = h.maxerror; tx.esterror = h.esterror;
        tx.status = h.status; tx.constant = h.constant; tx.precision = h.precision; tx.tolerance = h.tolerance;
        tx.ppsfreq = h.ppsfreq; tx.jitter = h.jitter; tx.shift = h.shift; tx.stabil = h.stabil;
        tx.jitcnt = h.jitcnt; tx.calcnt = h.calcnt; tx.errcnt = h.errcnt; tx.stbcnt = h.stbcnt;
    }
    int64_t ns = lx_clock_now_ns(LX_CLOCK_REALTIME);
    tx.time.tv_sec = ns / NSEC;
    tx.time.tv_usec = (tx.status & 0x2000) ? ns % NSEC : (ns % NSEC) / 1000; /* STA_NANO */
    tx.tick = 10000; /* tick_usec: USEC_PER_SEC / USER_HZ */
    tx.tai = 0;
    if (lkd_user_write(gva, &tx, sizeof tx)) return -LX_EFAULT;
    return state;
}

LKD_SYSCALL(adjtimex) { return do_adjtimex(LKD_ARGP(0)); }

LKD_SYSCALL(clock_adjtime)
{
    int clk = LKD_ARGI(0);
    clockid_t h;
    if (clk < 0 || host_clock_of(clk, &h) < 0) return -LX_EINVAL;
    if (clk != LX_CLOCK_REALTIME) return -LX_EOPNOTSUPP; /* only CLOCK_REALTIME has clock_adj */
    return do_adjtimex(LKD_ARGP(1));
}

/* ---- Sleeping ------------------------------------------------------------------------------------- */

/* Sleep until the guest clock clk reaches deadline. 0 when reached, -LX_ERESTARTSYS when interrupted.
 * The bulk of the wait is interruptible (wake pipe); the last millisecond is a host nanosleep, so the
 * wake-up is precise (poll(2) only has millisecond granularity). */
static int sleep_until(lkd_task *t, int clk, int64_t deadline)
{
    for (;;) {
        int64_t now = lx_clock_now_ns(clk);
        if (now >= deadline) return 0;
        int64_t left = deadline - now;
        if (left > 2000000) {
            int64_t chunk = left - 1000000;
            if (chunk > NSEC) chunk = NSEC; /* re-read the clock at least once a second (REALTIME jumps) */
            atomic_fetch_or(&t->flags, LKD_TF_IN_BLOCK);
            int r = lkd_block_sleep(t, lkd_host_mono_ns() + chunk);
            atomic_fetch_and(&t->flags, ~LKD_TF_IN_BLOCK);
            if (r == -LX_ERESTARTSYS) return r;
        } else {
            if (lkd_sig_interrupted(t)) return -LX_ERESTARTSYS;
            struct timespec ts = { 0, (long)left };
            nanosleep(&ts, NULL);
        }
    }
}

static int64_t sleep_common(lkd_task *t, int clk, int64_t deadline, int abs, uint64_t rem_gva);

static int64_t sleep_restart(lkd_task *t)
{
    return sleep_common(t, (int)t->restart.arg[0], (int64_t)t->restart.arg[1], 0, t->restart.arg[2]);
}

static int64_t sleep_common(lkd_task *t, int clk, int64_t deadline, int abs, uint64_t rem_gva)
{
    int r = sleep_until(t, clk, deadline);
    if (r == 0) return 0;
    /* kernel/time/hrtimer.c do_nanosleep()/hrtimer_nanosleep(): an absolute sleep is restarted only if
     * no handler runs (ERESTARTNOHAND); a relative one writes the remaining time and restarts through
     * restart_syscall with the same absolute expiry. */
    if (abs) return -LX_ERESTARTNOHAND;
    int64_t left = deadline - lx_clock_now_ns(clk);
    if (left <= 0) return 0;
    if (rem_gva) {
        struct lx_timespec ts = ts_of(left);
        if (lkd_user_write(rem_gva, &ts, sizeof ts)) return -LX_EFAULT;
    }
    t->restart.fn = sleep_restart;
    t->restart.arg[0] = (uint64_t)clk;
    t->restart.arg[1] = (uint64_t)deadline;
    t->restart.arg[2] = rem_gva;
    return -LX_ERESTART_RESTARTBLOCK;
}

LKD_SYSCALL(nanosleep)
{
    struct lx_timespec ts;
    if (lkd_user_read(LKD_ARGP(0), &ts, sizeof ts)) return -LX_EFAULT;
    if (!ts_valid(&ts)) return -LX_EINVAL;
    int64_t now = lx_clock_now_ns(LX_CLOCK_MONOTONIC), d = ts_ns(&ts);
    int64_t deadline = d > INT64_MAX - now ? INT64_MAX : now + d;
    return sleep_common(t, LX_CLOCK_MONOTONIC, deadline, 0, LKD_ARGP(1));
}

LKD_SYSCALL(clock_nanosleep)
{
    int clk = LKD_ARGI(0), flags = LKD_ARGI(1);
    switch (clk) {
    case LX_CLOCK_REALTIME: case LX_CLOCK_MONOTONIC: case LX_CLOCK_BOOTTIME: case LX_CLOCK_TAI:
        break;
    case LX_CLOCK_REALTIME_ALARM: case LX_CLOCK_BOOTTIME_ALARM:
        return -LX_EPERM; /* alarm_timer_nsleep(): CAP_WAKE_ALARM */
    case LX_CLOCK_MONOTONIC_RAW: case LX_CLOCK_REALTIME_COARSE: case LX_CLOCK_MONOTONIC_COARSE:
        return -LX_EOPNOTSUPP; /* no nsleep for these k_clocks */
    case LX_CLOCK_THREAD_CPUTIME_ID:
        return -LX_EINVAL;
    case LX_CLOCK_PROCESS_CPUTIME_ID:
        lkd_log_once("clock_nanosleep on a CPU-time clock is not supported");
        return -LX_EOPNOTSUPP;
    default:
        if (clk < 0) {
            if (clk & CPUCLOCK_PERTHREAD_MASK) return -LX_EINVAL;
            lkd_log_once("clock_nanosleep on a CPU-time clock is not supported");
            return -LX_EOPNOTSUPP;
        }
        return -LX_EINVAL;
    }
    struct lx_timespec ts;
    if (lkd_user_read(LKD_ARGP(2), &ts, sizeof ts)) return -LX_EFAULT;
    if (!ts_valid(&ts)) return -LX_EINVAL;
    int abs = (flags & LX_TIMER_ABSTIME) != 0;
    int64_t d = ts_ns(&ts), now = lx_clock_now_ns(clk);
    int64_t deadline = abs ? d : (d > INT64_MAX - now ? INT64_MAX : now + d);
    return sleep_common(t, clk, deadline, abs, abs ? 0 : LKD_ARGP(3));
}

/* ---- Interval timers -------------------------------------------------------------------------------- */

struct itimer_state {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    struct lkd_proc *proc;
    int running, stop;
    int64_t real_deadline, real_interval;   /* host mono ns; 0 = disarmed */
    int64_t cpu_left[3], cpu_interval[3];   /* VIRTUAL and PROF use index 1 and 2: remaining CPU ns */
    int64_t cpu_last[3];
};

static int64_t proc_cpu_ns(int which)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return which == LX_ITIMER_VIRTUAL ? tv_ns(ru.ru_utime) : tv_ns(ru.ru_utime) + tv_ns(ru.ru_stime);
}

static void itimer_fire(struct lkd_proc *p, int signo)
{
    lkd_task *target = NULL;
    pthread_mutex_lock(&p->lock);
    for (lkd_task *x = p->threads; x; x = x->next_in_proc)
        if (!(atomic_load(&x->flags) & LKD_TF_EXITING)) { target = x; if (x->tid == p->pid) break; }
    if (target) lkd_task_get(target);
    pthread_mutex_unlock(&p->lock);
    if (!target) return;
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_signo = signo;
    si.si_code = LX_SI_KERNEL; /* it_real_fn(): SEND_SIG_PRIV */
    lkd_sig_send_task(target, signo, &si, 0);
    lkd_task_put(target);
}

static void *itimer_thread(void *arg)
{
    struct itimer_state *st = arg;
    struct lkd_proc *p = st->proc;
    pthread_mutex_lock(&st->lock);
    while (!st->stop) {
        int64_t now = lkd_host_mono_ns(), wait = 10 * NSEC;
        int fire_real = 0, fire_cpu[3] = { 0, 0, 0 };
        if (st->real_deadline) {
            if (now >= st->real_deadline) {
                fire_real = 1;
                if (st->real_interval) {
                    st->real_deadline += st->real_interval;
                    if (st->real_deadline <= now) st->real_deadline = now + st->real_interval; /* overrun */
                } else st->real_deadline = 0;
            }
            if (st->real_deadline && st->real_deadline - now < wait) wait = st->real_deadline - now;
        }
        for (int w = 1; w <= 2; w++) {
            if (!st->cpu_left[w]) continue;
            int64_t c = proc_cpu_ns(w);
            st->cpu_left[w] -= c - st->cpu_last[w];
            st->cpu_last[w] = c;
            if (st->cpu_left[w] <= 0) {
                fire_cpu[w] = 1;
                st->cpu_left[w] = st->cpu_interval[w];
            }
            if (st->cpu_left[w] && wait > 4000000) wait = 4000000; /* sample CPU time every 4 ms (one tick) */
        }
        if (fire_real || fire_cpu[1] || fire_cpu[2]) {
            pthread_mutex_unlock(&st->lock);
            if (fire_real) itimer_fire(p, LX_SIGALRM);
            if (fire_cpu[1]) itimer_fire(p, LX_SIGVTALRM);
            if (fire_cpu[2]) itimer_fire(p, LX_SIGPROF);
            pthread_mutex_lock(&st->lock);
            continue;
        }
        if (!st->real_deadline && !st->cpu_left[1] && !st->cpu_left[2]) wait = 3600 * NSEC;
        struct timespec rel = { wait / NSEC, wait % NSEC };
        pthread_cond_timedwait_relative_np(&st->cv, &st->lock, &rel);
    }
    st->running = 0;
    pthread_mutex_unlock(&st->lock);
    if (atomic_fetch_sub(&p->refs, 1) == 1) proc_free(p);
    return NULL;
}

static struct itimer_state *itimers_get(struct lkd_proc *p)
{
    pthread_mutex_lock(&p->lock);
    struct itimer_state *st = p->wait->itimers;
    if (!st) {
        st = calloc(1, sizeof *st);
        if (st) {
            pthread_mutex_init(&st->lock, NULL);
            pthread_cond_init(&st->cv, NULL);
            st->proc = p;
            p->wait->itimers = st;
        }
    }
    pthread_mutex_unlock(&p->lock);
    return st;
}

static int itimers_start(struct itimer_state *st)
{
    if (st->running) return 0;
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 << 10);
    atomic_fetch_add(&st->proc->refs, 1);
    st->running = 1;
    st->stop = 0;
    if (pthread_create(&th, &a, itimer_thread, st) != 0) {
        st->running = 0;
        atomic_fetch_sub(&st->proc->refs, 1);
        pthread_attr_destroy(&a);
        return -LX_EAGAIN;
    }
    pthread_attr_destroy(&a);
    return 0;
}

void itimers_proc_exit(struct lkd_proc *p)
{
    struct itimer_state *st = p->wait ? p->wait->itimers : NULL;
    if (!st) return;
    pthread_mutex_lock(&st->lock);
    int running = st->running;
    st->stop = 1;
    pthread_cond_signal(&st->cv);
    pthread_mutex_unlock(&st->lock);
    if (!running && atomic_load(&p->refs) <= 0) {
        /* called from proc_free: the thread is gone and holds no reference */
        pthread_mutex_destroy(&st->lock);
        pthread_cond_destroy(&st->cv);
        free(st);
        p->wait->itimers = NULL;
    }
}

void itimers_fork_child(struct lkd_proc *p) { (void)p; } /* a fork child starts with no timers */
void itimers_fork_prepare(void) { }
void itimers_fork_parent(void) { }

static struct lx_timeval tv_of(int64_t ns) { struct lx_timeval tv = { ns / NSEC, (ns % NSEC) / 1000 }; return tv; }

static void itimer_read(struct itimer_state *st, int which, struct lx_itimerval *out)
{
    memset(out, 0, sizeof *out);
    if (!st) return;
    pthread_mutex_lock(&st->lock);
    if (which == LX_ITIMER_REAL) {
        if (st->real_deadline) {
            int64_t rem = st->real_deadline - lkd_host_mono_ns();
            if (rem <= 0) rem = 1000; /* itimer_get_remtime(): an expired but active timer shows 1 us */
            out->it_value = tv_of(rem);
        }
        out->it_interval = tv_of(st->real_interval);
    } else {
        if (st->cpu_left[which]) {
            int64_t c = proc_cpu_ns(which), left = st->cpu_left[which] - (c - st->cpu_last[which]);
            out->it_value = tv_of(left > 0 ? left : 1000);
        }
        out->it_interval = tv_of(st->cpu_interval[which]);
    }
    pthread_mutex_unlock(&st->lock);
}

static int tv_valid(const struct lx_timeval *tv) { return tv->tv_sec >= 0 && tv->tv_usec >= 0 && tv->tv_usec < 1000000; }
static int64_t tvl_ns(const struct lx_timeval *tv) { return tv->tv_sec * NSEC + tv->tv_usec * 1000; }

static int itimer_set(lkd_task *t, int which, const struct lx_itimerval *n, struct lx_itimerval *old)
{
    struct itimer_state *st = itimers_get(t->proc);
    if (!st) return -LX_ENOMEM;
    itimer_read(st, which, old);
    int64_t v = tvl_ns(&n->it_value), iv = tvl_ns(&n->it_interval);
    pthread_mutex_lock(&st->lock);
    if (which == LX_ITIMER_REAL) {
        st->real_deadline = v ? lkd_host_mono_ns() + v : 0;
        st->real_interval = v ? iv : 0;
    } else {
        st->cpu_left[which] = v;
        st->cpu_interval[which] = v ? iv : 0;
        st->cpu_last[which] = proc_cpu_ns(which);
    }
    int need = st->real_deadline || st->cpu_left[1] || st->cpu_left[2];
    int e = 0;
    if (need) e = itimers_start(st);
    pthread_cond_signal(&st->cv);
    pthread_mutex_unlock(&st->lock);
    return e;
}

LKD_SYSCALL(getitimer)
{
    int which = LKD_ARGI(0);
    if (which < LX_ITIMER_REAL || which > LX_ITIMER_PROF) return -LX_EINVAL;
    struct lx_itimerval v;
    itimer_read(t->proc->wait->itimers, which, &v);
    return lkd_user_write(LKD_ARGP(1), &v, sizeof v) ? -LX_EFAULT : 0;
}

LKD_SYSCALL(setitimer)
{
    int which = LKD_ARGI(0);
    struct lx_itimerval n, o;
    memset(&n, 0, sizeof n);
    if (LKD_ARGP(1)) {
        if (lkd_user_read(LKD_ARGP(1), &n, sizeof n)) return -LX_EFAULT;
        if (!tv_valid(&n.it_value) || !tv_valid(&n.it_interval)) return -LX_EINVAL;
    }
    if (which < LX_ITIMER_REAL || which > LX_ITIMER_PROF) return -LX_EINVAL;
    int e = itimer_set(t, which, &n, &o);
    if (e < 0) return e;
    if (LKD_ARGP(2) && lkd_user_write(LKD_ARGP(2), &o, sizeof o)) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(alarm)
{
    struct lx_itimerval n, o;
    memset(&n, 0, sizeof n);
    n.it_value.tv_sec = LKD_ARGUI(0);
    if (itimer_set(t, LX_ITIMER_REAL, &n, &o) < 0) return 0;
    /* alarm_setitimer(): round the old value to the nearest second, never 0 for a pending alarm */
    int64_t r = o.it_value.tv_sec;
    if ((!r && o.it_value.tv_usec) || o.it_value.tv_usec >= 500000) r++;
    return r;
}

void task_time_register(void)
{
    LKD_REGISTER(clock_gettime); LKD_REGISTER(clock_getres); LKD_REGISTER(gettimeofday); LKD_REGISTER(time);
    LKD_REGISTER(settimeofday); LKD_REGISTER(clock_settime); LKD_REGISTER(adjtimex); LKD_REGISTER(clock_adjtime);
    LKD_REGISTER(nanosleep); LKD_REGISTER(clock_nanosleep);
    LKD_REGISTER(getitimer); LKD_REGISTER(setitimer); LKD_REGISTER(alarm);
}
