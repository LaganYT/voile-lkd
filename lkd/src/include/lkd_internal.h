/* lkd_internal.h: what every LKD module includes. Owner: core. Do not edit (write a request).
 *
 * Provides the syscall registration macros, logging, the module hook list, the blocking-wait helpers
 * and the lock-rank table. Internal headers (this directory) are not part of the engine API.
 */
#ifndef LKD_INTERNAL_H
#define LKD_INTERNAL_H

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lkd/abi_x64.h>
#include <lkd/lkd.h>
#include <lkd/xlate.h>

/* ---- Syscall registration --------------------------------------------------------------------- */

/* A handler gets the calling task and the request (nr, a[0..5]) and returns the guest RAX: a value, or
 * -LX_E*. Special returns: LKD_RET_* below (dispositions) and -LX_ERESTART* (interrupted wait). */
typedef int64_t (*lkd_sysfn)(lkd_task *t, const lkd_sysreq *rq);

/* How to add a syscall: define it with LKD_SYSCALL(name) and register it in lkd_<module>_init():
 *     LKD_SYSCALL(getpid) { return t->proc->pid; }
 *     void lkd_task_init(void) { LKD_REGISTER(getpid); }
 * The name must be one of the generated LKD_NR_<name> (include/lkd/syscall_nr.h). */
/* Handlers are static (fs-1 point 5, task-1 point 5): only LKD_REGISTER in the same file names them, so
 * two files (a module and a test) may both define a handler of the same name. */
#define LKD_SYSCALL(name) static int64_t lkd_sys_##name(lkd_task *t, const lkd_sysreq *rq)
#define LKD_REGISTER(name) lkd_register_syscall(LKD_NR_##name, #name, lkd_sys_##name)

/* Typed views of the arguments. */
#define LKD_ARGU(i) ((uint64_t)rq->a[i])
#define LKD_ARGL(i) ((int64_t)rq->a[i])
#define LKD_ARGI(i) ((int)(int32_t)rq->a[i])       /* C int: the low 32 bits, sign-extended */
#define LKD_ARGUI(i) ((unsigned)(uint32_t)rq->a[i])
#define LKD_ARGP(i) ((uint64_t)rq->a[i])           /* a guest pointer: pass to lkd_user_* only */

/* Dispositions returned through the handler's int64 result. Impossible as real results because
 * they are below -4095. */
#define LKD_RET_CONTEXT_REPLACED ((int64_t)INT64_MIN + 1)
#define LKD_RET_EXIT_THREAD ((int64_t)INT64_MIN + 2)
#define LKD_RET_EXIT_GROUP ((int64_t)INT64_MIN + 3)

/* Register a handler. Returns 0, -LX_EEXIST if another module already owns nr (the first owner is
 * kept, the conflict is recorded and lkd_init() fails with -LX_EEXIST), -LX_EINVAL for a bad number.
 * Call from lkd_<module>_init() only (single-threaded init); the table is read-only afterwards. */
int lkd_register_syscall(unsigned nr, const char *name, lkd_sysfn fn);
/* Module that registered nr ("mm", "fs", ...), or NULL. */
const char *lkd_syscall_owner(unsigned nr);
/* Name of the module whose init is running (for diagnostics), "" outside init. */
const char *lkd_current_init_module(void);

/* Number of registered syscalls. */
unsigned lkd_registered_count(void);

/* Dispatch one request without the task bookkeeping, ptrace stops and tracing. For unit tests and for
 * modules that implement one syscall on top of another (open -> openat). Same result encoding as
 * a handler (not the disposition). */
int64_t lkd_dispatch(lkd_task *t, const lkd_sysreq *rq);

/* The configuration given to lkd_init_config (requests fs-1, mm-1, task-1). Valid after lkd_init_config. */
const lkd_config *lkd_config_get(void);

/* Atomic compare-and-swap of the aligned guest word at gva (futex WAKE_OP, PI, robust lists; request
 * sig-1): 0 when *expected matched and desired was stored, 1 when the word differed (*expected is set to
 * it), -LX_EFAULT (unmapped, not writable, or a concurrent munmap: the fault is recovered like
 * lkd_user_*), -LX_EINVAL when gva is not 4-byte aligned. ANY. */
int lkd_user_cmpxchg32(uint64_t gva, uint32_t *expected, uint32_t desired);

/* ---- Test hook for lkd_user_* ----------------------------------------------------------------- */

/* Replace the range check lkd_user_* calls before copying (NULL restores lkd_mm_check_range). Tests that
 * use raw host memory as guest memory install a permissive function: the mm module's real check rejects
 * anything its VMA table does not know. Tests of mm itself leave the default. */
void lkd_usermem_set_check_fn(int (*fn)(uint64_t gva, size_t len, int need_write));

/* ---- Errors ----------------------------------------------------------------------------------- */

/* Negative Linux errno from the current host errno. */
#define LKD_HOST_ERR() (-(int64_t)lkd_errno_from_darwin(errno))
/* Wrap the result of a host call that returns -1 on error. */
#define LKD_HOST_RET(r) ((r) < 0 ? LKD_HOST_ERR() : (int64_t)(r))

/* ---- Logging ---------------------------------------------------------------------------------- */

/* One line on stderr, prefixed "lkd: ". Serialized; safe in any thread, not in signal handlers. */
void lkd_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Same, but only the first time this call site runs. */
#define lkd_log_once(...) do { static _Atomic int lkd_once_; \
    if (!atomic_exchange(&lkd_once_, 1)) lkd_log(__VA_ARGS__); } while (0)
/* Internal invariant violated: log and abort. Used for bugs, never for guest-triggerable errors. */
#define LKD_BUG(cond, fmt, ...) do { if (__builtin_expect(!(cond), 0)) { \
    lkd_log("BUG %s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); abort(); } } while (0)
#define LKD_UNUSED __attribute__((unused))
#define LKD_WEAK __attribute__((weak))   /* placeholder definitions in src/core/stubs_*.c, overridden by the owner */

/* Host monotonic clock in nanoseconds (CLOCK_MONOTONIC_RAW). Deadlines of the blocking helpers use it. */
int64_t lkd_host_mono_ns(void);

/* ---- Module hooks ----------------------------------------------------------------------------- */

/* Every module <m> in {mm, fs, procfs, task, sig, net, ptrace} may define these; core defines weak
 * no-ops in src/core/weak_modules.c, so a module defines only the ones it needs (strong symbols win).
 *
 *   lkd_<m>_init(void)                 register syscalls and set up module state. Init order:
 *                                      mm, fs, procfs, task, sig, net, ptrace.
 *   lkd_<m>_fork_prepare(t, flags)     take every lock the module owns, ascending lock rank, so that
 *                                      the host fork() copies a consistent state. Never blocks long.
 *   lkd_<m>_fork_parent(t, host_child) release them in the parent. host_child < 0: fork failed.
 *   lkd_<m>_fork_child(t)              in the child: release or re-initialize locks (their holders,
 *                                      other threads, no longer exist), rebuild host objects that fork
 *                                      does not inherit (kqueues), drop state of other threads.
 *                                      Fork order: prepare in {mm, fs, procfs, sig, task, ptrace, net};
 *                                      parent and child in the reverse order.
 *   lkd_<m>_exec_commit(t)             from lkd_exec_commit, in the order task, mm, fs, sig, ptrace,
 *                                      net, procfs: the module resets its per-process state for exec.
 *   lkd_<m>_task_exit(t)               a guest thread is leaving (exit, exit_group, fatal signal), on its
 *                                      own host thread, in the order ptrace, sig, net, fs, mm, task,
 *                                      procfs. Release per-thread state; last-thread cleanup is
 *                                      lkd_<m>_proc_exit.
 *   lkd_<m>_proc_exit(t)               the last thread of the process is leaving: release process-wide
 *                                      objects (fd table, mm), same order.
 */
#define LKD_MODULES(X) X(mm) X(fs) X(procfs) X(task) X(sig) X(net) X(ptrace)

#define LKD_DECLARE_MODULE_HOOKS(m) \
    void lkd_##m##_init(void); \
    void lkd_##m##_fork_prepare(lkd_task *t, uint64_t clone_flags); \
    void lkd_##m##_fork_parent(lkd_task *t, pid_t host_child); \
    void lkd_##m##_fork_child(lkd_task *t); \
    void lkd_##m##_exec_commit(lkd_task *t); \
    void lkd_##m##_task_exit(lkd_task *t); \
    void lkd_##m##_proc_exit(lkd_task *t);
LKD_MODULES(LKD_DECLARE_MODULE_HOOKS)
#undef LKD_DECLARE_MODULE_HOOKS

/* Run a hook of every module in the documented order (core/hooks.c). The task module calls these from
 * its exit and exec paths; core's lkd_exec_commit calls lkd_hooks_exec_commit. */
void lkd_hooks_exec_commit(lkd_task *t);
/* Fork child: give back a lock that the parent's forking thread held across fork() (fork_prepare). Never
 * unlock it in the child: when another thread was waiting for it at fork time, the lock word carries
 * kernel wait state (psynch) that the child does not have, and unlocking or locking it later blocks for
 * ever (MESURE, witness w18_forkjit). The memory is zeroed then initialized (recursive != 0: recursive
 * mutex). Only the forking thread exists in the child. */
void lkd_fork_reinit_mutex(pthread_mutex_t *m, int recursive);
void lkd_fork_reinit_rwlock(pthread_rwlock_t *l);
void lkd_hooks_task_exit(lkd_task *t);
void lkd_hooks_proc_exit(lkd_task *t);

/* ---- Blocking helpers (core, src/core/block.c) ------------------------------------------------ */

/* Every blocking syscall waits through these (or through a primitive it makes interruptible by the
 * same rules) so that a signal, a kick or a group exit ends the wait. Each task owns a wake pipe.
 *
 * lkd_task_wake(t): ANY thread, async-signal-safe, never blocks. Makes the current or next
 * lkd_block_wait_* of t return -LX_ERESTARTSYS if lkd_sig_interrupted(t), else re-evaluates.
 * The sig module calls it when it queues a signal for t, the engine's kick path may call it too.
 *
 * lkd_block_wait_fd: OWNER. Waits until host_fd has one of `events` (POLLIN/POLLOUT host values from
 * <poll.h>), or deadline_ns (lkd_host_mono_ns() scale, < 0 = no deadline) passes, or t is
 * interrupted. Returns the host revents mask (> 0), 0 on timeout, -LX_ERESTARTSYS when interrupted
 * (the caller returns -LX_EINTR instead when its syscall is never restarted, see CONVENTIONS), or
 * -errno. Takes no lock. The signal check happens BEFORE sleeping, so a signal queued earlier is
 * never lost (the wake pipe carries the race window). */
void lkd_task_wake(lkd_task *t);
/* Called by lkd_task_wake after the pipe write (request sig-1): weak no-op in core, the sig module rings
 * the shared-futex wait of t, which does not sleep on the pipe. Async-signal-safe, never blocks. */
void lkd_sig_wake_hook(lkd_task *t);
int lkd_block_wait_fd(lkd_task *t, int host_fd, short events, int64_t deadline_ns);
/* Same on several fds; revents are written back (host values). Returns the count of ready fds, 0 on
 * timeout, -LX_ERESTARTSYS, or -errno. nfds <= 1024. */
struct pollfd;
int lkd_block_poll(lkd_task *t, struct pollfd *fds, unsigned nfds, int64_t deadline_ns);
/* Sleep until deadline_ns or interruption: 0 at the deadline, -LX_ERESTARTSYS if interrupted. */
int lkd_block_sleep(lkd_task *t, int64_t deadline_ns);
/* Wait for the next lkd_task_wake(t) itself (request ptrace-2): 1 as soon as a wake byte is consumed and
 * t is not interrupted, 0 at the deadline, -LX_ERESTARTSYS if interrupted. For waits whose condition
 * lives elsewhere (wait4 rereads the registry after each wake); the caller re-checks its condition. */
int lkd_block_wait_wake(lkd_task *t, int64_t deadline_ns);

/* ---- Lock ranks ------------------------------------------------------------------------------- */

/* Acquire in ascending rank, never while holding a higher rank (details: docs/CONVENTIONS.md).
 *   1   mm (mmap_lock)            1a  shm table           (VMA leaf lock is rank 9)
 *   2   vfs mount table / fsctx   2a  dev registry
 *   3   fd table                  3a  fd object lock
 *   4   sig (per process state)   4a  per-task pending queue
 *   5   task registry (threads)   5a  proc registry (shared, fcntl byte-range lock)
 *   6   wait/process state        6a  ptrace tracer/tracee state
 *   7   futex bucket (by index)   7a  epoll/inotify/timer tables
 *   8   leaf: logging, trace, stats
 *   9   leaf: mm VMA read/write lock (taken by lkd_mm_check_range; safe under anything)
 * A lock must never be held across a guest syscall, a blocking wait, lkd_user_* of a different
 * process, or any engine callback marked "without LKD locks". */
enum lkd_lock_rank {
    LKD_RANK_MM = 10, LKD_RANK_SHM = 11,
    LKD_RANK_VFS = 20, LKD_RANK_DEV = 21,
    LKD_RANK_FDTAB = 30, LKD_RANK_FDOBJ = 31,
    LKD_RANK_SIG = 40, LKD_RANK_SIGQ = 41,
    LKD_RANK_TASKS = 50, LKD_RANK_PROCREG = 51,
    LKD_RANK_WAIT = 60, LKD_RANK_PTRACE = 61,
    LKD_RANK_FUTEX = 70, LKD_RANK_EVENTS = 71,
    LKD_RANK_LEAF = 80, LKD_RANK_VMA = 90
};

#endif /* LKD_INTERNAL_H */
