/* test_core_contract.c: the behaviors the core headers promise to every module, checked against whatever
 * implements them (core placeholders now, the owning module once it exists). Models: Linux pid/tid
 * rules (kernel/pid.c: tgid == tid of the leader, ids unique), POSIX fd rules (dup returns the lowest
 * free descriptor, dup2 closes the target, FD_CLOEXEC is per descriptor: fcntl(2), dup(2) man pages),
 * clone(2) CLONE_FILES / CLONE_THREAD sharing. */
#include <sys/wait.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "fd.h"
#include "mm.h"
#include "sig.h"
#include "vfs.h"

static int released;
static void rel(lkd_fdobj *o) { (void)o; released++; }
static const lkd_fd_ops test_ops = { .name = "test", .release = rel };

static lkd_task *main_task(void)
{
    static const char *argv[] = { "/bin/test", "arg1", NULL };
    lkd_main_spec spec = { .size = sizeof spec, .exe_path = "/bin/test", .argv = argv, .cwd = "/" };
    lkd_task *t = lkd_task_create_main(&spec);
    if (t) lkd_set_current(t);
    return t;
}

LT_TEST(family_ids_are_unique_across_fork)
{
    LT_EQ(lkd_family_create(100), 0);
    int32_t a = lkd_pid_alloc(), b = lkd_pid_alloc();
    LT_CHECK(a > 0 && b > a);
    LT_CHECK(a >= 2 && b <= LKD_PID_MAX);
    int p[2];
    LT_EQ(pipe(p), 0);
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        int32_t ids[10];
        for (int i = 0; i < 10; i++) ids[i] = lkd_pid_alloc();
        (void)!write(p[1], ids, sizeof ids);
        _exit(0);
    }
    int32_t mine[10], theirs[10];
    for (int i = 0; i < 10; i++) mine[i] = lkd_pid_alloc();
    LT_EQ(read(p[0], theirs, sizeof theirs), (ssize_t)sizeof theirs);
    waitpid(c, NULL, 0);
    int dup = 0;
    for (int i = 0; i < 10; i++) for (int j = 0; j < 10; j++) if (mine[i] == theirs[j]) dup++;
    LT_EQ(dup, 0);   /* one counter in shared memory: parent and forked child never get the same id */
    close(p[0]); close(p[1]);
}

LT_TEST(main_task_tid_equals_pid_and_threads_share_the_process)
{
    lkd_task *t = main_task();
    LT_CHECK(t != NULL);
    if (!t) return;
    LT_EQ(t->tid, t->proc->pid);
    LT_EQ(lkd_task_tid(t), lkd_task_pid(t));
    LT_CHECK(t->proc->pid >= 2);
    LT_EQ(t->magic, LKD_TASK_MAGIC);
    LT_EQ(atomic_load(&t->cur_nr), -1);
    LT_CHECK(t->wake_rd >= 0 && t->wake_wr >= 0);
    LT_EQ(lkd_current(), t);
    atomic_store(&t->sigmask, LX_SIGBIT(LX_SIGUSR1));
    lkd_task *th = fake_make_thread(t);
    LT_CHECK(th != NULL);
    if (!th) return;
    LT_CHECK(th->proc == t->proc);                 /* CLONE_THREAD: same process */
    LT_CHECK(th->tid != t->tid);
    LT_CHECK(th->tid > 0);
    LT_EQ(th->proc->nthreads, 2);
    LT_EQ(atomic_load(&th->sigmask), LX_SIGBIT(LX_SIGUSR1));   /* the signal mask is inherited */
    LT_CHECK(th->fdt == t->fdt);                    /* CLONE_FILES: one table */
    LT_CHECK(th->mm == t->mm);                      /* CLONE_VM */
    lkd_task *other = lkd_task_alloc(t, 0);        /* a new process (fork/clone without CLONE_THREAD) */
    LT_CHECK(other != NULL);
    if (!other) return;
    LT_CHECK(other->proc != t->proc);
    LT_EQ(other->tid, other->proc->pid);
    LT_CHECK(other->proc->pid != t->proc->pid);
    LT_EQ(other->proc->ppid, t->proc->pid);
    LT_EQ(other->proc->cred.uid, t->proc->cred.uid);
}

LT_TEST(published_tasks_can_be_looked_up_until_unpublished)
{
    lkd_task *t = main_task();
    if (!t) { LT_CHECK(0); return; }
    /* lkd_task_create_main published the task already */
    lkd_task *f = lkd_task_lookup(t->tid);
    LT_CHECK(f == t);
    if (f) lkd_task_put(f);
    lkd_proc_info pi;
    LT_EQ(lkd_proc_lookup(t->proc->pid, &pi), 0);
    LT_EQ(pi.pid, t->proc->pid);
    LT_EQ(pi.uid, t->proc->cred.uid);
    LT_STREQ(pi.exe, "/bin/test");
    lkd_tid_info ti;
    LT_EQ(lkd_tid_lookup(t->tid, &ti), 0);
    LT_EQ(ti.tgid, t->proc->pid);
    LT_EQ(ti.tid, t->tid);
    LT_ERRNO(lkd_proc_lookup(0x3ffffff0, &pi), LX_ESRCH);
    LT_ERRNO(lkd_tid_lookup(0x3ffffff0, &ti), LX_ESRCH);
    LT_CHECK(lkd_task_lookup(0x3ffffff0) == NULL);
    lkd_proc_info list[64];
    int n = lkd_proc_list(list, 64);
    int seen = 0;
    for (int i = 0; i < n; i++) if (list[i].pid == t->proc->pid) seen++;
    LT_EQ(seen, 1);
    int32_t tids[8];
    LT_CHECK(lkd_tid_list(t->proc->pid, tids, 8) >= 1);
    lkd_task_unpublish(t);
    LT_CHECK(lkd_task_lookup(t->tid) == NULL);
}

LT_TEST(fd_table_posix_semantics)
{
    lkd_task *t = main_task();
    if (!t || !t->fdt) { LT_CHECK(0); return; }
    released = 0;
    lkd_fdobj *a = lkd_fdobj_new(LKD_FD_FILE, &test_ops, -1, LX_O_RDWR, NULL);
    lkd_fdobj *b = lkd_fdobj_new(LKD_FD_PIPE, &test_ops, -1, LX_O_RDONLY, NULL);
    LT_CHECK(a && b);
    if (!a || !b) return;
    LT_EQ(a->host_fd, -1);
    LT_EQ(a->notify_fd, -1);
    LT_EQ(a->kind, LKD_FD_FILE);
    int fa = lkd_fd_alloc(t, a, 0, 0);
    int fb = lkd_fd_alloc(t, b, 0, 1);
    LT_EQ(fa, 0);                     /* lowest free descriptor */
    LT_EQ(fb, 1);
    LT_EQ(lkd_fd_alloc(t, a, 10, 0), 10);   /* F_DUPFD semantics: lowest free >= min */
    int cx = -1;
    lkd_fdobj *g = lkd_fd_get(t, 1, &cx);
    LT_CHECK(g == b);
    LT_EQ(cx, 1);
    lkd_fdobj_put(g);
    g = lkd_fd_get(t, 0, &cx);
    LT_CHECK(g == a);
    LT_EQ(cx, 0);
    lkd_fdobj_put(g);
    LT_CHECK(lkd_fd_get(t, 2, NULL) == NULL);
    LT_CHECK(lkd_fd_get(t, 999999, NULL) == NULL);
    LT_CHECK(lkd_fd_get(t, -1, NULL) == NULL);
    /* dup: lowest free fd, same description, FD_CLOEXEC cleared unless asked */
    int d = lkd_fd_dup(t, 1, -1, 0, 0);
    LT_EQ(d, 2);
    lkd_fdobj *gd = lkd_fd_get(t, d, &cx);
    LT_CHECK(gd == b);
    LT_EQ(cx, 0);
    lkd_fdobj_put(gd);
    /* dup2 onto an open descriptor closes it first; dup2(fd, fd) is a no-op that returns fd */
    LT_EQ(lkd_fd_dup(t, 0, 2, 0, 0), 2);
    gd = lkd_fd_get(t, 2, NULL);
    LT_CHECK(gd == a);
    lkd_fdobj_put(gd);
    LT_ERRNO(lkd_fd_dup(t, 77, -1, 0, 0), LX_EBADF);
    LT_ERRNO(lkd_fd_close(t, 77), LX_EBADF);
    LT_EQ(lkd_fd_count(t), 4);
    int fds[16];
    LT_EQ(lkd_fd_list(t, fds, 16), 4);
    LT_EQ(fds[0], 0); LT_EQ(fds[1], 1); LT_EQ(fds[2], 2); LT_EQ(fds[3], 10);
    /* exec: only close-on-exec descriptors go (fd 1 has FD_CLOEXEC) */
    LT_EQ(lkd_fd_set_cloexec(t, 10, 1), 0);
    lkd_fd_close_on_exec(t);
    LT_CHECK(lkd_fd_get(t, 1, NULL) == NULL);
    LT_CHECK(lkd_fd_get(t, 10, NULL) == NULL);
    lkd_fdobj *k = lkd_fd_get(t, 0, NULL);
    LT_CHECK(k == a);
    lkd_fdobj_put(k);
    /* the description is released when the last reference goes */
    LT_EQ(released, 0);                 /* b: fd 1 is gone but the creator still holds a reference */
    lkd_fdobj_put(b);
    LT_EQ(released, 1);                 /* last reference: ops->release ran exactly once */
    LT_EQ(lkd_fd_close(t, 0), 0);
    LT_EQ(lkd_fd_close(t, 2), 0);
    LT_EQ(released, 1);
    lkd_fdobj_put(a);
    LT_EQ(released, 2);
}

LT_TEST(fd_table_clone_shares_or_copies_per_clone_files)
{
    lkd_task *t = main_task();
    if (!t || !t->fdt) { LT_CHECK(0); return; }
    released = 0;
    lkd_fdobj *a = lkd_fdobj_new(LKD_FD_FILE, &test_ops, -1, LX_O_RDWR, NULL);
    LT_EQ(lkd_fd_alloc(t, a, 3, 0), 3);
    lkd_fdobj_put(a);                   /* the table holds the only reference now */
    lkd_fdtable *same = lkd_fdtable_clone(t->fdt, LX_CLONE_FILES);
    LT_CHECK(same == t->fdt);
    lkd_fdtable_put(same);
    lkd_fdtable *copy = lkd_fdtable_clone(t->fdt, 0);
    LT_CHECK(copy != NULL && copy != t->fdt);
    lkd_task *child = lkd_task_alloc(t, 0);
    LT_CHECK(child != NULL);
    if (!copy || !child) return;
    child->fdt = copy;
    lkd_fdobj *x = lkd_fd_get(child, 3, NULL);
    lkd_fdobj *y = lkd_fd_get(t, 3, NULL);
    LT_CHECK(x != NULL && x == y);     /* the same open file description in both tables */
    lkd_fdobj_put(x); lkd_fdobj_put(y);
    LT_EQ(lkd_fd_close(child, 3), 0);   /* closing in the copy leaves the parent's descriptor */
    LT_EQ(released, 0);
    y = lkd_fd_get(t, 3, NULL);
    LT_CHECK(y != NULL);
    lkd_fdobj_put(y);
    LT_EQ(lkd_fd_close(t, 3), 0);
    LT_EQ(released, 1);
}

int main(void)
{
    if (fake_engine_init() != 0) { printf("FAIL init: lkd_init failed\n"); return 1; }
    LT_RUN(family_ids_are_unique_across_fork);
    LT_RUN(main_task_tid_equals_pid_and_threads_share_the_process);
    LT_RUN(published_tasks_can_be_looked_up_until_unpublished);
    LT_RUN(fd_table_posix_semantics);
    LT_RUN(fd_table_clone_shares_or_copies_per_clone_files);
    return lt_finish();
}
