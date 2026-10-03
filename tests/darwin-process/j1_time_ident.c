/*
 * j1_time_ident.c - J: clocks, timer resolution, pid/tid spaces, uid/gid, rlimits,
 * thread and process count limits, TPIDR_EL0 behaviour.
 */
#include "common.h"
#include <libproc.h>
#include <mach/thread_info.h>
#include <os/os_sync_wait_on_address.h>
#include <poll.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/time.h>

static mach_timebase_info_data_t tb;

static uint64_t abs_to_ns(uint64_t t) { return t * tb.numer / tb.denom; }

/* ---- J1: clocks ----------------------------------------------------------- */
struct clk { const char *name; clockid_t id; };

static void j1_clocks(void)
{
    struct clk clks[] = {
        {"CLOCK_REALTIME", CLOCK_REALTIME},
        {"CLOCK_MONOTONIC", CLOCK_MONOTONIC},
        {"CLOCK_MONOTONIC_RAW", CLOCK_MONOTONIC_RAW},
        {"CLOCK_MONOTONIC_RAW_APPROX", CLOCK_MONOTONIC_RAW_APPROX},
        {"CLOCK_UPTIME_RAW", CLOCK_UPTIME_RAW},
        {"CLOCK_UPTIME_RAW_APPROX", CLOCK_UPTIME_RAW_APPROX},
        {"CLOCK_PROCESS_CPUTIME_ID", CLOCK_PROCESS_CPUTIME_ID},
        {"CLOCK_THREAD_CPUTIME_ID", CLOCK_THREAD_CPUTIME_ID},
    };
    for (size_t i = 0; i < sizeof clks / sizeof clks[0]; i++) {
        struct timespec ts, res;
        int r = clock_gettime(clks[i].id, &ts);
        if (r != 0) { INFO("J1.clock", "%s id=%d clock_gettime failed errno=%d", clks[i].name, (int)clks[i].id, errno); continue; }
        int rr = clock_getres(clks[i].id, &res);
        /* measured resolution: smallest non-zero step between consecutive reads */
        uint64_t minstep = UINT64_MAX;
        struct timespec a, b;
        clock_gettime(clks[i].id, &a);
        for (int k = 0; k < 200000; k++) {
            clock_gettime(clks[i].id, &b);
            int64_t d = (int64_t)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec);
            if (d > 0 && (uint64_t)d < minstep) minstep = (uint64_t)d;
            a = b;
        }
        /* cost per call */
        uint64_t t0 = dp_ns();
        const int N = 1000000;
        for (int k = 0; k < N; k++) clock_gettime(clks[i].id, &b);
        double cost = (double)(dp_ns() - t0) / N;
        INFO("J1.clock", "%-28s id=%2d value=%lld.%09ld getres=%s%ld.%09ldns measured_min_step=%lluns cost=%.1fns/call",
             clks[i].name, (int)clks[i].id, (long long)ts.tv_sec, ts.tv_nsec,
             rr == 0 ? "" : "ERR ", rr == 0 ? (long)res.tv_sec : (long)errno, rr == 0 ? res.tv_nsec : 0L,
             (unsigned long long)(minstep == UINT64_MAX ? 0 : minstep), cost);
    }
    /* mach clocks */
    mach_timebase_info(&tb);
    INFO("J1.mach", "mach_timebase_info numer=%u denom=%u (%.4f ns/tick)", tb.numer, tb.denom, (double)tb.numer / tb.denom);
    uint64_t a = mach_absolute_time(), c = mach_continuous_time();
    uint64_t aa = mach_approximate_time(), ca = mach_continuous_approximate_time();
    INFO("J1.mach", "mach_absolute_time=%llu continuous=%llu approx=%llu continuous_approx=%llu", a, c, aa, ca);
    INFO("J1.mach", "continuous-absolute=%.3f s (time spent asleep since boot, per mach)", (double)abs_to_ns(c - a) / 1e9);
    uint64_t minstep = UINT64_MAX, prev = mach_absolute_time();
    for (int k = 0; k < 500000; k++) {
        uint64_t n = mach_absolute_time();
        if (n > prev && n - prev < minstep) minstep = n - prev;
        prev = n;
    }
    uint64_t t0 = dp_ns();
    for (int k = 0; k < 2000000; k++) (void)mach_absolute_time();
    INFO("J1.mach", "mach_absolute_time min step = %llu ticks (%llu ns), cost %.1f ns/call", minstep, abs_to_ns(minstep),
         (double)(dp_ns() - t0) / 2000000);

    /* the raw architectural counter */
    uint64_t cnt1, cnt2, frq;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt1));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    uint64_t mat = mach_absolute_time();
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt2));
    INFO("J1.cntvct", "cntfrq_el0=%llu Hz cntvct_el0=%llu mach_absolute_time=%llu (cntvct_before<=mach<=cntvct_after: %s) offset(mach-cntvct)=%lld",
         frq, cnt1, mat, (cnt1 <= mat && mat <= cnt2) ? "yes" : "no", (long long)(mat - cnt1));
    t0 = dp_ns();
    for (int k = 0; k < 5000000; k++) __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt1));
    INFO("J1.cntvct", "isb+mrs cntvct_el0 cost %.1f ns/read", (double)(dp_ns() - t0) / 5000000);
    t0 = dp_ns();
    for (int k = 0; k < 5000000; k++) __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cnt1));
    INFO("J1.cntvct", "mrs cntvct_el0 (no isb) cost %.1f ns/read", (double)(dp_ns() - t0) / 5000000);
    INFO("J1.cntvct", "cntfrq_el0 reads %llu Hz while hw.tbfrequency and mach_timebase_info say 24 MHz (see j2_counter for the behaviour of the register)", frq);

    /* relationships between clocks */
    struct timespec mono, upt, mraw, rt;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_UPTIME_RAW, &upt);
    clock_gettime(CLOCK_MONOTONIC_RAW, &mraw);
    clock_gettime(CLOCK_REALTIME, &rt);
    double dmono = mono.tv_sec + mono.tv_nsec / 1e9, dupt = upt.tv_sec + upt.tv_nsec / 1e9, draw = mraw.tv_sec + mraw.tv_nsec / 1e9;
    INFO("J1.rel", "CLOCK_MONOTONIC - CLOCK_UPTIME_RAW = %.3f s ; CLOCK_MONOTONIC_RAW - CLOCK_UPTIME_RAW = %.3f s", dmono - dupt, draw - dupt);
    INFO("J1.rel", "(mach_continuous_time - mach_absolute_time was %.3f s above, i.e. no system sleep since boot: CLOCK_MONOTONIC is not ahead because of sleep)", (double)abs_to_ns(mach_continuous_time() - mach_absolute_time()) / 1e9);
    struct timeval bt; size_t sz = sizeof bt;
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &bt, &sz, NULL, 0) == 0) {
        double up = (double)rt.tv_sec + rt.tv_nsec / 1e9 - ((double)bt.tv_sec + bt.tv_usec / 1e6);
        INFO("J1.rel", "wall-clock uptime since kern.boottime = %.1f s ; CLOCK_MONOTONIC = %.1f s ; CLOCK_UPTIME_RAW = %.1f s", up, dmono, dupt);
    }
    struct timeval tv; gettimeofday(&tv, NULL);
    INFO("J1.rel", "gettimeofday %lld.%06d time()=%lld", (long long)tv.tv_sec, (int)tv.tv_usec, (long long)time(NULL));
    /* absent Linux clocks */
    struct timespec x;
    for (int id = 0; id < 20; id++) {
        errno = 0;
        int r = clock_gettime((clockid_t)id, &x);
        if (r != 0) INFO("J1.clockids", "clock id %d unsupported errno=%d (%s)", id, errno, strerror(errno));
    }
}

/* ---- J2: sleeping/timer accuracy ---------------------------------------- */

typedef void (*sleep_fn)(uint64_t ns);

static void sl_nanosleep(uint64_t ns) { struct timespec t = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)}; nanosleep(&t, NULL); }
static void sl_usleep(uint64_t ns) { usleep((useconds_t)(ns / 1000)); }
static void sl_machwait(uint64_t ns) { mach_wait_until(mach_absolute_time() + ns * tb.denom / tb.numer); }
static void sl_select(uint64_t ns) { struct timeval t = {(time_t)(ns / 1000000000ull), (suseconds_t)((ns % 1000000000ull) / 1000)}; select(0, NULL, NULL, NULL, &t); }
static void sl_ossync(uint64_t ns)
{
    static uint32_t word = 0;
    os_sync_wait_on_address_with_timeout(&word, 0, sizeof word, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, ns);
}
static void sl_pollms(uint64_t ns) { poll(NULL, 0, (int)((ns + 999999) / 1000000)); }

static void measure_sleep(const char *id, const char *name, sleep_fn f, uint64_t req_ns, int n)
{
    uint64_t *v = malloc(n * sizeof *v);
    for (int i = 0; i < n; i++) {
        uint64_t t0 = dp_ns();
        f(req_ns);
        uint64_t d = dp_ns() - t0;
        v[i] = d > req_ns ? d - req_ns : 0;   /* overshoot */
    }
    dp_stats s = dp_stat_u64(v, n);
    INFO(id, "%-12s request=%7.0fus overshoot p50=%8.1fus p90=%8.1fus p99=%8.1fus max=%8.1fus (n=%d)", name, req_ns / 1e3,
         s.p50 / 1e3, s.p90 / 1e3, s.p99 / 1e3, s.max / 1e3, n);
    free(v);
}

static void j2_sleep_matrix(const char *tag)
{
    struct { const char *n; sleep_fn f; } fns[] = {{"nanosleep", sl_nanosleep}, {"usleep", sl_usleep}, {"mach_wait", sl_machwait},
                                                    {"os_sync_to", sl_ossync}, {"select", sl_select}};
    uint64_t reqs[] = {20000, 100000, 1000000, 10000000};
    char id[32]; snprintf(id, sizeof id, "J2.%s", tag);
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++)
        for (size_t k = 0; k < sizeof reqs / sizeof reqs[0]; k++)
            measure_sleep(id, fns[i].n, fns[i].f, reqs[k], reqs[k] >= 10000000 ? 100 : 300);
    measure_sleep(id, "poll(ms)", sl_pollms, 1000000, 200);
}

static void *j2_thread_default(void *arg) { (void)arg; j2_sleep_matrix("default_qos"); return NULL; }
static void *j2_thread_ui(void *arg)
{
    (void)arg;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    j2_sleep_matrix("qos_user_interactive");
    return NULL;
}
static void *j2_thread_rt(void *arg)
{
    (void)arg;
    thread_time_constraint_policy_data_t p;
    uint64_t ms = 1000000ull * tb.denom / tb.numer;
    p.period = (uint32_t)(ms);
    p.computation = (uint32_t)(ms / 10);
    p.constraint = (uint32_t)(ms / 2);
    p.preemptible = 1;
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr != KERN_SUCCESS) { INFO("J2.rt", "THREAD_TIME_CONSTRAINT_POLICY failed kr=%d", kr); return NULL; }
    j2_sleep_matrix("realtime_policy");
    return NULL;
}

static void j2_run(void)
{
    pthread_t t;
    pthread_create(&t, NULL, j2_thread_default, NULL); pthread_join(t, NULL);
    pthread_create(&t, NULL, j2_thread_ui, NULL); pthread_join(t, NULL);
    pthread_create(&t, NULL, j2_thread_rt, NULL); pthread_join(t, NULL);
}

/* ---- J3: pid / tid spaces ---------------------------------------------------- */

struct tidrec { uint64_t tid; uint32_t mach_name; uint64_t handle; };

static void *tid_thread(void *arg)
{
    struct tidrec *r = arg;
    pthread_threadid_np(NULL, &r->tid);
    r->mach_name = mach_thread_self();
    thread_identifier_info_data_t info; mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &c) == KERN_SUCCESS) r->handle = info.thread_handle;
    return NULL;
}

static void j3_ids(void)
{
    uint64_t tid; pthread_threadid_np(NULL, &tid);
    thread_identifier_info_data_t info; mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
    thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO, (thread_info_t)&info, &c);
    INFO("J3.ids", "pid=%d ppid=%d pgrp=%d sid=%d main-thread tid(pthread_threadid_np)=%llu (0x%llx) thread_info.thread_id=%llu mach_thread_self name=0x%x pthread_mach_thread_np=0x%x",
         getpid(), getppid(), getpgrp(), getsid(0), tid, tid, info.thread_id, mach_thread_self(), pthread_mach_thread_np(pthread_self()));
    CHECK(tid == info.thread_id, "J3.ids", "pthread_threadid_np == THREAD_IDENTIFIER_INFO.thread_id");
    INFO("J3.ids", "tid fits in 32 bits now: %s ; tid >> 32 = %llu", tid < (1ull << 32) ? "yes" : "no", tid >> 32);

    /* sequential thread creation: how do tids advance? */
    enum { N = 2000 };
    uint64_t *tids = malloc(N * sizeof *tids);
    for (int i = 0; i < N; i++) {
        struct tidrec r = {0};
        pthread_t t; pthread_create(&t, NULL, tid_thread, &r); pthread_join(t, NULL);
        tids[i] = r.tid;
    }
    uint64_t mind = UINT64_MAX, maxd = 0; int mono = 1; double sumd = 0;
    for (int i = 1; i < N; i++) {
        if (tids[i] <= tids[i - 1]) mono = 0;
        uint64_t d = tids[i] - tids[i - 1];
        if (d < mind) mind = d;
        if (d > maxd) maxd = d;
        sumd += d;
    }
    INFO("J3.tid", "2000 sequential create+join: first=%llu last=%llu increments min=%llu max=%llu mean=%.2f strictly increasing=%s",
         tids[0], tids[N - 1], mind, maxd, sumd / (N - 1), mono ? "yes" : "no");
    free(tids);
    /* system-wide consumption rate of tids: sample two thread tids 3 s apart */
    struct tidrec a = {0}, b = {0};
    pthread_t t;
    pthread_create(&t, NULL, tid_thread, &a); pthread_join(t, NULL);
    uint64_t t0 = dp_ns();
    sleep(3);
    pthread_create(&t, NULL, tid_thread, &b); pthread_join(t, NULL);
    double secs = (dp_ns() - t0) / 1e9;
    double rate = (double)(b.tid - a.tid) / secs;
    INFO("J3.tid", "system-wide tid counter advanced %llu in %.2f s = %.0f tids/s (includes every thread of every process)", b.tid - a.tid, secs, rate);
    double left = ((double)(1ull << 32) - (double)b.tid) / (rate > 1 ? rate : 1);
    INFO("J3.tid", "at that rate the low 32 bits of the host tid wrap after %.1f days (infer: tid=%llu now)", left / 86400.0, b.tid);

    /* the pid space */
    pid_t kids[3];
    for (int i = 0; i < 3; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        kids[i] = p;
    }
    for (int i = 0; i < 3; i++) waitpid(kids[i], NULL, 0);
    INFO("J3.pid", "three consecutive fork() pids: %d %d %d (pid_t is %zu bytes)", kids[0], kids[1], kids[2], sizeof(pid_t));
    int npids = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0) / (int)sizeof(pid_t);
    pid_t *pl = malloc((npids + 64) * sizeof(pid_t));
    int got = proc_listpids(PROC_ALL_PIDS, 0, pl, (npids + 64) * (int)sizeof(pid_t)) / (int)sizeof(pid_t);
    pid_t maxpid = 0; for (int i = 0; i < got; i++) if (pl[i] > maxpid) maxpid = pl[i];
    INFO("J3.pid", "proc_listpids: %d processes, highest pid in use %d", got, maxpid);
    free(pl);
}

/* ---- J4: identity, limits ------------------------------------------------- */

static void j4_identity_limits(void)
{
    gid_t groups[64]; int ng = getgroups(64, groups);
    INFO("J4.id", "uid=%u euid=%u gid=%u egid=%u ngroups=%d NGROUPS_MAX=%d sysconf=%ld", getuid(), geteuid(), getgid(), getegid(), ng, NGROUPS_MAX, sysconf(_SC_NGROUPS_MAX));
    errno = 0;
    int r = setuid(0);
    INFO("J4.id", "setuid(0) as non-root returns %d errno=%d (%s): a guest root must be faked by the emulation layer", r, errno, strerror(errno));
    r = setuid(getuid());
    INFO("J4.id", "setuid(getuid()) returns %d", r);
    struct { const char *n; int r; } lims[] = {
        {"RLIMIT_CPU", RLIMIT_CPU}, {"RLIMIT_FSIZE", RLIMIT_FSIZE}, {"RLIMIT_DATA", RLIMIT_DATA}, {"RLIMIT_STACK", RLIMIT_STACK},
        {"RLIMIT_CORE", RLIMIT_CORE}, {"RLIMIT_AS", RLIMIT_AS}, {"RLIMIT_MEMLOCK", RLIMIT_MEMLOCK}, {"RLIMIT_NPROC", RLIMIT_NPROC},
        {"RLIMIT_NOFILE", RLIMIT_NOFILE},
    };
    for (size_t i = 0; i < sizeof lims / sizeof lims[0]; i++) {
        struct rlimit rl;
        if (getrlimit(lims[i].r, &rl) != 0) { INFO("J4.rlimit", "%s getrlimit errno=%d", lims[i].n, errno); continue; }
        char s[32], h[32];
        if (rl.rlim_cur == RLIM_INFINITY) snprintf(s, sizeof s, "unlimited"); else snprintf(s, sizeof s, "%llu", (unsigned long long)rl.rlim_cur);
        if (rl.rlim_max == RLIM_INFINITY) snprintf(h, sizeof h, "unlimited"); else snprintf(h, sizeof h, "%llu", (unsigned long long)rl.rlim_max);
        INFO("J4.rlimit", "%-14s soft=%s hard=%s", lims[i].n, s, h);
    }
    /* NOFILE raise quirk: RLIM_INFINITY is refused, kern.maxfilesperproc is the real cap */
    struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
    struct rlimit want = {RLIM_INFINITY, RLIM_INFINITY};
    errno = 0;
    r = setrlimit(RLIMIT_NOFILE, &want);
    INFO("J4.rlimit", "setrlimit(NOFILE, RLIM_INFINITY) -> %d errno=%d (%s)", r, errno, strerror(errno));
    int mfpp = 0; size_t sz = sizeof mfpp;
    sysctlbyname("kern.maxfilesperproc", &mfpp, &sz, NULL, 0);
    want.rlim_cur = mfpp; want.rlim_max = rl.rlim_max;
    errno = 0;
    r = setrlimit(RLIMIT_NOFILE, &want);
    INFO("J4.rlimit", "setrlimit(NOFILE, kern.maxfilesperproc=%d) -> %d errno=%d", mfpp, r, errno);
    getrlimit(RLIMIT_NOFILE, &rl);
    INFO("J4.rlimit", "NOFILE now soft=%llu hard=%llu OPEN_MAX=%d", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max, OPEN_MAX);
}

/* ---- J5: thread count limit ---------------------------------------------- */

static pthread_mutex_t gmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gcv = PTHREAD_COND_INITIALIZER;
static int gstop;
static void *park_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&gmx);
    while (!gstop) pthread_cond_wait(&gcv, &gmx);
    pthread_mutex_unlock(&gmx);
    return NULL;
}

static void j5_thread_limit(void)
{
    int limit = 0; size_t sz = sizeof limit;
    sysctlbyname("kern.num_taskthreads", &limit, &sz, NULL, 0);
    /* SAFETY: creating thousands of threads in one process caused two kernel panics on this Mac
     * (com.apple.kec.pthread spinlock timeout, 2026-10-02 10:00 and 10:48). Never raise this cap. */
    int cap = 400;
    (void)limit;
    pthread_t *ts = malloc(cap * sizeof *ts);
    pthread_attr_t at; pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16384);
    int n = 0, err = 0;
    uint64_t t0 = dp_ns();
    for (; n < cap; n++) {
        err = pthread_create(&ts[n], &at, park_thread, NULL);
        if (err) break;
    }
    double secs = (dp_ns() - t0) / 1e9;
    INFO("J5.threads", "kern.num_taskthreads=%d ; created %d parked threads before pthread_create failed with %d (%s) ; %.0f threads/s", limit, n, err, err ? strerror(err) : "cap reached", n / (secs > 0 ? secs : 1));
    mach_msg_type_number_t cnt = 0; thread_act_array_t th = NULL;
    task_threads(mach_task_self(), &th, &cnt);
    INFO("J5.threads", "task_threads reports %u threads in the task", cnt);
    pthread_mutex_lock(&gmx); gstop = 1; pthread_cond_broadcast(&gcv); pthread_mutex_unlock(&gmx);
    for (int i = 0; i < n; i++) pthread_join(ts[i], NULL);
    free(ts);
    CHECK(n >= 400, "J5.threads", "at least 400 threads per process possible (capped on purpose, %d)", n);
}

/* ---- J6: process count burst --------------------------------------------- */
static void j6_process_burst(void)
{
    int maxuid = 0; size_t sz = sizeof maxuid;
    sysctlbyname("kern.maxprocperuid", &maxuid, &sz, NULL, 0);
    int cnt = proc_listpids(PROC_UID_ONLY, getuid(), NULL, 0) / (int)sizeof(pid_t);
    struct rlimit rl; getrlimit(RLIMIT_NPROC, &rl);
    INFO("J6.proc", "kern.maxprocperuid=%d RLIMIT_NPROC soft=%llu hard=%llu ; processes of this uid now: %d", maxuid, (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max, cnt);
    enum { B = 300 };
    pid_t kids[B]; int n = 0;
    uint64_t t0 = dp_ns();
    for (; n < B; n++) {
        pid_t p = fork();
        if (p < 0) break;
        if (p == 0) { alarm(15); pause(); _exit(0); }
        kids[n] = p; dp_track(p);
    }
    double secs = (dp_ns() - t0) / 1e9;
    INFO("J6.proc", "forked %d sleeping children in %.3f s (%.0f us per fork incl. tiny address space)", n, secs, secs * 1e6 / (n ? n : 1));
    for (int i = 0; i < n; i++) kill(kids[i], SIGKILL);
    for (int i = 0; i < n; i++) waitpid(kids[i], NULL, 0);
    CHECK(n == B, "J6.proc", "%d simultaneous children spawned", n);
}

/* ---- J7: TPIDR_EL0 / thread attributes ----------------------------------- */
static void *tpidr_thread(void *arg)
{
    uint64_t magic = (uint64_t)(uintptr_t)arg, back = 0, ro = 0;
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(magic));
    usleep(20000);
    sched_yield();
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(back));
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(ro));
    CHECK(back == magic, "J7.tpidr", "TPIDR_EL0 write %llx read back %llx after 20 ms sleep (per-thread, preserved across context switches)", magic, back);
    INFO("J7.tpidr", "TPIDRRO_EL0 (pthread TSD base, read-only) = 0x%llx, pthread_self=%p", ro, (void *)pthread_self());
    return NULL;
}

static void *custom_stack_thread(void *arg)
{
    char probe;
    INFO("J7.stack", "thread stack local at %p, expected within [%p, %p)", &probe, arg, (char *)arg + 256 * 1024);
    uintptr_t p = (uintptr_t)&probe;
    CHECK(p >= (uintptr_t)arg && p < (uintptr_t)arg + 256 * 1024, "J7.stack", "pthread_attr_setstack stack is really used");
    char nm[80]; pthread_getname_np(pthread_self(), nm, sizeof nm);
    INFO("J7.stack", "default thread name before setname: '%s'", nm);
    return NULL;
}

static void j7_misc(void)
{
    pthread_t t[2];
    pthread_create(&t[0], NULL, tpidr_thread, (void *)(uintptr_t)0x1111222233334444ull);
    pthread_create(&t[1], NULL, tpidr_thread, (void *)(uintptr_t)0x5555666677778888ull);
    pthread_join(t[0], NULL); pthread_join(t[1], NULL);
    /* custom stack, as a clone(CLONE_VM, stack=...) emulation would need */
    void *stk = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    pthread_attr_t at; pthread_attr_init(&at);
    int e = pthread_attr_setstack(&at, stk, 256 * 1024);
    INFO("J7.stack", "pthread_attr_setstack(user mmap'd 256K) -> %d", e);
    e = pthread_create(&t[0], &at, custom_stack_thread, stk);
    INFO("J7.stack", "pthread_create with user stack -> %d", e);
    if (!e) pthread_join(t[0], NULL);
    /* thread names */
    char name[128], got[128];
    memset(name, 'n', 100); name[100] = 0;
    e = pthread_setname_np(name);
    pthread_getname_np(pthread_self(), got, sizeof got);
    INFO("J7.name", "pthread_setname_np(100 chars) -> %d ; stored length %zu (Linux comm is 15)", e, strlen(got));
    /* suspended creation */
    pthread_t st;
    extern int pthread_create_suspended_np(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) __attribute__((weak_import));
    if (pthread_create_suspended_np) {
        e = pthread_create_suspended_np(&st, NULL, custom_stack_thread, stk);
        INFO("J7.suspended", "pthread_create_suspended_np -> %d (thread is created suspended; thread_resume starts it)", e);
        if (!e) { thread_resume(pthread_mach_thread_np(st)); pthread_join(st, NULL); }
    } else {
        INFO("J7.suspended", "pthread_create_suspended_np not found");
    }
}

int main(void)
{
    dp_watchdog(220);
    mach_timebase_info(&tb);
    j1_clocks();
    j2_run();
    j3_ids();
    j4_identity_limits();
    j5_thread_limit();
    j6_process_burst();
    j7_misc();
    return dp_summary("j1_time_ident");
}
