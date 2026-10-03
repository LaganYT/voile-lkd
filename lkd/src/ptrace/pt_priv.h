/* pt_priv.h: private declarations of the ptrace module (src/ptrace).
 *
 * Model. A ptrace link is one slot of a family-wide table in a shared file mapping (transport.c). The
 * tracee publishes each stop in its slot (kind, Linux exit_code, event message, a register and FP
 * snapshot taken by its own host thread, the siginfo) and parks on its wake pipe. The tracer's
 * ptrace() requests read the slot or write commands into it (registers, FP state, siginfo, signal
 * mask, resume signal and mode) while the tracee is parked; the tracee applies them itself when it
 * resumes. Wake-ups cross host processes through the family service (famsvc opcodes of the ptrace
 * range). Guest memory of the tracee is read and written by the mm module's cross-process service.
 *
 * Locks. One process-local mutex (rank LKD_RANK_PTRACE) plus one fcntl byte-range lock on the table
 * file (released by the host kernel when a holder dies). Both are taken together by pt_lock(). No
 * registry, sig or task function is called with them held; no blocking wait either.
 */
#ifndef LKD_PT_PRIV_H
#define LKD_PT_PRIV_H

#include "famsvc.h"
#include "lkd_internal.h"
#include "ptrace.h"
#include "sig.h"
#include "task.h"

#define PT_SLOTS 512
#define PT_XS_MAX 2048          /* largest XSAVE image kept per stop */
#define PT_TASK_SIZE_MAX 0x00007ffffffff000ull
#define PT_MAGIC 0x4c4b445054524331ull /* "LKDPTRC1" */
#define PT_VERSION 1

/* Kind of the stop in a slot. */
enum { PK_NONE = 0, PK_SYS_ENTRY = 1, PK_SYS_EXIT = 2, PK_SIGNAL = 3, PK_EVENT = 4, PK_GROUP = 5 };
/* Resume mode chosen by the tracer. */
enum { PM_CONT = 0, PM_SYSCALL = 1, PM_SYSEMU = 2 };
/* Link state. DETACHED: the tracer let go while the tracee was parked, the tracee frees the slot.
 * EXITED: the tracee died, the tracer's wait reports it once and frees the slot. */
enum { PL_FREE = 0, PL_ATTACHED = 1, PL_DETACHED = 2, PL_EXITED = 3 };

struct pt_slot {
    _Atomic uint32_t link;       /* PL_*: lock-free hint, authoritative under pt_lock */
    _Atomic int32_t tid;         /* tracee thread id */
    _Atomic int32_t tracer_pid;  /* tracer thread group (lock-free scans of wait and TracerPid) */
    _Atomic int32_t tracer_tid;
    uint32_t gen;                /* bumped at every allocation */
    int32_t tgid;                /* tracee thread group */
    int32_t tracee_host;         /* host pid of the tracee's process (refreshed at every stop) */
    int32_t tracer_host;
    uint32_t uid;                /* tracee uid (siginfo, wait) */
    uint32_t seized;
    uint32_t options;            /* PTRACE_O_* */
    uint32_t mode;               /* PM_* of the last resume */
    uint32_t step;               /* single-step requested at the last resume */
    uint32_t trap;               /* PTRACE_INTERRUPT pending (seized only) */
    uint32_t stopped;            /* parked and controllable by the tracer */
    uint32_t kind;               /* PK_* of the current or last stop */
    int32_t code;                /* Linux exit_code: sig, SIGTRAP|0x80, (event << 8) | SIGTRAP */
    uint32_t reported;           /* consumed by a wait without WNOWAIT (exit_code cleared) */
    uint64_t seq;                /* stop counter */
    uint64_t resumed_seq;        /* the stop the tracer resumed (== seq after a resume) */
    uint64_t msg;                /* PTRACE_GETEVENTMSG */
    int32_t resume_sig;          /* data of the resuming request */
    int32_t exit_status;         /* PL_EXITED: wait status */
    uint32_t regs_ok, regs_dirty;
    uint32_t fx_ok, xs_ok, fx_dirty, xs_dirty;
    uint32_t why;                /* CLD_TRAPPED or CLD_STOPPED (non-seized group stop) */
    uint32_t si_valid, si_dirty;
    uint32_t mask_dirty;
    uint32_t xs_len;             /* bytes of the XSAVE image (multiple of 8) */
    int64_t sys_nr;              /* syscall in flight at the stop, -1 if none */
    lkd_regs regs;               /* register view of the stop (Linux pt_regs semantics) */
    uint8_t fx[512];
    uint8_t xs[PT_XS_MAX];
    struct lx_siginfo si;
    uint64_t sigmask;
    uint64_t dr[8];              /* u_debugreg as stored by POKEUSER (no hardware breakpoint behind) */
};

/* ---- transport.c -------------------------------------------------------------------------------- */

int pt_table_attach(void);                 /* map the family table (create it in the first process) */
int pt_table_ready(void);
struct pt_slot *pt_slot_at(int i);
void pt_lock(void);
void pt_unlock(void);
int pt_alloc_locked(void);                 /* index of a free slot (marked ATTACHED, gen bumped) or -1 */
void pt_free_locked(int i);
int pt_find_tracee(int32_t tid);           /* lock-free hint: slot of tid in ATTACHED/DETACHED state, -1 */
int pt_host_alive(int32_t host_pid);       /* host process exists and is not a zombie */
int pt_tid_alive(int32_t tid);             /* guest thread exists and is not dead (registry) */
/* Tracer -> tracee: make tid (in thread group tgid) re-read its slot. how: PT_WAKE_* bits. */
enum { PT_WAKE_SYNC = 1, PT_WAKE_KICK = 2, PT_WAKE_TRAP = 4 };
int pt_wake_tracee(int32_t tgid, int32_t tid, unsigned how);
/* Tracee -> tracer: SIGCHLD (CLD_TRAPPED or exit) and wake of the tracer process' wait4 callers. */
void pt_notify_tracer(int32_t tracer_pid, int32_t tracee_tid, uint32_t uid, int cld_code, int cld_status);
void pt_transport_init(void);
void pt_transport_fork_prepare(void);
void pt_transport_fork_parent(void);
void pt_transport_fork_child(void);

struct pt_stats { uint64_t stops, resumes, wakes, notifies, lock_ns_max; };
void pt_get_stats(struct pt_stats *out);
extern struct pt_stats pt_stats_g;

/* ---- stop.c (tracee side) ------------------------------------------------------------------------ */

struct pt_local {
    int slot;                         /* cached slot index, validated against gen and tid */
    uint32_t gen;
    int32_t creator_tid, creator_pid; /* thread that created this process (TRACEME's tracer) */
    int32_t tid_seen;                 /* tid at the last hook (a non-leader exec changes it) */
    int emu;                          /* PTRACE_SYSEMU in effect: no syscall-exit stop */
    int step_on;                      /* single-step enabled on the engine */
    int tick;
    int exited;                       /* task_exit already ran */
    struct {                          /* the tracer link of the parent, captured in the fork child */
        int valid;
        int32_t tracer_pid, tracer_tid, tracer_host;
        uint32_t options, seized;
    } inherit;
};
struct pt_local *pt_local_of(lkd_task *t, int create);

struct pt_stop_req {
    int kind;                     /* PK_* */
    int code;                     /* Linux exit_code of the stop (sig, SIGTRAP|0x80, (event << 8) | SIGTRAP) */
    uint64_t msg;                 /* PTRACE_GETEVENTMSG */
    const struct lx_siginfo *si;  /* siginfo of a signal-delivery-stop; NULL: built from code */
    int64_t nr;                   /* syscall in flight or -1 */
    int64_t ret;                  /* syscall result (exit stops) */
    int ctx_replaced;             /* exit stop of execve: the new context is in place */
    int why;                      /* CLD_TRAPPED, or CLD_STOPPED for a non-seized group stop */
    int no_siginfo;               /* group stop of a non-seized tracee: GETSIGINFO is EINVAL */
    int ignore_fatal;             /* PTRACE_EVENT_EXIT of a task that is dying anyway */
};
struct pt_resume {
    int sig;                      /* signal chosen by the tracer (0 = none) */
    int detached;
    int killed;                   /* SIGKILL arrived while stopped */
    unsigned mode;                /* PM_* */
    int step;
    int regs_changed;
    lkd_regs regs;
    int si_changed;
    struct lx_siginfo si;
};
/* Park t in a stop. Returns 1 (stopped and resumed, *out filled), 0 (not traced: no stop), -1 (the task
 * must die). Called on t's own thread with no LKD lock held. */
int pt_do_stop(lkd_task *t, const struct pt_stop_req *rq, struct pt_resume *out);
/* Slot index of t if t is traced, else -1 (validates the cached index). */
int pt_my_slot(lkd_task *t);
/* Re-derive t->ptrace.flags and tracer_tid from the slot; publish the registry if they changed. ANY. */
void pt_sync_flags(lkd_task *t);
/* After a syscall-stop resume: queue the tracer's signal like send_sig(sig, current, 1). */
void pt_requeue_signal(lkd_task *t, int sig);

/* ---- regs.c --------------------------------------------------------------------------------------- */

uint64_t pt_getreg(const lkd_regs *r, unsigned off);
int pt_putreg(lkd_regs *r, unsigned off, uint64_t val);
uint64_t pt_get_debugreg(const struct pt_slot *s, unsigned n);
int pt_set_debugreg(struct pt_slot *s, unsigned n, uint64_t val);
uint32_t pt_mxcsr_mask(const uint8_t *fx);
int pt_check_xstate(const uint8_t *cur, const uint8_t *img, size_t len);

/* ---- life.c --------------------------------------------------------------------------------------- */

/* Linux wait status of the current stop of a slot: (exit_code << 8) | 0x7f. */
int pt_wait_status(const struct pt_slot *s);
/* Detach every tracee of a tracer thread (whole_process: of the whole process); EXITKILL ones get
 * SIGKILL; tracees that were stopped continue with the signal dropped. */
void pt_tracer_gone(lkd_task *t, int whole_process);
/* Create the link of the tracee thread `tid` (process tgid on host tracee_host) to a tracer thread.
 * Returns the slot index, -LX_EPERM if tid is already traced, -LX_EAGAIN if the table is full. */
int pt_link(int32_t tid, int32_t tgid, int32_t tracee_host, uint32_t uid, int32_t tracer_pid,
            int32_t tracer_tid, int32_t tracer_host, uint32_t options, int seized);
/* Thread group id and host pid of any thread of the family; 0 or -LX_ESRCH. */
int pt_resolve_tid(int32_t tid, int32_t *tgid, int32_t *host, uint32_t *uid, char *state);

/* ---- request.c ------------------------------------------------------------------------------------ */

void pt_request_register(void);

/* Statistics for tests. */
extern _Atomic uint64_t pt_resume_count;

#endif /* LKD_PT_PRIV_H */
