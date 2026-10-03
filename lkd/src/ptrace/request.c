/* request.c: the ptrace(2) system call, tracer side. Owner: ptrace.
 *
 * Models kernel/ptrace.c SYSCALL_DEFINE4(ptrace) (traceme, attach, check_attach, arch_ptrace,
 * ptrace_request, ptrace_regset, ptrace_resume, ptrace_get_syscall_info) and arch/x86/kernel/ptrace.c
 * arch_ptrace(), getreg/putreg, genregs_get/set of Linux v6.12. Error order follows the kernel: the target
 * is found (ESRCH), ATTACH/SEIZE are decided, then ptrace_check_attach() (ESRCH unless the caller thread is
 * the tracer and the tracee is stopped), then the request (unknown ones are EIO).
 *
 * The tracee never runs while the tracer reads or writes its slot: it is parked (transport.c, stop.c).
 * Memory goes through the mm module (lkd_mm_remote_*), which invalidates translated code in the tracee.
 * Registers and FP state are the snapshots published at the stop; writes are commands the tracee applies
 * itself when it resumes.
 */
#include <unistd.h>

#include "mm.h"
#include "pt_priv.h"

#define USER_REGS_WORDS 27
#define USER_STRUCT_BYTES LX_PTRACE_USER_STRUCT_BYTES
#define DEBUGREG0_OFF 848u
#define IO_BITMAP_BYTES 8192u

/* ---- helpers -------------------------------------------------------------------------------------- */

static int64_t uw(uint64_t addr, const void *p, size_t n) { return lkd_user_write(addr, p, n) ? -LX_EFAULT : 0; }
static int64_t ur(uint64_t addr, void *p, size_t n) { return lkd_user_read(addr, p, n) ? -LX_EFAULT : 0; }

/* ptrace_check_attach(): on success returns the slot index with the table lock HELD. */
static int check_attach(lkd_task *t, int32_t pid, int ignore_state)
{
    int i = pt_find_tracee(pid);
    if (i < 0) return -LX_ESRCH;
    pt_lock();
    struct pt_slot *s = pt_slot_at(i);
    if (atomic_load(&s->link) != PL_ATTACHED || atomic_load(&s->tid) != pid ||
        atomic_load(&s->tracer_tid) != t->tid || (!ignore_state && !s->stopped)) {
        pt_unlock();
        return -LX_ESRCH;
    }
    return i;
}

static void words_of(const lkd_regs *r, uint64_t w[USER_REGS_WORDS])
{
    for (unsigned k = 0; k < USER_REGS_WORDS; k++) w[k] = pt_getreg(r, k * 8);
}

/* genregs_set(): each word through putreg(), in order; stops at the first error, earlier words stay. */
static int words_set(lkd_regs *r, const uint64_t *w, unsigned n)
{
    for (unsigned k = 0; k < n; k++) {
        int e = pt_putreg(r, k * 8, w[k]);
        if (e) return e;
    }
    return 0;
}

static int valid_signal(uint64_t sig) { return sig <= LX_NSIG; }

static int check_options(uint64_t data)
{
    if (data & ~(uint64_t)LX_PTRACE_O_MASK) return -LX_EINVAL;
    /* PTRACE_O_SUSPEND_SECCOMP needs CONFIG_SECCOMP and CONFIG_CHECKPOINT_RESTORE: the modeled kernel has
     * no seccomp (task module: prctl/seccomp answer EINVAL/ENOSYS). */
    if (data & LX_PTRACE_O_SUSPEND_SECCOMP) return -LX_EINVAL;
    return 0;
}

/* ---- TRACEME and ATTACH ---------------------------------------------------------------------------- */

static int64_t do_traceme(lkd_task *t)
{
    if (pt_my_slot(t) >= 0) return -LX_EPERM; /* already traced */
    struct pt_local *L = pt_local_of(t, 1);
    if (!L) return -LX_ENOMEM;
    int32_t tpid = L->creator_pid, ttid = L->creator_tid;
    if (!ttid) { /* no creator thread known: the real parent process (its main thread) */
        lkd_proc_refresh_ids(t->proc);
        tpid = t->proc->ppid > 0 ? t->proc->ppid : 1;
        ttid = tpid;
    }
    /* "real_parent->flags & PF_EXITING": the creator thread is gone, the call fails */
    if (ttid != t->tid && !pt_tid_alive(ttid) && ttid != 1) return -LX_EPERM;
    int32_t host = 0;
    lkd_proc_info *pi = malloc(sizeof *pi);
    if (pi && lkd_proc_lookup(tpid, pi) == 0) host = pi->host_pid;
    free(pi);
    int i = pt_link(t->tid, t->proc->pid, getpid(), t->proc->cred.uid, tpid, ttid, host, 0, 0);
    if (i < 0) return i;
    L->slot = i;
    L->gen = pt_slot_at(i)->gen;
    pt_sync_flags(t);
    return 0;
}

static int64_t do_attach(lkd_task *t, int64_t req, int32_t pid, uint64_t addr, uint64_t data)
{
    int seize = req == LX_PTRACE_SEIZE;
    uint32_t opts = 0;
    if (seize) {
        if (addr != 0) return -LX_EIO;
        if (data & ~(uint64_t)LX_PTRACE_O_MASK) return -LX_EIO;
        int e = check_options(data);
        if (e) return e;
        opts = (uint32_t)data;
    }
    int32_t tgid = 0, host = 0;
    uint32_t uid = 0;
    char state = 0;
    int e = pt_resolve_tid(pid, &tgid, &host, &uid, &state);
    if (e < 0) return e;
    if (tgid == t->proc->pid) return -LX_EPERM; /* same_thread_group(): also covers tracing oneself */
    e = lkd_ptrace_check_access(t, tgid, LKD_PTRACE_MODE_ATTACH);
    if (e < 0) return e;
    if (state == LKD_PS_ZOMBIE || state == LKD_PS_DEAD) return -LX_EPERM; /* task->exit_state */
    int i = pt_link(pid, tgid, host, uid, t->proc->pid, t->tid, getpid(), opts, seize);
    if (i < 0) return i;
    atomic_fetch_or(&t->ptrace.flags, LKD_PT_TRACER);
    /* the tracee re-reads its slot (flags) before it can see the SIGSTOP */
    (void)pt_wake_tracee(tgid, pid, PT_WAKE_SYNC | PT_WAKE_KICK);
    if (!seize) {
        struct lx_siginfo si;
        memset(&si, 0, sizeof si);
        si.si_signo = LX_SIGSTOP;
        si.si_code = LX_SI_KERNEL; /* send_sig_info(SIGSTOP, SEND_SIG_PRIV, task) */
        (void)lkd_sig_send_tid(NULL, tgid, pid, LX_SIGSTOP, &si);
    }
    return 0;
}

/* ---- register sets ---------------------------------------------------------------------------------- */

/* Everything GETREGSET/SETREGSET need from the slot, copied under the lock. */
struct snap {
    int regs_ok, fx_ok, xs_ok;
    uint32_t xs_len;
    lkd_regs regs;
    uint8_t fx[512];
    uint8_t *xs;
};

static int64_t regset_get(lkd_task *t, int32_t pid, uint64_t nt, uint64_t uiov, int fixed_len)
{
    (void)fixed_len;
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    pt_unlock();
    struct lx_iovec iov;
    int64_t e = ur(uiov, &iov, sizeof iov);
    if (e) return e;
    size_t elem = 8, full;
    switch (nt) {
    case LX_NT_PRSTATUS: full = LX_PTRACE_USER_REGS_BYTES; break;
    case LX_NT_PRFPREG: full = LX_PTRACE_FPREGS_BYTES; break;
    case LX_NT_X86_XSTATE: full = 0; break; /* decided under the lock below */
    case LX_NT_X86_IO_BITMAP: full = IO_BITMAP_BYTES; break;
    default: return -LX_EINVAL; /* no such regset in the x86-64 view (NT_PRXFPREG is 32-bit only) */
    }
    if (iov.iov_len % elem) return -LX_EINVAL;
    uint8_t *buf = malloc(PT_XS_MAX);
    if (!buf) return -LX_ENOMEM;
    uint64_t len = 0;
    int64_t r = 0;
    i = check_attach(t, pid, 0);
    if (i < 0) { free(buf); return i; }
    struct pt_slot *s = pt_slot_at(i);
    if (nt == LX_NT_X86_XSTATE) full = s->xs_ok ? s->xs_len : 0;
    len = iov.iov_len < full ? iov.iov_len : full;
    if (nt == LX_NT_PRSTATUS) {
        if (len && !s->regs_ok) r = -LX_EIO;
        else { uint64_t w[USER_REGS_WORDS]; words_of(&s->regs, w); memcpy(buf, w, sizeof w); }
    } else if (nt == LX_NT_PRFPREG) {
        if (len && !s->fx_ok) r = -LX_EIO; else memcpy(buf, s->fx, 512);
    } else if (nt == LX_NT_X86_XSTATE) {
        if (len == 0 && !s->xs_ok) r = iov.iov_len ? -LX_ENODEV : 0;
        else if (len) memcpy(buf, s->xs, len);
    } else { /* NT_386_IOPERM: ioperm_get() without a bitmap */
        if (len) r = -LX_ENXIO;
    }
    pt_unlock();
    if (!r && len) r = uw(iov.iov_base, buf, len);
    free(buf);
    if (r) return r;
    iov.iov_len = len;
    return uw(uiov + 8, &iov.iov_len, 8);
}

static int64_t regset_set(lkd_task *t, int32_t pid, uint64_t nt, uint64_t uiov)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    pt_unlock();
    struct lx_iovec iov;
    int64_t e = ur(uiov, &iov, sizeof iov);
    if (e) return e;
    size_t full;
    switch (nt) {
    case LX_NT_PRSTATUS: full = LX_PTRACE_USER_REGS_BYTES; break;
    case LX_NT_PRFPREG: full = LX_PTRACE_FPREGS_BYTES; break;
    case LX_NT_X86_XSTATE: full = 0; break;
    case LX_NT_X86_IO_BITMAP: full = IO_BITMAP_BYTES; break;
    default: return -LX_EINVAL;
    }
    if (iov.iov_len % 8) return -LX_EINVAL;
    if (nt == LX_NT_X86_IO_BITMAP) return -LX_EOPNOTSUPP; /* no ->set: copy_regset_from_user() */
    uint8_t *buf = malloc(PT_XS_MAX);
    if (!buf) return -LX_ENOMEM;
    i = check_attach(t, pid, 0);
    if (i < 0) { free(buf); return i; }
    struct pt_slot *s = pt_slot_at(i);
    if (nt == LX_NT_X86_XSTATE) full = s->xs_ok ? s->xs_len : 0;
    uint64_t len = iov.iov_len < full ? iov.iov_len : full;
    uint32_t xs_len = s->xs_len;
    pt_unlock();
    if (len > PT_XS_MAX) len = PT_XS_MAX;
    if (len && (e = ur(iov.iov_base, buf, len))) { free(buf); return e; }
    int64_t r = 0;
    i = check_attach(t, pid, 0);
    if (i < 0) { free(buf); return i; }
    s = pt_slot_at(i);
    if (nt == LX_NT_PRSTATUS) {
        if (len) {
            if (!s->regs_ok) r = -LX_EIO;
            else {
                uint64_t w[USER_REGS_WORDS];
                memcpy(w, buf, len);
                r = words_set(&s->regs, w, (unsigned)(len / 8));
                s->regs_dirty = 1; /* words before an error were applied, like genregs_set() */
            }
        }
    } else if (nt == LX_NT_PRFPREG) {
        /* xfpregs_set(): the whole 512-byte image or EINVAL; reserved MXCSR bits are masked off */
        if (len != 512) r = -LX_EINVAL;
        else if (!s->fx_ok) r = -LX_EIO;
        else {
            uint32_t mx;
            memcpy(&mx, buf + 24, 4);
            mx &= pt_mxcsr_mask(s->fx);
            memcpy(buf + 24, &mx, 4);
            memcpy(s->fx, buf, 512);
            if (s->xs_ok) memcpy(s->xs, buf, 464); /* keep the software-reserved bytes at 464 */
            s->fx_dirty = 1;
        }
    } else { /* NT_X86_XSTATE */
        if (!s->xs_ok) r = -LX_ENODEV;
        else if (len != xs_len || len != s->xs_len) r = -LX_EFAULT; /* xstateregs_set(): whole buffer only */
        else if ((r = pt_check_xstate(s->xs, buf, len))) { }
        else {
            memcpy(s->xs, buf, 464);              /* x87/SSE image (sw bytes at 464 stay) */
            memcpy(s->xs + 512, buf + 512, len - 512);
            memcpy(s->fx, s->xs, 512);
            s->xs_dirty = 1;
            s->fx_dirty = 0;
        }
    }
    pt_unlock();
    free(buf);
    if (r) return r;
    iov.iov_len = len;
    return uw(uiov + 8, &iov.iov_len, 8);
}

/* ---- the rest of the requests ----------------------------------------------------------------------- */

static int64_t do_resume(lkd_task *t, int32_t pid, int64_t req, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    int64_t r = 0;
    int32_t tgid = s->tgid;
    int step = req == LX_PTRACE_SINGLESTEP || req == LX_PTRACE_SYSEMU_SINGLESTEP;
    if (!valid_signal(data)) r = -LX_EIO;
    else if (req == LX_PTRACE_SINGLEBLOCK) r = -LX_EIO; /* no branch trace in the engine */
    else if (step) {
        /* arch_has_single_step(): only if the engine can run one guest instruction */
        const lkd_engine_ops *ops = lkd_engine();
        if (!ops->set_single_step || ops->set_single_step(t, 0) == -LX_ENOSYS) {
            lkd_log_once("ptrace: PTRACE_SINGLESTEP refused: the engine has no single-step support (EIO)");
            r = -LX_EIO;
        }
    }
    if (r) { pt_unlock(); return r; }
    s->mode = req == LX_PTRACE_SYSCALL ? PM_SYSCALL
            : (req == LX_PTRACE_SYSEMU || req == LX_PTRACE_SYSEMU_SINGLESTEP) ? PM_SYSEMU : PM_CONT;
    s->step = (uint32_t)step;
    s->resume_sig = (int32_t)data;
    s->resumed_seq = s->seq;
    s->stopped = 0;
    pt_stats_g.resumes++;
    pt_unlock();
    (void)pt_wake_tracee(tgid, pid, 0);
    return 0;
}

static int64_t do_detach(lkd_task *t, int32_t pid, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (!valid_signal(data)) { pt_unlock(); return -LX_EIO; }
    int32_t tgid = s->tgid;
    s->mode = PM_CONT;
    s->step = 0;
    s->resume_sig = (int32_t)data;
    s->stopped = 0;
    atomic_store(&s->link, PL_DETACHED); /* the parked tracee applies pending commands, then frees the slot */
    pt_unlock();
    (void)pt_wake_tracee(tgid, pid, 0);
    return 0;
}

static int64_t do_peekpoke(lkd_task *t, int32_t pid, int64_t req, uint64_t addr, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    int32_t tgid = pt_slot_at(i)->tgid;
    pt_unlock();
    if (req == LX_PTRACE_PEEKTEXT || req == LX_PTRACE_PEEKDATA) {
        uint64_t w = 0;
        int64_t n = lkd_mm_remote_read(tgid, addr, &w, 8);
        if (n != 8) return -LX_EIO;
        return uw(data, &w, 8);
    }
    int64_t n = lkd_mm_remote_write(tgid, addr, &data, 8);
    return n == 8 ? 0 : -LX_EIO;
}

static int64_t do_peekuser(lkd_task *t, int32_t pid, uint64_t addr, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    int64_t r = -LX_EIO;
    uint64_t tmp = 0;
    if (!(addr & 7) && addr < USER_STRUCT_BYTES) {
        if (addr < LX_PTRACE_USER_REGS_BYTES) {
            if (s->regs_ok) { tmp = pt_getreg(&s->regs, (unsigned)addr); r = 0; }
        } else if (addr >= DEBUGREG0_OFF && addr <= DEBUGREG0_OFF + 7 * 8) {
            tmp = pt_get_debugreg(s, (unsigned)((addr - DEBUGREG0_OFF) / 8));
            r = 0;
        } else {
            r = 0; /* the rest of struct user reads as zero */
        }
    }
    pt_unlock();
    return r ? r : uw(data, &tmp, 8);
}

static int64_t do_pokeuser(lkd_task *t, int32_t pid, uint64_t addr, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    int64_t r = -LX_EIO;
    if (!(addr & 7) && addr < USER_STRUCT_BYTES) {
        if (addr < LX_PTRACE_USER_REGS_BYTES) {
            if (s->regs_ok) { r = pt_putreg(&s->regs, (unsigned)addr, data); s->regs_dirty = 1; }
        } else if (addr >= DEBUGREG0_OFF && addr <= DEBUGREG0_OFF + 7 * 8) {
            r = pt_set_debugreg(s, (unsigned)((addr - DEBUGREG0_OFF) / 8), data);
        }
    }
    pt_unlock();
    return r;
}

static int64_t do_getregs(lkd_task *t, int32_t pid, uint64_t data, int set)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (!s->regs_ok) { pt_unlock(); return -LX_EIO; }
    uint64_t w[USER_REGS_WORDS];
    if (!set) {
        words_of(&s->regs, w);
        pt_unlock();
        return uw(data, w, sizeof w);
    }
    pt_unlock();
    int64_t e = ur(data, w, sizeof w);
    if (e) return e;
    i = check_attach(t, pid, 0);
    if (i < 0) return i;
    s = pt_slot_at(i);
    int64_t r = words_set(&s->regs, w, USER_REGS_WORDS);
    s->regs_dirty = 1;
    pt_unlock();
    return r;
}

static int64_t do_fpregs(lkd_task *t, int32_t pid, uint64_t data, int set)
{
    uint64_t iov[2] = { data, 512 };
    (void)iov;
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (!s->fx_ok) { pt_unlock(); return -LX_EIO; }
    uint8_t buf[512];
    if (!set) {
        memcpy(buf, s->fx, 512);
        pt_unlock();
        return uw(data, buf, 512);
    }
    pt_unlock();
    int64_t e = ur(data, buf, 512);
    if (e) return e;
    i = check_attach(t, pid, 0);
    if (i < 0) return i;
    s = pt_slot_at(i);
    uint32_t mx;
    memcpy(&mx, buf + 24, 4);
    mx &= pt_mxcsr_mask(s->fx);
    memcpy(buf + 24, &mx, 4);
    memcpy(s->fx, buf, 512);
    if (s->xs_ok) memcpy(s->xs, buf, 464);
    s->fx_dirty = 1;
    pt_unlock();
    return 0;
}

static int64_t do_siginfo(lkd_task *t, int32_t pid, uint64_t data, int set)
{
    struct lx_siginfo si;
    if (set) {
        int i = check_attach(t, pid, 0);
        if (i < 0) return i;
        pt_unlock();
        int64_t e = ur(data, &si, sizeof si);
        if (e) return e;
    }
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (!s->si_valid) { pt_unlock(); return -LX_EINVAL; } /* last_siginfo == NULL */
    if (set) { s->si = si; s->si_dirty = 1; pt_unlock(); return 0; }
    si = s->si;
    pt_unlock();
    return uw(data, &si, sizeof si);
}

static int64_t do_syscall_info(lkd_task *t, int32_t pid, uint64_t user_size, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    struct lx_ptrace_syscall_info info;
    memset(&info, 0, sizeof info);
    info.op = LX_PTRACE_SYSCALL_INFO_NONE;
    info.arch = LX_AUDIT_ARCH_X86_64;
    info.instruction_pointer = s->regs.rip;
    info.stack_pointer = s->regs.rsp;
    uint64_t actual = offsetof(struct lx_ptrace_syscall_info, u);
    /* the kernel keys on last_siginfo->si_code == SIGTRAP | 0x80, i.e. TRACESYSGOOD is needed */
    if (s->si_valid && s->si.si_code == (LX_SIGTRAP | 0x80)) {
        if (s->msg == 1) {
            info.op = LX_PTRACE_SYSCALL_INFO_ENTRY;
            info.u.entry.nr = s->regs.orig_rax;
            info.u.entry.args[0] = s->regs.rdi; info.u.entry.args[1] = s->regs.rsi;
            info.u.entry.args[2] = s->regs.rdx; info.u.entry.args[3] = s->regs.r10;
            info.u.entry.args[4] = s->regs.r8; info.u.entry.args[5] = s->regs.r9;
            actual = offsetof(struct lx_ptrace_syscall_info, u) + 8 + 48;
        } else if (s->msg == 2) {
            int64_t rax = (int64_t)s->regs.rax;
            int is_err = rax >= -(int64_t)LX_MAX_ERRNO && rax <= -1;
            info.op = LX_PTRACE_SYSCALL_INFO_EXIT;
            info.u.exit.rval = rax;
            info.u.exit.is_error = (uint8_t)is_err;
            actual = offsetof(struct lx_ptrace_syscall_info, u) + 8 + 1;
        }
    }
    pt_unlock();
    uint64_t n = actual < user_size ? actual : user_size;
    if (n && lkd_user_write(data, &info, n)) return -LX_EFAULT;
    return (int64_t)actual;
}

static int64_t do_arch_prctl(lkd_task *t, int32_t pid, uint64_t option, uint64_t arg2)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    int64_t r = 0;
    uint64_t out = 0;
    int put = 0;
    if (!s->regs_ok) r = -LX_EIO;
    else switch (option) { /* do_arch_prctl_64(child, option, arg2) */
    case LX_ARCH_SET_FS: if (arg2 >= PT_TASK_SIZE_MAX) r = -LX_EPERM; else { s->regs.fs_base = arg2; s->regs_dirty = 1; } break;
    case LX_ARCH_SET_GS: if (arg2 >= PT_TASK_SIZE_MAX) r = -LX_EPERM; else { s->regs.gs_base = arg2; s->regs_dirty = 1; } break;
    case LX_ARCH_GET_FS: out = s->regs.fs_base; put = 1; break;
    case LX_ARCH_GET_GS: out = s->regs.gs_base; put = 1; break;
    case LX_ARCH_GET_CPUID: case LX_ARCH_SET_CPUID: r = -LX_ENODEV; break; /* no CPUID faulting (task module) */
    default: r = -LX_EINVAL; break;
    }
    pt_unlock();
    return r ? r : (put ? uw(arg2, &out, 8) : 0);
}

static int64_t do_sigmask(lkd_task *t, int32_t pid, uint64_t size, uint64_t data, int set)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (size != 8) { pt_unlock(); return -LX_EINVAL; }
    if (!set) { uint64_t m = s->sigmask; pt_unlock(); return uw(data, &m, 8); }
    pt_unlock();
    uint64_t m;
    int64_t e = ur(data, &m, 8);
    if (e) return e;
    m &= ~(LX_SIGBIT(LX_SIGKILL) | LX_SIGBIT(LX_SIGSTOP));
    i = check_attach(t, pid, 0);
    if (i < 0) return i;
    s = pt_slot_at(i);
    s->sigmask = m;
    s->mask_dirty = 1;
    pt_unlock();
    return 0;
}

static int64_t do_interrupt(lkd_task *t, int32_t pid)
{
    int i = check_attach(t, pid, 1);
    if (i < 0) return i;
    struct pt_slot *s = pt_slot_at(i);
    if (!s->seized) { pt_unlock(); return -LX_EIO; }
    int32_t tgid = s->tgid;
    s->trap = 1; /* at least one trap follows, even if the tracee is stopped now */
    pt_unlock();
    unsigned how = PT_WAKE_SYNC | PT_WAKE_KICK;
    if (lkd_sig_trace_stop_supported()) how |= PT_WAKE_TRAP;
    (void)pt_wake_tracee(tgid, pid, how);
    return 0;
}

static int64_t do_kill(lkd_task *t, int32_t pid)
{
    int i = check_attach(t, pid, 1);
    if (i < 0) return i;
    int32_t tgid = pt_slot_at(i)->tgid;
    pt_unlock();
    (void)lkd_sig_send_pid(t, tgid, LX_SIGKILL, NULL); /* send_sig_info(SIGKILL, SEND_SIG_NOINFO, child) */
    return 0;
}

static int64_t do_setoptions(lkd_task *t, int32_t pid, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    int e = check_options(data);
    if (!e) pt_slot_at(i)->options = (uint32_t)data;
    pt_unlock();
    return e;
}

static int64_t do_geteventmsg(lkd_task *t, int32_t pid, uint64_t data)
{
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    uint64_t m = pt_slot_at(i)->msg;
    pt_unlock();
    return uw(data, &m, 8);
}

/* ---- the system call -------------------------------------------------------------------------------- */

LKD_SYSCALL(ptrace)
{
    int64_t req = LKD_ARGL(0);
    int32_t pid = LKD_ARGI(1);
    uint64_t addr = LKD_ARGU(2), data = LKD_ARGU(3);
    if (!pt_table_ready()) return -LX_ENOSYS; /* the shared table could not be created: say so */
    if (req == LX_PTRACE_TRACEME) return do_traceme(t);
    if (pid <= 0) return -LX_ESRCH;
    lkd_tid_info ti;
    if (lkd_tid_lookup(pid, &ti) < 0) return -LX_ESRCH; /* find_get_task_by_vpid() */
    if (req == LX_PTRACE_ATTACH || req == LX_PTRACE_SEIZE) return do_attach(t, req, pid, addr, data);
    switch (req) {
    case LX_PTRACE_KILL: return do_kill(t, pid);
    case LX_PTRACE_INTERRUPT: {
        /* ptrace_check_attach(ignore_state) only for a seized tracee; others are not stopped: ESRCH */
        int i = pt_find_tracee(pid);
        if (i >= 0 && pt_slot_at(i)->seized) return do_interrupt(t, pid);
        break;
    }
    default: break;
    }
    /* every other request needs a stopped tracee of this thread */
    int i = check_attach(t, pid, 0);
    if (i < 0) return i;
    pt_unlock();
    switch (req) {
    case LX_PTRACE_PEEKTEXT: case LX_PTRACE_PEEKDATA: case LX_PTRACE_POKETEXT: case LX_PTRACE_POKEDATA:
        return do_peekpoke(t, pid, req, addr, data);
    case LX_PTRACE_PEEKUSER: return do_peekuser(t, pid, addr, data);
    case LX_PTRACE_POKEUSER: return do_pokeuser(t, pid, addr, data);
    case LX_PTRACE_CONT: case LX_PTRACE_SYSCALL: case LX_PTRACE_SINGLESTEP: case LX_PTRACE_SYSEMU:
    case LX_PTRACE_SYSEMU_SINGLESTEP: case LX_PTRACE_SINGLEBLOCK:
        return do_resume(t, pid, req, data);
    case LX_PTRACE_DETACH: return do_detach(t, pid, data);
    case LX_PTRACE_GETREGS: return do_getregs(t, pid, data, 0);
    case LX_PTRACE_SETREGS: return do_getregs(t, pid, data, 1);
    case LX_PTRACE_GETFPREGS: return do_fpregs(t, pid, data, 0);
    case LX_PTRACE_SETFPREGS: return do_fpregs(t, pid, data, 1);
    case LX_PTRACE_GETREGSET: return regset_get(t, pid, addr, data, 0);
    case LX_PTRACE_SETREGSET: return regset_set(t, pid, addr, data);
    case LX_PTRACE_SETOPTIONS: return do_setoptions(t, pid, data);
    case LX_PTRACE_GETEVENTMSG: return do_geteventmsg(t, pid, data);
    case LX_PTRACE_GETSIGINFO: return do_siginfo(t, pid, data, 0);
    case LX_PTRACE_SETSIGINFO: return do_siginfo(t, pid, data, 1);
    case LX_PTRACE_GETSIGMASK: return do_sigmask(t, pid, addr, data, 0);
    case LX_PTRACE_SETSIGMASK: return do_sigmask(t, pid, addr, data, 1);
    case LX_PTRACE_GET_SYSCALL_INFO: return do_syscall_info(t, pid, addr, data);
    case LX_PTRACE_ARCH_PRCTL: return do_arch_prctl(t, pid, data, addr); /* arguments are reversed */
    case LX_PTRACE_LISTEN:
    case LX_PTRACE_PEEKSIGINFO:
    case LX_PTRACE_SECCOMP_GET_FILTER: case LX_PTRACE_SECCOMP_GET_METADATA:
    case LX_PTRACE_GET_RSEQ_CONFIGURATION:
    case LX_PTRACE_SET_SYSCALL_USER_DISPATCH_CONFIG: case LX_PTRACE_GET_SYSCALL_USER_DISPATCH_CONFIG:
    case LX_PTRACE_GET_THREAD_AREA: case LX_PTRACE_SET_THREAD_AREA:
    case LX_PTRACE_GETFPXREGS: case LX_PTRACE_SETFPXREGS:
        /* Not modeled (no seccomp, no rseq, no syscall user dispatch, no 32-bit TLS, no queued-signal
         * export, no LISTEN): the kernel's default of ptrace_request() is EIO. Logged, never faked. */
        lkd_log_once("ptrace: request 0x%llx is not implemented (EIO)", (unsigned long long)req);
        return -LX_EIO;
    default:
        return -LX_EIO;
    }
}

void pt_request_register(void) { LKD_REGISTER(ptrace); }
