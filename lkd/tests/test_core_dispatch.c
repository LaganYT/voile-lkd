/* test_core_dispatch.c: syscall registration, dispatch, dispositions, restart, ENOSYS, tracing, the
 * blocking helpers, the syscall number table. Models: x86-64 syscall ABI (arch/x86/entry/common.c
 * do_syscall_x64: out-of-range numbers return -ENOSYS), kernel restart codes (include/linux/errno.h),
 * syscall_64.tbl v6.12 numbers. */
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "../src/core/core_priv.h"
#include "sig.h"

static int init_rc, registered_after_init, owner_restart_ok;

/* ---- test syscalls ---- */
static int64_t last_nr; static uint64_t last_args[6]; static int64_t last_cur_nr_inside;
LKD_SYSCALL(read)
{
    last_nr = (int64_t)rq->nr;
    memcpy(last_args, rq->a, sizeof last_args);
    last_cur_nr_inside = atomic_load(&t->cur_nr);
    return (int64_t)(rq->a[0] + rq->a[5]);
}
LKD_SYSCALL(write) { return -LX_EBADF; }
LKD_SYSCALL(getpid) { return LKD_RET_CONTEXT_REPLACED; }
LKD_SYSCALL(exit) { return LKD_RET_EXIT_THREAD; }
LKD_SYSCALL(exit_group) { return LKD_RET_EXIT_GROUP; }
LKD_SYSCALL(nanosleep) { return -LX_ERESTART_RESTARTBLOCK; }
LKD_SYSCALL(pause) { return -LX_ERESTARTNOHAND; }
LKD_SYSCALL(accept) { return -LX_ERESTARTSYS; }
LKD_SYSCALL(ioctl) { return -LX_ENOIOCTLCMD; }
LKD_SYSCALL(getppid) { return (int64_t)0x7fffffffffff0000ll; } /* a big pointer-like value is not an errno */
LKD_SYSCALL(open) { return -LX_ENOENT; }
static int64_t cont_calls;
static int64_t continuation(lkd_task *t) { (void)t; cont_calls++; return 77; }
LKD_SYSCALL(sched_yield) { t->restart.fn = continuation; return -LX_ERESTART_RESTARTBLOCK; }

static void install_test_syscalls(void)
{
    lkd_dispatch_reset();
    lkd_core_register_syscalls();
    lkd_dispatch_set_init_module("testmod");
    LKD_REGISTER(read); LKD_REGISTER(write); LKD_REGISTER(getpid); LKD_REGISTER(exit);
    LKD_REGISTER(exit_group); LKD_REGISTER(nanosleep); LKD_REGISTER(pause); LKD_REGISTER(accept);
    LKD_REGISTER(ioctl); LKD_REGISTER(getppid); LKD_REGISTER(open); LKD_REGISTER(sched_yield);
    lkd_dispatch_set_init_module("");
}

static lkd_sysreq rq(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq r = { nr, { a0, a1, a2, a3, a4, a5 } };
    return r;
}

LT_TEST(modules_init_without_conflict)
{
    /* Every module registers its syscalls in lkd_<m>_init; two owners for one number fail lkd_init. */
    LT_EQ(init_rc, 0);
    LT_CHECK(registered_after_init >= 1); /* core's restart_syscall at least */
    LT_STREQ(lkd_syscall_owner(LKD_NR_restart_syscall), "core");
    LT_CHECK(owner_restart_ok);
}

LT_TEST(dispatch_passes_number_and_six_args)
{
    lkd_task *t = fake_make_task();
    LT_CHECK(t != NULL);
    int64_t rax = 0;
    lkd_sysreq r = rq(LKD_NR_read, 1, 2, 3, 4, 5, 6);
    lkd_disp d = lkd_syscall(t, &r, &rax);
    LT_EQ(d, LKD_RETURN);
    LT_EQ(rax, 7);
    LT_EQ(last_nr, LKD_NR_read);
    for (int i = 0; i < 6; i++) LT_EQ(last_args[i], i + 1);
    /* x86-64 order: rdi rsi rdx r10 r8 r9 map to a[0..5] */
    LT_EQ(last_cur_nr_inside, LKD_NR_read);
    LT_EQ(atomic_load(&t->cur_nr), -1);
    LT_EQ(t->cur_args[5], 6);
}

LT_TEST(negative_errno_is_returned_in_rax)
{
    lkd_task *t = fake_make_task();
    int64_t rax = 0;
    lkd_sysreq r = rq(LKD_NR_write, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_EBADF);
    r = rq(LKD_NR_open, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_ENOENT);
    /* a value that looks like a high pointer is not an error */
    r = rq(LKD_NR_getppid, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, 0x7fffffffffff0000ll);
}

LT_TEST(unimplemented_and_out_of_range_numbers_return_enosys_and_count)
{
    lkd_task *t = fake_make_task();
    int64_t rax = 0;
    uint64_t before = lkd_enosys_count(LKD_NR_getcwd);
    lkd_sysreq r = rq(LKD_NR_getcwd, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_ENOSYS);
    lkd_syscall(t, &r, &rax);
    LT_EQ(lkd_enosys_count(LKD_NR_getcwd), before + 2);
    /* unassigned number inside the table and numbers >= table size (x32 range, garbage) */
    r = rq(LKD_NR_TABLE_SIZE - 1, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_ENOSYS);
    r = rq(LKD_NR_TABLE_SIZE, 0, 0, 0, 0, 0, 0);
    uint64_t oob = lkd_enosys_count(LKD_NR_TABLE_SIZE);
    lkd_syscall(t, &r, &rax);
    LT_ERRNO(rax, LX_ENOSYS);
    LT_EQ(lkd_enosys_count(LKD_NR_TABLE_SIZE), oob + 1);
    r = rq(0x40000000ull | LKD_NR_read, 0, 0, 0, 0, 0, 0); /* x32 bit set: not an x86-64 syscall */
    lkd_syscall(t, &r, &rax);
    LT_ERRNO(rax, LX_ENOSYS);
    r = rq(~0ull, 0, 0, 0, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    LT_ERRNO(rax, LX_ENOSYS);
}

LT_TEST(duplicate_registration_is_refused_and_first_owner_kept)
{
    lkd_dispatch_set_init_module("modA");
    LT_EQ(lkd_register_syscall(LKD_NR_dup, "dup", lkd_sys_read), 0);
    lkd_dispatch_set_init_module("modB");
    LT_ERRNO(lkd_register_syscall(LKD_NR_dup, "dup", lkd_sys_write), LX_EEXIST);
    LT_STREQ(lkd_syscall_owner(LKD_NR_dup), "modA");
    LT_EQ(lkd_dispatch_conflicts(), 1);
    LT_ERRNO(lkd_register_syscall(LKD_NR_TABLE_SIZE, "x", lkd_sys_read), LX_EINVAL);
    LT_ERRNO(lkd_register_syscall(5, "x", NULL), LX_EINVAL);
    lkd_dispatch_set_init_module("");
    LT_CHECK(lkd_syscall_owner(LKD_NR_dup3) == NULL);
}

LT_TEST(dispositions)
{
    lkd_task *t = fake_make_task();
    int64_t rax = 123;
    lkd_sysreq r = rq(LKD_NR_getpid, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_CONTEXT_REPLACED);
    r = rq(LKD_NR_exit, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_EXIT_THREAD);
    r = rq(LKD_NR_exit_group, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_EXIT_GROUP);
}

LT_TEST(ioctl_enoioctlcmd_becomes_enotty)
{
    lkd_task *t = fake_make_task();
    int64_t rax = 0;
    lkd_sysreq r = rq(LKD_NR_ioctl, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_ENOTTY);
}

LT_TEST(restart_codes_resolve_to_restart_or_eintr)
{
    /* Linux arch/x86/kernel/signal.c: with no signal to deliver, ERESTARTSYS restarts the syscall with
     * the original number; ERESTART_RESTARTBLOCK restarts through restart_syscall (219). A task that
     * must die (LKD_TF_FATAL) is interrupted for good: -EINTR, no restart. */
    lkd_task *t = fake_make_task();
    int64_t rax = 0;
    lkd_sysreq r = rq(LKD_NR_accept, 3, 4, 5, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RESTART);
    LT_EQ(rax, LKD_NR_accept);
    r = rq(LKD_NR_pause, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RESTART);
    LT_EQ(rax, LKD_NR_pause);
    r = rq(LKD_NR_nanosleep, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RESTART);
    LT_EQ(rax, LKD_NR_restart_syscall);

    atomic_fetch_or(&t->flags, LKD_TF_FATAL);
    r = rq(LKD_NR_accept, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_EINTR);
    atomic_fetch_and(&t->flags, ~LKD_TF_FATAL);
}

LT_TEST(restart_syscall_runs_the_stored_continuation)
{
    lkd_task *t = fake_make_task();
    int64_t rax = 0;
    t->restart.fn = NULL;
    lkd_sysreq r = rq(LKD_NR_restart_syscall, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_ERRNO(rax, LX_EINTR); /* do_no_restart_syscall */
    r = rq(LKD_NR_sched_yield, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RESTART);
    LT_EQ(rax, LKD_NR_restart_syscall);
    r = rq(LKD_NR_restart_syscall, 0, 0, 0, 0, 0, 0);
    LT_EQ(lkd_syscall(t, &r, &rax), LKD_RETURN);
    LT_EQ(rax, 77);
    LT_EQ(cont_calls, 1);
}

/* ---- syscall number table ---- */
LT_TEST(syscall_numbers_match_linux_v6_12_table)
{
    LT_EQ(LKD_NR_read, 0); LT_EQ(LKD_NR_write, 1); LT_EQ(LKD_NR_open, 2); LT_EQ(LKD_NR_mmap, 9);
    LT_EQ(LKD_NR_rt_sigaction, 13); LT_EQ(LKD_NR_ioctl, 16); LT_EQ(LKD_NR_clone, 56); LT_EQ(LKD_NR_fork, 57);
    LT_EQ(LKD_NR_execve, 59); LT_EQ(LKD_NR_wait4, 61); LT_EQ(LKD_NR_kill, 62); LT_EQ(LKD_NR_fcntl, 72);
    LT_EQ(LKD_NR_ptrace, 101); LT_EQ(LKD_NR_arch_prctl, 158); LT_EQ(LKD_NR_gettid, 186);
    LT_EQ(LKD_NR_futex, 202); LT_EQ(LKD_NR_restart_syscall, 219); LT_EQ(LKD_NR_exit_group, 231);
    LT_EQ(LKD_NR_epoll_wait, 232); LT_EQ(LKD_NR_tgkill, 234); LT_EQ(LKD_NR_openat, 257);
    LT_EQ(LKD_NR_newfstatat, 262); LT_EQ(LKD_NR_ppoll, 271); LT_EQ(LKD_NR_epoll_pwait, 281);
    LT_EQ(LKD_NR_eventfd2, 290); LT_EQ(LKD_NR_pipe2, 293); LT_EQ(LKD_NR_prlimit64, 302);
    LT_EQ(LKD_NR_getrandom, 318); LT_EQ(LKD_NR_memfd_create, 319); LT_EQ(LKD_NR_execveat, 322);
    LT_EQ(LKD_NR_rseq, 334); LT_EQ(LKD_NR_statx, 332); LT_EQ(LKD_NR_clone3, 435);
    LT_EQ(LKD_NR_close_range, 436); LT_EQ(LKD_NR_openat2, 437); LT_EQ(LKD_NR_faccessat2, 439);
    LT_EQ(LKD_NR_epoll_pwait2, 441); LT_EQ(LKD_NR_futex_waitv, 449); LT_EQ(LKD_NR_mseal, 462);
    LT_EQ(LKD_NR_HIGHEST, 462);
    LT_EQ(LKD_NR_TABLE_SIZE, 512);
    LT_STREQ(lkd_syscall_name(0), "read");
    LT_STREQ(lkd_syscall_name(231), "exit_group");
    LT_STREQ(lkd_syscall_name(318), "getrandom");
    LT_CHECK(lkd_syscall_name(LKD_NR_TABLE_SIZE) == NULL);
    LT_CHECK(lkd_syscall_name(LKD_NR_dup3 + 1000) == NULL);
    LT_EQ(strlen(LKD_SYSCALL_TABLE_SHA256), 64);
    LT_CHECK(strstr(LKD_SYSCALL_TABLE_SOURCE_URL, "/v6.12/arch/x86/entry/syscalls/syscall_64.tbl") != NULL);
    /* the unassigned numbers inside the range have no name (e.g. 335 is uretprobe, 336+ assigned) */
    LT_CHECK(lkd_syscall_name(LKD_NR_mseal + 1) == NULL);
}

/* ---- tracing ---- */
static int capture_begin(void)
{
    fflush(NULL);
    int save = dup(2);
    char path[] = "/tmp/lkd_trace_XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    dup2(fd, 2);
    close(fd);
    return save;
}
static size_t capture_end(int save, char *out, size_t cap)
{
    off_t end = lseek(2, 0, SEEK_CUR);
    lseek(2, 0, SEEK_SET);
    ssize_t n = read(2, out, cap - 1);
    if (n < 0) n = 0;
    out[n] = 0;
    dup2(save, 2);
    close(save);
    (void)end;
    return (size_t)n;
}

LT_TEST(trace_prints_name_args_and_result_without_memory_contents)
{
    lkd_task *t = fake_make_task();
    char secret[64] = "TOPSECRET-guest-memory";
    int64_t rax;
    char out[4096];
    lkd_trace_enable(1);
    int save = capture_begin();
    lkd_sysreq r = rq(LKD_NR_read, (uint64_t)(uintptr_t)secret, 0x20, 0, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    r = rq(LKD_NR_open, 0x1000, 0x42, 0, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    r = rq(LKD_NR_getppid, 0, 0, 0, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    capture_end(save, out, sizeof out);
    lkd_trace_enable(0);
    char want_tid[32];
    snprintf(want_tid, sizeof want_tid, "[%d] ", t->tid);
    LT_CHECK(strstr(out, want_tid) != NULL);
    LT_CHECK(strstr(out, "read(0x") != NULL);
    LT_CHECK(strstr(out, ", 0x20)") != NULL);       /* trailing zero arguments are omitted */
    LT_CHECK(strstr(out, "open(0x1000, 0x42) = -2 (ENOENT)") != NULL);
    LT_CHECK(strstr(out, "getppid() = 0x7fffffffffff0000") != NULL);
    LT_CHECK(strstr(out, "TOPSECRET") == NULL);
    LT_CHECK(strstr(out, "->") == NULL);            /* level 1 prints completions only */
}

LT_TEST(enosys_is_logged_once_per_number_when_tracing)
{
    lkd_task *t = fake_make_task();
    int64_t rax;
    char out[4096];
    lkd_trace_enable(1);
    int save = capture_begin();
    lkd_sysreq r = rq(LKD_NR_mlockall, 0, 0, 0, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    lkd_syscall(t, &r, &rax);
    lkd_syscall(t, &r, &rax);
    capture_end(save, out, sizeof out);
    lkd_trace_enable(0);
    int n = 0;
    for (const char *p = out; (p = strstr(p, "unimplemented syscall")) != NULL; p++) n++;
    LT_EQ(n, 1);
    LT_CHECK(strstr(out, "mlockall") != NULL);
    LT_CHECK(strstr(out, "= -38 (ENOSYS)") != NULL);
}

LT_TEST(trace_level_two_prints_entry_lines_before_blocking_calls)
{
    lkd_task *t = fake_make_task();
    int64_t rax;
    char out[2048];
    lkd_trace_enable(2);
    int save = capture_begin();
    lkd_sysreq r = rq(LKD_NR_write, 1, 0xabc, 5, 0, 0, 0);
    lkd_syscall(t, &r, &rax);
    capture_end(save, out, sizeof out);
    lkd_trace_enable(0);
    LT_CHECK(strstr(out, "-> write(0x1, 0xabc, 0x5)") != NULL);
    LT_CHECK(strstr(out, "write(0x1, 0xabc, 0x5) = -9 (EBADF)") != NULL);
    char *entry = strstr(out, "->"), *done = strstr(out, "= -9");
    LT_CHECK(entry && done && entry < done);
}

/* ---- blocking helpers ---- */
static void *wake_later(void *arg)
{
    lkd_task *t = arg;
    usleep(30000);
    atomic_fetch_or(&t->flags, LKD_TF_FATAL);
    lkd_task_wake(t);
    return NULL;
}

LT_TEST(block_wait_returns_ready_fd_timeout_and_interruption)
{
    lkd_task *t = fake_make_task();
    int p[2];
    LT_EQ(pipe(p), 0);
    /* nothing to read: times out after about 40 ms */
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(lkd_block_wait_fd(t, p[0], POLLIN, t0 + 40000000), 0);
    int64_t dt = lkd_host_mono_ns() - t0;
    LT_CHECK(dt >= 38000000 && dt < 400000000);
    /* data available: returns POLLIN at once */
    LT_EQ(write(p[1], "x", 1), 1);
    LT_EQ(lkd_block_wait_fd(t, p[0], POLLIN, -1), POLLIN);
    char c;
    LT_EQ(read(p[0], &c, 1), 1);
    /* a task woken while waiting forever returns -ERESTARTSYS when interrupted (LKD_TF_FATAL) */
    pthread_t th;
    LT_EQ(pthread_create(&th, NULL, wake_later, t), 0);
    LT_ERRNO(lkd_block_wait_fd(t, p[0], POLLIN, -1), LX_ERESTARTSYS);
    pthread_join(th, NULL);
    /* the interruption check happens before sleeping: a flag set earlier is never lost */
    LT_ERRNO(lkd_block_wait_fd(t, p[0], POLLIN, -1), LX_ERESTARTSYS);
    atomic_fetch_and(&t->flags, ~LKD_TF_FATAL);
    /* a wake without interruption is spurious and the wait continues to its deadline */
    lkd_task_wake(t);
    t0 = lkd_host_mono_ns();
    LT_EQ(lkd_block_wait_fd(t, p[0], POLLIN, t0 + 30000000), 0);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 28000000);
    close(p[0]); close(p[1]);
}

LT_TEST(block_sleep_and_poll_set)
{
    lkd_task *t = fake_make_task();
    int a[2], b[2];
    LT_EQ(pipe(a), 0); LT_EQ(pipe(b), 0);
    int64_t t0 = lkd_host_mono_ns();
    LT_EQ(lkd_block_sleep(t, t0 + 25000000), 0);
    LT_CHECK(lkd_host_mono_ns() - t0 >= 24000000);
    LT_EQ(write(b[1], "y", 1), 1);
    struct pollfd fds[2] = { { a[0], POLLIN, 0 }, { b[0], POLLIN, 0 } };
    LT_EQ(lkd_block_poll(t, fds, 2, -1), 1);
    LT_EQ(fds[0].revents, 0);
    LT_EQ(fds[1].revents, POLLIN);
    LT_ERRNO(lkd_block_poll(t, fds, 2000, 0), LX_EINVAL);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

/* ---- fork hooks ---- */
LT_TEST(fork_host_calls_engine_hooks_in_order_and_forks)
{
    lkd_task *t = fake_make_task();
    fake_engine_reset();
    int p[2];
    LT_EQ(pipe(p), 0);
    fflush(NULL);
    pid_t c = lkd_fork_host(t, 0);
    if (c == 0) {
        /* child: engine prepare and child hooks ran, parent hook did not */
        char buf[4];
        buf[0] = (char)fake_fork_prepare_count;
        buf[1] = (char)fake_fork_child_count;
        buf[2] = (char)fake_fork_parent_count;
        buf[3] = (char)(fake_order_n >= 2 && fake_order[0] == FAKE_EV_FORK_PREPARE && fake_order[1] == FAKE_EV_FORK_CHILD);
        (void)!write(p[1], buf, 4);
        _exit(0);
    }
    LT_CHECK(c > 0);
    char buf[4] = { 0 };
    LT_EQ(read(p[0], buf, 4), 4);
    LT_EQ(buf[0], 1); LT_EQ(buf[1], 1); LT_EQ(buf[2], 0); LT_EQ(buf[3], 1);
    int st;
    LT_EQ(waitpid(c, &st, 0), c);
    LT_CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* parent side */
    LT_EQ(fake_fork_prepare_count, 1);
    LT_EQ(fake_fork_parent_count, 1);
    LT_EQ(fake_fork_child_count, 0);
    LT_EQ(fake_order[0], FAKE_EV_FORK_PREPARE);
    LT_EQ(fake_order[1], FAKE_EV_FORK_PARENT);
    close(p[0]); close(p[1]);
}

int main(void)
{
    init_rc = fake_engine_init();
    registered_after_init = (int)lkd_registered_count();
    owner_restart_ok = lkd_syscall_owner(LKD_NR_restart_syscall) != NULL;
    LT_RUN(modules_init_without_conflict);
    install_test_syscalls();
    LT_RUN(dispatch_passes_number_and_six_args);
    LT_RUN(negative_errno_is_returned_in_rax);
    LT_RUN(unimplemented_and_out_of_range_numbers_return_enosys_and_count);
    LT_RUN(dispositions);
    LT_RUN(ioctl_enoioctlcmd_becomes_enotty);
    LT_RUN(restart_codes_resolve_to_restart_or_eintr);
    LT_RUN(restart_syscall_runs_the_stored_continuation);
    LT_RUN(syscall_numbers_match_linux_v6_12_table);
    LT_RUN(trace_prints_name_args_and_result_without_memory_contents);
    LT_RUN(enosys_is_logged_once_per_number_when_tracing);
    LT_RUN(trace_level_two_prints_entry_lines_before_blocking_calls);
    LT_RUN(block_wait_returns_ready_fd_timeout_and_interruption);
    LT_RUN(block_sleep_and_poll_set);
    LT_RUN(fork_host_calls_engine_hooks_in_order_and_forks);
    LT_RUN(duplicate_registration_is_refused_and_first_owner_kept);
    return lt_finish();
}
