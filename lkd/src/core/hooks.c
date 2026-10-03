/* hooks.c: orchestration of the per-module hooks (fork, exec, exit). Owner: core.
 * The module hook functions are weak no-ops in weak_modules.c; a module overrides the ones it needs. */
#include <unistd.h>

#include "core_priv.h"
#include "task.h"

typedef struct {
    const char *name;
    void (*prepare)(lkd_task *, uint64_t);
    void (*parent)(lkd_task *, pid_t);
    void (*child)(lkd_task *);
    void (*exec_commit)(lkd_task *);
    void (*task_exit)(lkd_task *);
    void (*proc_exit)(lkd_task *);
} modhooks;

#define MH(m) { #m, lkd_##m##_fork_prepare, lkd_##m##_fork_parent, lkd_##m##_fork_child, \
                lkd_##m##_exec_commit, lkd_##m##_task_exit, lkd_##m##_proc_exit }
static const modhooks H_mm = MH(mm), H_fs = MH(fs), H_procfs = MH(procfs), H_task = MH(task),
                      H_sig = MH(sig), H_net = MH(net), H_ptrace = MH(ptrace);

/* Ascending lock rank: mm 1, fs 2-3, procfs (none), sig 4, task 5-6, ptrace 6a, net 7a. */
static const modhooks *const fork_order[] = { &H_mm, &H_fs, &H_procfs, &H_sig, &H_task, &H_ptrace, &H_net };
static const modhooks *const exec_order[] = { &H_task, &H_mm, &H_fs, &H_sig, &H_ptrace, &H_net, &H_procfs };
static const modhooks *const exit_order[] = { &H_ptrace, &H_sig, &H_net, &H_fs, &H_mm, &H_task, &H_procfs };
#define N(a) (sizeof(a) / sizeof((a)[0]))

void lkd_hooks_exec_commit(lkd_task *t)
{
    for (size_t i = 0; i < N(exec_order); i++) exec_order[i]->exec_commit(t);
}

void lkd_hooks_task_exit(lkd_task *t)
{
    for (size_t i = 0; i < N(exit_order); i++) exit_order[i]->task_exit(t);
}

void lkd_hooks_proc_exit(lkd_task *t)
{
    for (size_t i = 0; i < N(exit_order); i++) exit_order[i]->proc_exit(t);
}

void lkd_exec_commit(lkd_task *t, const lkd_exec_image *img)
{
    (void)img;
    lkd_hooks_exec_commit(t);
}

int lkd_fork_prepare(lkd_task *t, uint64_t clone_flags)
{
    const lkd_engine_ops *ops = lkd_engine();
    if (ops && ops->fork_prepare) ops->fork_prepare(t);
    for (size_t i = 0; i < N(fork_order); i++) fork_order[i]->prepare(t, clone_flags);
    return 0;
}

void lkd_fork_finish(lkd_task *t, pid_t host_child)
{
    const lkd_engine_ops *ops = lkd_engine();
    if (host_child == 0) {
        for (size_t i = N(fork_order); i-- > 0;) fork_order[i]->child(t);
        if (ops && ops->fork_child) ops->fork_child(t);
    } else {
        for (size_t i = N(fork_order); i-- > 0;) fork_order[i]->parent(t, host_child);
        if (ops && ops->fork_parent) ops->fork_parent(t, host_child);
    }
}

pid_t lkd_fork_host(lkd_task *t, uint64_t clone_flags)
{
    int e = lkd_fork_prepare(t, clone_flags);
    if (e < 0) return e;
    pid_t p = fork();
    if (p < 0) {
        int64_t err = LKD_HOST_ERR();
        lkd_fork_finish(t, -1);
        return (pid_t)err;
    }
    lkd_fork_finish(t, p);
    return p;
}

void lkd_fork_reinit_mutex(pthread_mutex_t *m, int recursive)
{
    memset(m, 0, sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (recursive) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

void lkd_fork_reinit_rwlock(pthread_rwlock_t *l)
{
    memset(l, 0, sizeof *l);
    pthread_rwlock_init(l, NULL);
}
