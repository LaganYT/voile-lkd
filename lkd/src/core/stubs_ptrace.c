/* stubs_ptrace.c: weak placeholders for the ptrace module. Owner: core until ptrace overrides them.
 * Untraced tasks never stop. The access check is honest: only a process may look at itself. */
#include "ptrace.h"
#include "task.h"

LKD_WEAK int lkd_ptrace_syscall_enter(lkd_task *t, lkd_sysreq *rq, int64_t *skip_ret)
{ (void)t; (void)rq; (void)skip_ret; return 0; }
LKD_WEAK void lkd_ptrace_syscall_exit(lkd_task *t, int64_t *ret) { (void)t; (void)ret; }
LKD_WEAK void lkd_ptrace_syscall_exit_replaced(lkd_task *t) { (void)t; }
LKD_WEAK int lkd_ptrace_signal_stop(lkd_task *t, int signo, struct lx_siginfo *info) { (void)t; (void)info; return signo; }
LKD_WEAK int lkd_ptrace_event(lkd_task *t, int event, uint64_t msg) { (void)t; (void)event; (void)msg; return 0; }
LKD_WEAK void lkd_ptrace_task_init(lkd_task *child, lkd_task *parent, uint64_t clone_flags)
{ (void)child; (void)parent; (void)clone_flags; }
LKD_WEAK int lkd_ptrace_wait_event(lkd_task *waiter, int32_t sel_pid, unsigned options, struct lkd_wait_event *ev)
{ (void)waiter; (void)sel_pid; (void)options; (void)ev; return 0; }
LKD_WEAK int lkd_ptrace_has_tracees(lkd_task *waiter) { (void)waiter; return 0; }
LKD_WEAK int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options)
{ (void)waiter; (void)sel_pid; (void)options; return 0; }
LKD_WEAK int lkd_ptrace_trap_stop(lkd_task *t) { atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP); return 0; }
LKD_WEAK void lkd_ptrace_task_dead(lkd_task *t) { (void)t; }
LKD_WEAK int32_t lkd_ptrace_tracer_of(int32_t tid) { (void)tid; return 0; }
LKD_WEAK int lkd_ptrace_check_access(lkd_task *caller, int32_t pid, unsigned mode)
{
    (void)mode;
    return caller && caller->proc && caller->proc->pid == pid ? 0 : -LX_EPERM;
}
LKD_WEAK int lkd_ptrace_stop(lkd_task *t, lkd_stop_kind kind, int signo, int *resume_signo)
{
    (void)t; (void)kind;
    if (resume_signo) *resume_signo = signo;
    return 0;
}
