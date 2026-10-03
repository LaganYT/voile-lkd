/*
 * f2_fork_cost.c - F: what fork() costs when the process owns a large reserved address space
 * (64 GiB PROT_NONE) and a lot of touched memory (1 GiB and more), compared with posix_spawn.
 *
 * For every configuration the parent forks N children that _exit(0) at once and records
 *   fork()      : time spent inside fork() in the parent
 *   child start : time from just before fork() to the first instruction after fork() in the child
 *   total       : time from just before fork() until the child has been reaped (includes the child's teardown)
 * and prints min / p50 / p90 / max in microseconds. The first fork and the steady state are reported separately
 * because the first fork write-protects the parent's pages and the following ones find them already protected.
 */
#include "fg_util.h"
#include <libkern/OSCacheControl.h>
#include <mach/mach_vm.h>

typedef struct { _Atomic uint64_t child_t; } shared_t;
static shared_t *sh;

#define GiB (1ull << 30)

static void report(const char *label, uint64_t *tf, uint64_t *tc, uint64_t *tt, int n)
{
    uint64_t first_fork = tf[0], first_total = tt[0];
    dp_stats sf = dp_stat_u64(tf, n), sc = dp_stat_u64(tc, n), st = dp_stat_u64(tt, n);
    INFO("F2.cost", "%-52s fork() first=%8.0f us | steady p50=%8.0f min=%8.0f p90=%8.0f max=%8.0f | child start p50=%8.0f | total p50=%8.0f (n=%d)",
         label, first_fork / 1e3, sf.p50 / 1e3, sf.min / 1e3, sf.p90 / 1e3, sf.max / 1e3, sc.p50 / 1e3, st.p50 / 1e3, n);
    (void)first_total;
}

static int measure_fork(const char *label, int n)
{
    uint64_t tf[64], tc[64], tt[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        atomic_store(&sh->child_t, 0);
        uint64_t t0 = dp_ns();
        pid_t pid = fork();
        if (pid == 0) { atomic_store(&sh->child_t, dp_ns()); _exit(0); }
        uint64_t t1 = dp_ns();
        if (pid < 0) { INFO("F2.cost", "%s: fork failed %s after %d forks", label, ERRS(errno), i); return -1; }
        int st = 0;
        waitpid(pid, &st, 0);
        uint64_t t2 = dp_ns();
        uint64_t ct = atomic_load(&sh->child_t);
        tf[i] = t1 - t0;
        tc[i] = ct > t0 ? ct - t0 : 0;
        tt[i] = t2 - t0;
    }
    report(label, tf, tc, tt, n);
    return 0;
}

static void measure_spawn(const char *label, int n)
{
    uint64_t tf[64], tt[64], tz[64];
    if (n > 64) n = 64;
    char *argv[] = { "true", NULL };
    for (int i = 0; i < n; i++) {
        uint64_t t0 = dp_ns();
        pid_t pid;
        int r = posix_spawn(&pid, "/usr/bin/true", NULL, NULL, argv, environ);
        uint64_t t1 = dp_ns();
        if (r) { INFO("F2.cost", "%s: posix_spawn failed %s", label, ERRS(r)); return; }
        int st = 0;
        waitpid(pid, &st, 0);
        uint64_t t2 = dp_ns();
        tf[i] = t1 - t0; tt[i] = t2 - t0; tz[i] = 0;
    }
    dp_stats sf = dp_stat_u64(tf, n), st = dp_stat_u64(tt, n);
    INFO("F2.cost", "%-52s posix_spawn(/usr/bin/true) call p50=%8.0f us | total (reaped) p50=%8.0f min=%8.0f max=%8.0f (n=%d)", label, sf.p50 / 1e3, st.p50 / 1e3, st.min / 1e3, st.max / 1e3, n);
}

static void measure_forkexec(const char *label, int n)
{
    uint64_t tt[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        uint64_t t0 = dp_ns();
        pid_t pid = fork();
        if (pid == 0) { execl("/usr/bin/true", "true", (char *)NULL); _exit(127); }
        if (pid < 0) { INFO("F2.cost", "%s: fork failed %s", label, ERRS(errno)); return; }
        int st = 0;
        waitpid(pid, &st, 0);
        tt[i] = dp_ns() - t0;
    }
    dp_stats st = dp_stat_u64(tt, n);
    INFO("F2.cost", "%-52s fork+exec(/usr/bin/true)+reap total p50=%8.0f min=%8.0f max=%8.0f (n=%d)", label, st.p50 / 1e3, st.min / 1e3, st.max / 1e3, n);
}

static void touch(void *p, size_t len)
{
    for (size_t off = 0; off < len; off += FG_PAGE) ((volatile unsigned char *)p)[off] = 1;
}

static void *reserve(size_t len)
{
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p;
}

static void run_config(const char *label, int n, int with_exec)
{
    measure_fork(label, n);
    if (with_exec) {
        measure_spawn(label, 20);
        measure_forkexec(label, 20);
    }
}

void *f2_sleeper(void *q)
{
    while (!atomic_load((_Atomic int *)q)) usleep(20000);
    return NULL;
}

int main(int argc, char **argv)
{
    dp_watchdog(400);
    fg_load_info("F2.env");
    sh = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (sh == MAP_FAILED) { FAIL("F2.setup", "shared page: %s", ERRS(errno)); return 1; }

    /* C0 baseline */
    run_config("C0 baseline (no extra mapping)", 30, 1);

    /* C1 64 GiB PROT_NONE reservation only */
    void *r64 = reserve(64 * GiB);
    if (r64 == MAP_FAILED) INFO("F2.cost", "64 GiB PROT_NONE|MAP_NORESERVE reservation failed: %s", ERRS(errno));
    else {
        INFO("F2.cost", "64 GiB PROT_NONE|MAP_NORESERVE reservation at %p ok", r64);
        run_config("C1 64 GiB PROT_NONE reservation", 30, 1);
    }

    /* C2 reservation + 1 GiB touched private */
    void *m1 = mmap(NULL, 1 * GiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t t0 = dp_ns();
    touch(m1, 1 * GiB);
    INFO("F2.cost", "touching 1 GiB private anonymous took %.1f ms (%.0f ns per 16 KiB page)", (double)(dp_ns() - t0) / 1e6, (double)(dp_ns() - t0) / (GiB / FG_PAGE));
    run_config("C2 64 GiB reservation + 1 GiB touched private", 30, 1);

    /* parent re-touch cost after a fork with a sleeping child (copy-on-write of the whole GiB) */
    {
        memset((void *)sh, 0, sizeof *sh);
        pid_t pid = fork();
        if (pid == 0) { dp_role = "child"; dp_watchdog(30); usleep(2000000); _exit(0); }
        dp_track(pid);
        t0 = dp_ns();
        touch(m1, 1 * GiB);
        double ms = (double)(dp_ns() - t0) / 1e6;
        INFO("F2.cost", "parent rewrites the 1 GiB right after a fork (child alive): %.1f ms = %.0f ns/page = %.2f GB/s", ms, ms * 1e6 / (GiB / FG_PAGE), 1.0 / (ms / 1e3));
        int st = 0;
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    munmap(m1, 1 * GiB);

    /* C3 1 GiB RW touched inside the reservation (mprotect splits the entry) */
    if (r64 != MAP_FAILED) {
        mprotect(r64, 1 * GiB, PROT_READ | PROT_WRITE);
        touch(r64, 1 * GiB);
        run_config("C3 64 GiB reservation, first 1 GiB mprotect(RW)+touched", 30, 0);
        mprotect(r64, 1 * GiB, PROT_NONE);
        madvise(r64, 1 * GiB, MADV_FREE);
    }

    /* C4 untouched 1 GiB RW */
    m1 = mmap(NULL, 1 * GiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    run_config("C4 64 GiB reservation + 1 GiB RW untouched", 30, 0);
    munmap(m1, 1 * GiB);

    /* C5 1 GiB touched shared */
    void *ms = mmap(NULL, 1 * GiB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    touch(ms, 1 * GiB);
    run_config("C5 64 GiB reservation + 1 GiB touched MAP_SHARED", 30, 0);
    munmap(ms, 1 * GiB);

    /* C6 fragmented: 64 KiB RW + 16 KiB PROT_NONE, repeated, 1 GiB of RW in total */
    {
        size_t unit = 5 * FG_PAGE, count = GiB / (4 * FG_PAGE);
        size_t total = unit * count;
        unsigned char *f = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        for (size_t i = 0; i < count; i++) {
            touch(f + i * unit, 4 * FG_PAGE);
            mprotect(f + i * unit + 4 * FG_PAGE, FG_PAGE, PROT_NONE);
        }
        run_config("C6 fragmented 1 GiB (32768 VM entries) + reservation", 30, 0);
        munmap(f, total);
    }

    /* C7 4 GiB touched private */
    {
        void *m4 = mmap(NULL, 4 * GiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m4 != MAP_FAILED) {
            t0 = dp_ns();
            touch(m4, 4 * GiB);
            INFO("F2.cost", "touching 4 GiB took %.1f ms", (double)(dp_ns() - t0) / 1e6);
            run_config("C7 64 GiB reservation + 4 GiB touched private", 15, 0);
            munmap(m4, 4 * GiB);
        } else INFO("F2.cost", "4 GiB mmap failed %s", ERRS(errno));
    }

    /* C8 MAP_JIT region: 512 MiB mapped, 64 MiB touched */
    {
        size_t jl = 512ull << 20;
        unsigned char *j = mmap(NULL, jl, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (j != MAP_FAILED) {
            pthread_jit_write_protect_np(0);
            touch(j, 64ull << 20);
            pthread_jit_write_protect_np(1);
            run_config("C8 reservation + 512 MiB MAP_JIT (64 MiB touched)", 20, 0);
            munmap(j, jl);
        } else INFO("F2.cost", "MAP_JIT 512 MiB failed %s", ERRS(errno));
    }

    /* C8b MAP_JIT sweep: is the JIT region copied eagerly? (1 GiB region, increasing touched amount) */
    {
        size_t jl = 1ull << 30;
        size_t amounts[] = { 0, 16ull << 20, 64ull << 20, 256ull << 20 };
        for (int i = 0; i < 4; i++) {
            unsigned char *j = mmap(NULL, jl, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
            if (j == MAP_FAILED) { INFO("F2.cost", "MAP_JIT 1 GiB failed %s", ERRS(errno)); break; }
            if (amounts[i]) { pthread_jit_write_protect_np(0); touch(j, amounts[i]); pthread_jit_write_protect_np(1); }
            char lbl[96];
            snprintf(lbl, sizeof lbl, "C8b 1 GiB MAP_JIT, %zu MiB touched", amounts[i] >> 20);
            run_config(lbl, 12, 0);
            if (i == 3) {
                int rn = minherit(j, jl, VM_INHERIT_NONE); int en = errno;
                snprintf(lbl, sizeof lbl, "C8c same, minherit(VM_INHERIT_NONE)=%s", rn ? ERRS(en) : "ok");
                run_config(lbl, 12, 0);
                int rs = minherit(j, jl, VM_INHERIT_SHARE); int es = errno;
                snprintf(lbl, sizeof lbl, "C8d same, minherit(VM_INHERIT_SHARE)=%s", rs ? ERRS(es) : "ok");
                run_config(lbl, 12, 0);
            }
            munmap(j, jl);
        }
        /* comparison: ordinary anonymous memory with the same amount touched */
        unsigned char *a = mmap(NULL, 1ull << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        touch(a, 256ull << 20);
        run_config("C8e 1 GiB ordinary anonymous, 256 MiB touched", 12, 0);
        munmap(a, 1ull << 30);
    }

    /* C8f many threads: does the thread count matter? */
    {
        static pthread_t th[200];
        static _Atomic int quit;
        extern void *f2_sleeper(void *);
        for (int i = 0; i < 200; i++) pthread_create(&th[i], NULL, f2_sleeper, (void *)&quit);
        usleep(100000);
        run_config("C8f 200 sleeping threads", 15, 0);
        atomic_store(&quit, 1);
        for (int i = 0; i < 200; i++) pthread_join(th[i], NULL);
    }

    /* C9 larger reservations: does fork care about the reserved size? */
    if (r64 != MAP_FAILED) { munmap(r64, 64 * GiB); r64 = MAP_FAILED; }
    {
        size_t sizes[] = { 1ull << 38, 1ull << 40, 1ull << 42, 1ull << 44, 1ull << 46 };
        const char *names[] = { "256 GiB", "1 TiB", "4 TiB", "16 TiB", "64 TiB" };
        for (int i = 0; i < 5; i++) {
            void *r = reserve(sizes[i]);
            if (r == MAP_FAILED) { INFO("F2.cost", "%s PROT_NONE reservation failed: %s", names[i], ERRS(errno)); continue; }
            char lbl[80];
            snprintf(lbl, sizeof lbl, "C9 %s PROT_NONE reservation only", names[i]);
            run_config(lbl, 15, 0);
            munmap(r, sizes[i]);
        }
    }

    dp_summary("f2_fork_cost");
    return 0;
}
