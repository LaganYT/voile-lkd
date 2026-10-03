/* ptrace.h: cooperative user-space ptrace between processes of the family. OWNER: ptrace.
 * Core provides weak no-op placeholders (src/core/stubs_ptrace.c): untraced tasks never stop.
 *
 * Design (reports/hl-reuse-process.md 6.3): the tracee publishes its stop (kind, signal, registers)
 * in shared state and parks at a safe point; the tracer's ptrace() calls read that state or post
 * commands the tracee (or a helper thread of the tracee's process) executes. The tracer never touches
 * the tracee's memory directly: PEEK/POKE and /proc/<pid>/mem go through the mm module's cross-process
 * service (lkd_mm_remote_*), and code POKEs call ops->invalidate_code in the tracee before it resumes.
 */
#ifndef LKD_PTRACE_H
#define LKD_PTRACE_H

#include "lkd_internal.h"

struct lkd_wait_event;

/* Bits of lkd_ptrace_task.flags. The hot ones are tested by lkd_syscall on every call. */
#define LKD_PT_TRACED        0x01u /* has a tracer */
#define LKD_PT_SYSCALL_TRACE 0x02u /* PTRACE_SYSCALL in effect: stop at syscall entry and exit */
#define LKD_PT_STOPPED       0x04u /* parked in a ptrace stop right now */
#define LKD_PT_SEIZED        0x08u
#define LKD_PT_TRACER        0x10u /* this task has tracees */
#define LKD_PT_SINGLESTEP    0x20u

/* The ptrace link embedded in every task. `state` is ptrace-owned (tracer/tracee records). */
struct lkd_ptrace_task {
    _Atomic uint32_t flags;
    int32_t tracer_tid;   /* mirror for fast /proc TracerPid; authoritative copy is in the shared state */
    void *state;
};

/* ---- Syscall boundary (called by core/dispatch.c) ---------------------------------------------- */

/* Syscall-entry stop. Called only when LKD_PT_SYSCALL_TRACE is set. rq is in/out: the tracer may
 * rewrite the number and arguments (through SETREGS) before the syscall runs. At the stop the tracee
 * view is rax = -ENOSYS, orig_rax = number, rcx = rip after SYSCALL, r11 = rflags (the engine's
 * get_regs supplies the rest; ptrace patches those fields in the view it serves). Returns 0 to run the
 * syscall, 1 to skip it (tracer set orig_rax = -1; *skip_ret is then the tracer's rax, which becomes the
 * syscall result), -1 if the task was killed in the stop. May block. */
int lkd_ptrace_syscall_enter(lkd_task *t, lkd_sysreq *rq, int64_t *skip_ret);
/* Syscall-exit stop: *ret is the result about to be written, the tracer may change it. */
void lkd_ptrace_syscall_exit(lkd_task *t, int64_t *ret);
/* Syscall-exit stop of a call that replaced the context (execve, execveat, rt_sigreturn: the handler
 * returned LKD_RET_CONTEXT_REPLACED, request ptrace-1). The view is the new context: rcx and r11 are not
 * the SYSCALL ones, rax is 0 after an exec and the restored rax after rt_sigreturn. */
void lkd_ptrace_syscall_exit_replaced(lkd_task *t);

/* ---- Events raised by other modules ------------------------------------------------------------ */

/* Signal-delivery-stop for `signo` (called by lkd_signal_next before the action runs). *info may be
 * rewritten by the tracer (SETSIGINFO). Returns the signal to deliver (0 = suppressed by CONT(0)),
 * or -1 if the task was killed while stopped. Not called for untraced tasks (returns signo). */
int lkd_ptrace_signal_stop(lkd_task *t, int signo, struct lx_siginfo *info);
/* PTRACE_EVENT_* stop (fork, vfork, clone, exec, vfork_done, exit) if the matching PTRACE_O_TRACE*
 * option is set; msg is the value PTRACE_GETEVENTMSG returns. Returns 1 if a stop happened. */
int lkd_ptrace_event(lkd_task *t, int event, uint64_t msg);
/* Called from lkd_ptrace_task_init in the clone path: the child inherits the tracer when the event
 * option or CLONE_PTRACE says so (auto-attach), and starts stopped with SIGSTOP in that case. */
void lkd_ptrace_task_init(lkd_task *child, lkd_task *parent, uint64_t clone_flags);

/* ---- Cooperation with wait4/waitid (task) ------------------------------------------------------ */

/* Look for a stop or exit notification of a TRACEE of `waiter`'s process matching sel_pid (wait4
 * semantics: > 0 that tid, -1 any, 0 same pgrp, < -1 pgrp -sel_pid) and the wait options. Returns 1 and
 * fills *ev (consuming the event unless LX_WNOWAIT), 0 if none now, -LX_ECHILD never (task decides).
 * Wait statuses are Linux-exact: 0x57f, 0x857f with TRACESYSGOOD, (event << 16) | 0x57f for events. */
int lkd_ptrace_wait_event(lkd_task *waiter, int32_t sel_pid, unsigned options, struct lkd_wait_event *ev);
/* Does `waiter`'s process have tracees (so that wait4 does not fail with ECHILD)? */
int lkd_ptrace_has_tracees(lkd_task *waiter);
/* Same, restricted to the tracees the wait selector (sel_pid as above) and options (__WALL, __WCLONE,
 * __WNOTHREAD) can name: wait4 answers ECHILD when neither a child nor such a tracee exists. ANY. */
int lkd_ptrace_wait_has(lkd_task *waiter, int32_t sel_pid, unsigned options);

/* ---- PTRACE_INTERRUPT (called by sig) ------------------------------------------------------------ */

/* lkd_signal_next saw LKD_TF_TRACE_STOP (cleared by the caller): stop in PTRACE_EVENT_STOP if the task
 * is traced. Returns 0, or -1 if the task was killed in the stop. OWNER, no LKD lock held. */
int lkd_ptrace_trap_stop(lkd_task *t);
/* 1 when lkd_signal_next honors LKD_TF_TRACE_STOP (strong in sig), weak 0 in ptrace. */
int lkd_sig_trace_stop_supported(void);

/* ---- Lifecycle and queries --------------------------------------------------------------------- */

/* Thread exit (lkd_<m>_task_exit hook does this too): tracee death publishes the exit to its tracer;
 * tracer death detaches every tracee (EXITKILL ones get SIGKILL), they continue running. */
void lkd_ptrace_task_dead(lkd_task *t);
/* TracerPid of tid (any process of the family), 0 if untraced. Lock-free. */
int32_t lkd_ptrace_tracer_of(int32_t tid);
/* Linux ptrace_may_access(): may `caller` read the memory / maps / fds of pid? mode is
 * LKD_PTRACE_MODE_READ or LKD_PTRACE_MODE_ATTACH. Same uid and dumpable, or CAP_SYS_PTRACE, plus the
 * Yama scope (documented in the ptrace module). Returns 0 or -LX_EPERM/-LX_EACCES/-LX_ESRCH. Used by
 * procfs (mem, maps, environ, fd) and process_vm_readv/writev. */
#define LKD_PTRACE_MODE_READ 1u
#define LKD_PTRACE_MODE_ATTACH 2u
int lkd_ptrace_check_access(lkd_task *caller, int32_t pid, unsigned mode);

#endif /* LKD_PTRACE_H */
