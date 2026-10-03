/* test_ptrace_transport.c: the shared table, its lock, the cross-process wake and the cost of a stop and a
 * resume. The table is the module's own design (no Linux counterpart): its cases check the contract written
 * in src/ptrace/transport.c. The round-trip cases print MESURE lines and assert only generous bounds. */
#include "test_ptrace_util.h"
#include "../src/ptrace/pt_priv.h"

/* The table holds PT_SLOTS links; the 513th fails visibly with EAGAIN, a duplicate tid with EPERM (already
 * traced, like ptrace_attach()). Freed slots are reused with a new generation. */
LT_TEST(table_capacity)
{
    int idx[PT_SLOTS + 4];
    int n = 0;
    for (; n < PT_SLOTS + 4; n++) {
        int i = pt_link(0x20000 + n, 0x20000 + n, getpid(), 1000, 1, 1, getpid(), 0, 0);
        if (i < 0) { LT_EQ(i, -LX_EAGAIN); break; }
        idx[n] = i;
    }
    LT_EQ(n, PT_SLOTS);
    LT_EQ(pt_link(0x20003, 5, getpid(), 1000, 1, 1, getpid(), 0, 0), -LX_EPERM);
    LT_EQ(pt_find_tracee(0x20005), idx[5]);
    LT_EQ(pt_find_tracee(0x2ffff), -1);
    uint32_t gen = pt_slot_at(idx[7])->gen;
    pt_lock();
    pt_free_locked(idx[7]);
    pt_unlock();
    LT_EQ(pt_find_tracee(0x20007), -1);
    int j = pt_link(0x30000, 0x30000, getpid(), 1000, 1, 1, getpid(), 0, 0);
    LT_EQ(j, idx[7]);
    LT_EQ(pt_slot_at(j)->gen, gen + 1);
    LT_EQ(lkd_ptrace_tracer_of(0x30000), 1);
    pt_lock();
    for (int k = 0; k < n; k++) pt_free_locked(idx[k]);
    pt_unlock();
    LT_EQ(lkd_ptrace_tracer_of(0x20005), 0);
}

static void *lock_thread(void *arg)
{
    pt_lock();
    *(volatile int *)arg = 1;
    pt_unlock();
    return NULL;
}

/* A process that dies holding the table lock does not block the others: the byte-range lock is dropped by
 * the host kernel (CONVENTIONS 3, rule 4). */
LT_TEST(lock_is_released_when_the_holder_dies)
{
    int up[2];
    LT_EQ(pipe(up), 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm(20);
        pt_lock();
        char b = 1;
        (void)!write(up[1], &b, 1);
        sleep(15);
        _exit(0);
    }
    char b;
    LT_EQ(read(up[0], &b, 1), 1);
    volatile int got = 0;
    pthread_t th;
    LT_EQ(pthread_create(&th, NULL, lock_thread, (void *)&got), 0);
    usleep(150000);
    LT_EQ(got, 0);                                               /* blocked while the child holds it */
    kill(c, SIGKILL);
    int st;
    waitpid(c, &st, 0);
    for (int i = 0; i < 400 && !got; i++) usleep(5000);
    LT_EQ(got, 1);
    pthread_join(th, NULL);
    close(up[0]); close(up[1]);
}

/* The table is shared memory: what a forked process writes under the lock is seen by the parent, and the
 * parent's links are visible in the child. */
LT_TEST(table_is_shared_between_processes)
{
    int i = pt_link(0x40000, 0x40000, getpid(), 1000, 7, 7, getpid(), 0x11, 0);
    LT_CHECK(i >= 0);
    int fd[2];
    LT_EQ(pipe(fd), 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        alarm(20);
        int ok = pt_find_tracee(0x40000) == i && pt_slot_at(i)->options == 0x11;
        pt_lock();
        pt_slot_at(i)->options = 0x22;
        pt_slot_at(i)->msg = 0x1234567890ull;
        pt_unlock();
        _exit(ok ? 0 : 1);
    }
    int st;
    waitpid(c, &st, 0);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    LT_EQ(pt_slot_at(i)->options, 0x22);
    LT_EQ(pt_slot_at(i)->msg, 0x1234567890ull);
    pt_lock();
    pt_free_locked(i);
    pt_unlock();
    close(fd[0]); close(fd[1]);
}

/* pt_wake_tracee() reaches a task of another host process through the family service and ends its wait on
 * the wake pipe. */
LT_TEST(wake_crosses_processes)
{
    int up[2], ack[2];
    LT_EQ(pipe(up), 0);
    LT_EQ(pipe(ack), 0);
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) {
        alarm(30);
        lkd_task *t = lkd_current();
        char b = 1;
        (void)!write(up[1], &b, 1);
        struct pollfd p = { .fd = t->wake_rd, .events = POLLIN };
        int r = poll(&p, 1, 10000);
        int64_t v = r > 0 ? 1 : 0;
        (void)!write(ack[1], &v, sizeof v);
        int st;
        (void)sct(t, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0);
        if (lkd_task_release(t, &st)) _exit(st);
        _exit(90);
    }
    LT_CHECK(pid > 0);
    char b;
    LT_EQ(read(up[0], &b, 1), 1);
    usleep(50000);
    LT_EQ(pt_wake_tracee((int32_t)pid, (int32_t)pid, 0), 0);
    LT_EQ(get64(ack[0]), 1);
    int st;
    LT_EQ(wait_pid((int32_t)pid, &st, 0), pid);
    /* a process that does not exist */
    LT_ERRNO(pt_wake_tracee(0x3ffffff0, 0x3ffffff0, 0), LX_ESRCH);
    close(up[0]); close(up[1]); close(ack[0]); close(ack[1]);
}

/* ---- the cost of a stop and a resume -------------------------------------------------------------- */

#define N_ROUNDS 1500

static void report(const char *what, int64_t *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_i64);
    printf("MESURE %s: n=%d p50=%.1f us p90=%.1f us p99=%.1f us max=%.1f us\n", what, n, (double)v[n / 2] / 1000.0,
           (double)v[n * 9 / 10] / 1000.0, (double)v[n * 99 / 100] / 1000.0, (double)v[n - 1] / 1000.0);
}

/* wait4 with WNOHANG in a loop: the latency of the transport alone, without the 10 ms tick of the blocking
 * wait4 in the task module (src/task/wait.c: lkd_block_sleep does not return on a plain wake-up). */
static int64_t wait_polled(int32_t pid, int *st)
{
    for (int i = 0; i < 20000000; i++) {
        int64_t r = wait_pid(pid, st, LX_WNOHANG);
        if (r != 0) return r;
        if (i > 2000) usleep(20);
    }
    return 0;
}

/* Resume a tracee that makes getpid() calls and wait for its next stop: PTRACE_SYSCALL + wait4, then also
 * with a GETREGSET after each stop, as EAC does. Tracee in a thread of this process. */
LT_TEST(round_trip_thread_tracee)
{
    struct tracee *e = tracee_start(body_loop, NULL);
    for (int i = 0; i < 4000 && e->calls < 3; i++) usleep(500);
    LT_EQ(PT(LX_PTRACE_ATTACH, e->tid, 0, 0), 0);
    LT_CHECK(expect_stop(e->tid, 0x137f));
    static int64_t v[N_ROUNDS];
    int bad = 0;
    for (int i = 0; i < N_ROUNDS; i++) {
        int64_t t0 = mono_ns();
        if (PT(LX_PTRACE_SYSCALL, e->tid, 0, 0) != 0) bad++;
        int st;
        if (wait_pid(e->tid, &st, 0) != e->tid || st != 0x57f) bad++;
        v[i] = mono_ns() - t0;
    }
    LT_EQ(bad, 0);
    report("stop+resume, blocking wait4, tracee in a thread", v, N_ROUNDS);
    LT_CHECK(v[N_ROUNDS / 2] < 30000000);
    for (int i = 0; i < N_ROUNDS; i++) {
        int64_t t0 = mono_ns();
        if (PT(LX_PTRACE_SYSCALL, e->tid, 0, 0) != 0) bad++;
        int st;
        if (wait_polled(e->tid, &st) != e->tid || st != 0x57f) bad++;
        v[i] = mono_ns() - t0;
    }
    LT_EQ(bad, 0);
    report("stop+resume, polled wait4, tracee in a thread", v, N_ROUNDS);
    LT_CHECK(v[N_ROUNDS / 2] < 2000000);
    struct lx_user_regs r;
    for (int i = 0; i < N_ROUNDS; i++) {
        int64_t t0 = mono_ns();
        int64_t n;
        get_regs216(e->tid, &r, &n);
        if (n != 216) bad++;
        v[i] = mono_ns() - t0;
    }
    LT_EQ(bad, 0);
    report("GETREGSET 216 bytes (no round trip: slot read)", v, N_ROUNDS);
    LT_CHECK(v[N_ROUNDS / 2] < 2000000);
    (void)PT(LX_PTRACE_DETACH, e->tid, 0, 0);
    tracee_quit(e);
    get64(e->wr[0]);
    pthread_join(e->th, NULL);
}

static void spin_child(lkd_task *t)
{
    int died = 0;
    cpu_init(t);
    (void)gsys(t, LKD_NR_ptrace, LX_PTRACE_TRACEME, 0, 0, 0, 0, 0, &died);
    died = self_signal(t, LX_SIGSTOP);
    for (int i = 0; i < 100000 && !died; i++) (void)gsys(t, LKD_NR_getpid, 0, 0, 0, 0, 0, 0, &died);
    int st;
    (void)gsys(t, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0, &died);
    if (lkd_task_release(t, &st)) _exit(st);
    _exit(91);
}

/* The same between two host processes: the resume wakes the tracee through the family service (a socket
 * round trip) and the stop notifies the tracer's process the same way. */
LT_TEST(round_trip_process_tracee)
{
    fflush(NULL);
    int64_t pid = sct(TR, LKD_NR_fork, 0, 0, 0, 0, 0, 0);
    if (pid == 0) { alarm(60); spin_child(lkd_current()); _exit(92); }
    LT_CHECK(pid > 0);
    int32_t c = (int32_t)pid;
    LT_CHECK(expect_stop(c, 0x137f));
    static int64_t v[N_ROUNDS];
    int bad = 0;
    for (int i = 0; i < N_ROUNDS; i++) {
        int64_t t0 = mono_ns();
        if (PT(LX_PTRACE_SYSCALL, c, 0, 0) != 0) bad++;
        int st;
        if (wait_pid(c, &st, 0) != c || st != 0x57f) bad++;
        v[i] = mono_ns() - t0;
    }
    LT_EQ(bad, 0);
    report("stop+resume, blocking wait4, tracee in another host process", v, N_ROUNDS);
    LT_CHECK(v[N_ROUNDS / 2] < 30000000);
    for (int i = 0; i < N_ROUNDS; i++) {
        int64_t t0 = mono_ns();
        if (PT(LX_PTRACE_SYSCALL, c, 0, 0) != 0) bad++;
        int st;
        if (wait_polled(c, &st) != c || st != 0x57f) bad++;
        v[i] = mono_ns() - t0;
    }
    LT_EQ(bad, 0);
    report("stop+resume, polled wait4, tracee in another host process", v, N_ROUNDS);
    LT_CHECK(v[N_ROUNDS / 2] < 3000000);
    struct pt_stats s;
    pt_get_stats(&s);
    printf("MESURE stops=%llu resumes=%llu wakes=%llu notifies=%llu (this process)\n", (unsigned long long)s.stops,
           (unsigned long long)s.resumes, (unsigned long long)s.wakes, (unsigned long long)s.notifies);
    LT_EQ(PT(LX_PTRACE_KILL, c, 0, 0), 0);
    int st = 0;
    LT_EQ(wait_pid(c, &st, 0), c);
    LT_EQ(st, 9);
}

int main(int argc, char **argv)
{
    if (argc > 1) g_filter = argv[1];
    test_setup(1);
    RUN(table_capacity);
    RUN(lock_is_released_when_the_holder_dies);
    RUN(table_is_shared_between_processes);
    RUN(wake_crosses_processes);
    RUN(round_trip_thread_tracee);
    RUN(round_trip_process_tracee);
    return lt_finish();
}
