/* stop.c: the tracee side of ptrace: stops at safe points and the hooks the other modules call.
 * Owner: ptrace.
 *
 * A stop is published in the tracee's slot (transport.c): Linux exit_code, event message, siginfo, a
 * register view with the semantics of pt_regs at that kind of stop, and FP images, all taken by the
 * tracee's own host thread (the engine's get_regs/get_fp are only valid there). The thread then parks
 * on its wake pipe until the tracer's request changes the slot. Commands written by the tracer while it
 * is parked (SETREGS, SETFPREGS, SETSIGINFO, SETSIGMASK, the resume signal, mode and step) are applied
 * by the tracee itself when it resumes, in one critical section, so a stale tracer cannot write after
 * the resume (its requests then fail with ESRCH).
 *
 * Models kernel/signal.c ptrace_stop(), ptrace_signal(), include/linux/ptrace.h ptrace_report_syscall(),
 * ptrace_event(), kernel/entry/common.c syscall_trace_enter()/syscall_exit_work() (Linux v6.12).
 * imported from HL proc_ptrace.c trace_stop(): the publish / park-on-notification / re-read shape, and
 * the "tracer disconnect resumes the tracee with its signal suppressed" rule.
 */
#include <poll.h>
#include <unistd.h>

#include "pt_priv.h"

_Atomic uint64_t pt_resume_count;

struct pt_local *pt_local_of(lkd_task *t, int create)
{
    struct pt_local *L = __atomic_load_n((struct pt_local **)&t->ptrace.state, __ATOMIC_ACQUIRE);
    if (!L && create) {
        L = calloc(1, sizeof *L);
        if (!L) return NULL;
        L->slot = -1;
        L->tid_seen = t->tid;
        __atomic_store_n((struct pt_local **)&t->ptrace.state, L, __ATOMIC_RELEASE);
    }
    return L;
}

int pt_my_slot(lkd_task *t)
{
    struct pt_local *L = pt_local_of(t, 0);
    if (L && L->slot >= 0 && pt_table_ready()) {
        struct pt_slot *s = pt_slot_at(L->slot);
        if (atomic_load(&s->link) == PL_ATTACHED && atomic_load(&s->tid) == t->tid &&
            __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) == L->gen)
            return L->slot;
    }
    if (!pt_table_ready()) return -1;
    int i = pt_find_tracee(t->tid);
    if (i < 0) return -1;
    L = pt_local_of(t, 1);
    if (!L) return -1;
    L->slot = i;
    L->gen = __atomic_load_n(&pt_slot_at(i)->gen, __ATOMIC_ACQUIRE);
    return i;
}

void pt_sync_flags(lkd_task *t)
{
    uint32_t want = 0;
    int32_t tracer = 0;
    int i = pt_table_ready() ? pt_find_tracee(t->tid) : -1;
    if (i >= 0) {
        struct pt_slot *s = pt_slot_at(i);
        want |= LKD_PT_TRACED;
        tracer = atomic_load(&s->tracer_tid);
        unsigned m = __atomic_load_n(&s->mode, __ATOMIC_ACQUIRE);
        if (m == PM_SYSCALL || m == PM_SYSEMU || __atomic_load_n(&s->trap, __ATOMIC_ACQUIRE))
            want |= LKD_PT_SYSCALL_TRACE; /* a pending INTERRUPT trap is taken at the next syscall boundary */
        if (__atomic_load_n(&s->seized, __ATOMIC_ACQUIRE)) want |= LKD_PT_SEIZED;
        if (__atomic_load_n(&s->step, __ATOMIC_ACQUIRE)) want |= LKD_PT_SINGLESTEP;
    }
    const uint32_t mine = LKD_PT_TRACED | LKD_PT_SYSCALL_TRACE | LKD_PT_SEIZED | LKD_PT_SINGLESTEP;
    uint32_t old = atomic_load(&t->ptrace.flags), nw;
    do { nw = (old & ~mine) | want; } while (!atomic_compare_exchange_weak(&t->ptrace.flags, &old, nw));
    int32_t oldtr = t->ptrace.tracer_tid;
    t->ptrace.tracer_tid = tracer;
    if (oldtr != tracer || (old & mine) != want) lkd_registry_publish(t);
}

void pt_requeue_signal(lkd_task *t, int sig)
{
    if (sig < 1 || sig > LX_NSIG) return;
    struct lx_siginfo si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = LX_SI_KERNEL; /* send_sig(sig, current, 1) is SEND_SIG_PRIV */
    lkd_sig_send_task(t, sig, &si, 1);
}

/* ---- the registers the tracer sees ---------------------------------------------------------------- */

/* ptrace_report_syscall_entry/exit and the exec exit stop show the registers as the kernel keeps them in
 * pt_regs: at entry rax = -ENOSYS and orig_rax = number; rcx and r11 hold the return rip and rflags that
 * the SYSCALL instruction saved; at exit rax is the result. After execve the context is new and rcx/r11
 * are not those (the kernel returns by IRET). The engine's registers are only the base. */
static void patch_view(const struct pt_stop_req *rq, lkd_regs *r)
{
    switch (rq->kind) {
    case PK_SYS_ENTRY:
        r->orig_rax = (uint64_t)rq->nr;
        r->rax = (uint64_t)-(int64_t)LX_ENOSYS;
        r->rcx = r->rip;
        r->r11 = r->eflags;
        break;
    case PK_SYS_EXIT:
        r->orig_rax = (uint64_t)rq->nr;
        /* rt_sigreturn: rax is the one restored from the frame, already in the engine's registers */
        if (!(rq->ctx_replaced && rq->nr == LKD_NR_rt_sigreturn)) r->rax = (uint64_t)rq->ret;
        if (!rq->ctx_replaced) { r->rcx = r->rip; r->r11 = r->eflags; }
        break;
    case PK_EVENT:
        if ((rq->code >> 8) == LX_PTRACE_EVENT_EXEC) {
            r->orig_rax = 59;
        } else if (rq->nr >= 0) { /* inside a syscall (fork, clone, exit...) */
            r->orig_rax = (uint64_t)rq->nr;
            r->rax = (uint64_t)-(int64_t)LX_ENOSYS;
            r->rcx = r->rip;
            r->r11 = r->eflags;
        }
        break;
    case PK_SIGNAL:
        /* An exception (int3, page fault, #DE...) enters the kernel with orig_ax = -1; a signal taken
         * on the way out of a syscall keeps the syscall number. A hardware fault carries si_code > 0
         * or SI_KERNEL. */
        if (rq->si && (rq->si->si_code > 0 || rq->si->si_code == LX_SI_KERNEL)) r->orig_rax = (uint64_t)-1;
        break;
    default:
        break;
    }
}

/* XSAVE images in the user ABI carry the software-reserved bytes at 464 (struct _fpx_sw_bytes). */
static void fill_sw_bytes(uint8_t *xs, size_t len)
{
    if (len < 576) return;
    uint32_t magic1 = 0x46505853u, ext = (uint32_t)len + 4, xsz = (uint32_t)len;
    uint64_t feat = len >= 832 ? 7u : 3u;
    memcpy(xs + 464, &magic1, 4);
    memcpy(xs + 468, &ext, 4);
    memcpy(xs + 472, &feat, 8);
    memcpy(xs + 480, &xsz, 4);
}

/* ---- the stop ------------------------------------------------------------------------------------- */

static void apply_resume(lkd_task *t, struct pt_local *L, const struct pt_resume *r,
                         const uint8_t *fx, int fx_dirty, const uint8_t *xs, size_t xs_len, int xs_dirty,
                         uint64_t mask, int mask_dirty)
{
    const lkd_engine_ops *ops = lkd_engine();
    if (r->regs_changed) (void)ops->set_regs(t, &r->regs);
    if (xs_dirty) (void)ops->set_fp(t, LKD_FP_XSAVE, xs, xs_len);
    if (fx_dirty) (void)ops->set_fp(t, LKD_FP_FXSAVE, fx, 512);
    if (mask_dirty) lkd_sig_set_mask(t, mask);
    if (ops->set_single_step && (r->step || L->step_on)) {
        if (ops->set_single_step(t, r->step) == 0) L->step_on = r->step;
    }
}

static int tracer_dead(const struct pt_slot *s)
{
    return s->tracer_host > 0 && s->tracer_host != getpid() && !pt_host_alive(s->tracer_host);
}

int pt_do_stop(lkd_task *t, const struct pt_stop_req *rq, struct pt_resume *out)
{
    memset(out, 0, sizeof *out);
    int i = pt_my_slot(t);
    if (i < 0) { pt_sync_flags(t); return 0; }
    struct pt_local *L = pt_local_of(t, 1);
    if (!L) return 0;
    const lkd_engine_ops *ops = lkd_engine();
    uint32_t gen = L->gen;

    /* Snapshot, on this thread, outside every lock. */
    lkd_regs r;
    memset(&r, 0, sizeof r);
    int regs_ok = ops->get_regs(t, &r) == 0;
    if (regs_ok) patch_view(rq, &r);
    uint8_t *xs = malloc(PT_XS_MAX);
    if (!xs) return 0;
    memset(xs, 0, PT_XS_MAX);
    size_t xlen = PT_XS_MAX;
    int xs_ok = ops->get_fp(t, LKD_FP_XSAVE, xs, &xlen) == 0;
    uint8_t fx[512];
    int fx_ok = 0;
    if (xs_ok) {
        if (xlen > PT_XS_MAX) xlen = PT_XS_MAX;
        xlen &= ~(size_t)7;
        if (xlen < 576) { xs_ok = 0; }
        else { memcpy(fx, xs, 512); fx_ok = 1; fill_sw_bytes(xs, xlen); }
    }
    if (!fx_ok) {
        size_t fl = sizeof fx;
        fx_ok = ops->get_fp(t, LKD_FP_FXSAVE, fx, &fl) == 0;
    }
    uint64_t mask = atomic_load(&t->sigmask);
    if (t->restore_sigmask) mask = t->saved_sigmask;
    atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP);

    pt_lock();
    struct pt_slot *s = pt_slot_at(i);
    if (atomic_load(&s->link) != PL_ATTACHED || atomic_load(&s->tid) != t->tid || s->gen != gen) {
        pt_unlock();
        free(xs);
        pt_sync_flags(t);
        return 0;
    }
    s->kind = (uint32_t)rq->kind;
    s->code = rq->code;
    s->msg = rq->msg;
    s->why = (uint32_t)rq->why;
    s->sys_nr = rq->nr;
    s->tgid = t->proc->pid;
    s->tracee_host = getpid();
    s->uid = t->proc->cred.uid;
    s->regs = r;
    s->regs_ok = (uint32_t)regs_ok;
    s->regs_dirty = 0;
    s->fx_ok = (uint32_t)fx_ok;
    s->xs_ok = (uint32_t)xs_ok;
    s->fx_dirty = s->xs_dirty = 0;
    if (fx_ok) memcpy(s->fx, fx, 512);
    if (xs_ok) { memcpy(s->xs, xs, xlen); s->xs_len = (uint32_t)xlen; }
    else s->xs_len = 0;
    s->sigmask = mask;
    s->mask_dirty = 0;
    s->si_dirty = 0;
    s->trap = 0;
    if (rq->no_siginfo) {
        s->si_valid = 0;
    } else {
        s->si_valid = 1;
        if (rq->si) {
            s->si = *rq->si;
        } else {
            memset(&s->si, 0, sizeof s->si);
            s->si.si_signo = rq->kind == PK_GROUP ? (rq->code & 0x7f) : LX_SIGTRAP;
            s->si.si_code = rq->code;
            s->si.u.kill.pid = t->tid;
            s->si.u.kill.uid = s->uid;
        }
    }
    s->resume_sig = 0;
    s->seq++;
    s->reported = 0;
    s->stopped = 1;
    int32_t tracer_pid = atomic_load(&s->tracer_pid);
    uint32_t uid = s->uid;
    int why = (int)s->why;
    int code = s->code;
    pt_stats_g.stops++;
    pt_unlock();
    free(xs);

    atomic_fetch_or(&t->ptrace.flags, LKD_PT_STOPPED);
    lkd_registry_publish(t);
    pt_notify_tracer(tracer_pid, t->tid, uid, why ? why : LX_CLD_TRAPPED, code & 0x7f);

    /* Park until the tracer resumes, detaches or dies, or SIGKILL arrives. */
    uint8_t *nfx = NULL, *nxs = NULL;
    int fx_dirty = 0, xs_dirty = 0, mask_dirty = 0;
    uint64_t newmask = 0;
    size_t nxlen = 0;
    int result = 1;
    for (;;) {
        pt_lock();
        s = pt_slot_at(i);
        int detached = 0, resumed = 0, killed = 0;
        if (atomic_load(&s->tid) != t->tid || s->gen != gen || atomic_load(&s->link) == PL_FREE) {
            detached = 1; /* the slot was recycled under us: nobody controls this task any more */
        } else if (atomic_load(&s->link) == PL_DETACHED) {
            detached = 1;
        } else if (!s->stopped) {
            resumed = 1;
        } else if (!rq->ignore_fatal && (atomic_load(&t->flags) & LKD_TF_FATAL)) {
            s->stopped = 0;
            killed = 1;
        } else if ((L->tick++ % 8) == 7 && tracer_dead(s)) {
            /* the tracer's host process vanished without running its exit path */
            s->resume_sig = 0;
            s->stopped = 0;
            atomic_store(&s->link, PL_DETACHED);
            detached = 1;
            if (s->options & LX_PTRACE_O_EXITKILL) killed = 2;
        }
        if (detached || resumed) {
            out->sig = (s->link == PL_FREE || s->gen != gen) ? 0 : s->resume_sig;
            out->mode = s->mode;
            out->step = (int)s->step;
            out->detached = detached;
            if (s->gen == gen && s->tid == t->tid) {
                if (s->regs_dirty) { out->regs_changed = 1; out->regs = s->regs; s->regs_dirty = 0; }
                if (s->si_dirty) { out->si_changed = 1; out->si = s->si; s->si_dirty = 0; }
                int want_xs = s->xs_dirty || (s->fx_dirty && s->xs_ok); /* one coherent image */
                if (s->fx_dirty) {
                    nfx = malloc(512);
                    if (nfx) { memcpy(nfx, s->fx, 512); fx_dirty = 1; }
                    s->fx_dirty = 0;
                }
                if (want_xs) {
                    nxs = malloc(PT_XS_MAX);
                    if (nxs) { nxlen = s->xs_len; memcpy(nxs, s->xs, nxlen); xs_dirty = 1; }
                    s->xs_dirty = 0;
                }
                if (s->mask_dirty) { newmask = s->sigmask; mask_dirty = 1; s->mask_dirty = 0; }
                if (detached && atomic_load(&s->link) == PL_DETACHED) pt_free_locked(i);
            }
            pt_unlock();
            break;
        }
        pt_unlock();
        if (killed) { result = -1; break; }
        struct pollfd p = { .fd = t->wake_rd, .events = POLLIN };
        if (poll(&p, 1, 100) > 0) { char b[64]; while (read(t->wake_rd, b, sizeof b) > 0) { } }
    }
    if (result > 0 && !rq->ignore_fatal && (atomic_load(&t->flags) & LKD_TF_FATAL)) result = -1;
    atomic_fetch_and(&t->ptrace.flags, ~LKD_PT_STOPPED);
    if (result < 0) { free(nfx); free(nxs); lkd_registry_publish(t); return -1; }
    pt_resume_count++;
    apply_resume(t, L, out, nfx, fx_dirty, nxs, nxlen, xs_dirty, newmask, mask_dirty);
    free(nfx);
    free(nxs);
    if (out->detached) L->slot = -1;
    pt_sync_flags(t);
    lkd_registry_publish(t);
    return 1;
}

/* ---- hooks called by core, sig and task ------------------------------------------------------------ */

int lkd_ptrace_syscall_enter(lkd_task *t, lkd_sysreq *rq, int64_t *skip_ret)
{
    struct pt_local *L = pt_local_of(t, 0);
    if (L) L->emu = 0;
    int i = pt_my_slot(t);
    if (i < 0) { pt_sync_flags(t); return 0; }
    L = pt_local_of(t, 1);
    uint32_t opts = pt_slot_at(i)->options;
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    if (pt_slot_at(i)->trap) {
        /* PTRACE_INTERRUPT: the trap replaces this syscall stop ("the pending condition is cleared") */
        q.kind = PK_GROUP;
        q.code = LX_SIGTRAP | (LX_PTRACE_EVENT_STOP << 8);
        q.nr = (int64_t)rq->nr;
        struct pt_resume rt;
        int rr = pt_do_stop(t, &q, &rt);
        if (rr < 0) return -1;
        return 0;
    }
    q.kind = PK_SYS_ENTRY;
    q.code = LX_SIGTRAP | ((opts & LX_PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
    q.msg = 1; /* PTRACE_EVENTMSG_SYSCALL_ENTRY */
    q.nr = (int64_t)rq->nr;
    struct pt_resume rs;
    int r = pt_do_stop(t, &q, &rs);
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (rs.sig) pt_requeue_signal(t, rs.sig); /* ptrace_report_syscall(): send_sig(signr, current, 1) */
    L->emu = rs.mode == PM_SYSEMU;
    if (rs.regs_changed) {
        /* syscall_trace_enter() reloads the number and the arguments from pt_regs */
        rq->nr = rs.regs.orig_rax;
        rq->a[0] = rs.regs.rdi; rq->a[1] = rs.regs.rsi; rq->a[2] = rs.regs.rdx;
        rq->a[3] = rs.regs.r10; rq->a[4] = rs.regs.r8; rq->a[5] = rs.regs.r9;
    }
    if ((int64_t)rq->nr == -1 || rs.mode == PM_SYSEMU) {
        /* orig_ax = -1, or SYSEMU: the syscall is skipped and the result is what the tracer left in rax */
        *skip_ret = rs.regs_changed ? (int64_t)rs.regs.rax : -(int64_t)LX_ENOSYS;
        return 1;
    }
    return 0;
}

void lkd_ptrace_syscall_exit(lkd_task *t, int64_t *ret)
{
    struct pt_local *L = pt_local_of(t, 0);
    if (L && L->emu) { L->emu = 0; return; } /* SYSEMU alone does not report syscall exits */
    int i = pt_my_slot(t);
    if (i < 0) { pt_sync_flags(t); return; }
    /* syscall_exit_work() re-reads the work flags: a PTRACE_CONT given at the entry stop ends the tracing
     * of this call, so there is no exit stop */
    if (__atomic_load_n(&pt_slot_at(i)->mode, __ATOMIC_ACQUIRE) != PM_SYSCALL) return;
    uint32_t opts = pt_slot_at(i)->options;
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    q.kind = PK_SYS_EXIT;
    q.code = LX_SIGTRAP | ((opts & LX_PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
    q.msg = 2; /* PTRACE_EVENTMSG_SYSCALL_EXIT */
    q.nr = atomic_load(&t->cur_nr);
    q.ret = *ret;
    struct pt_resume rs;
    int r = pt_do_stop(t, &q, &rs);
    if (r <= 0) return; /* a kill is seen by the caller through LKD_TF_FATAL at its safe point */
    if (rs.sig) pt_requeue_signal(t, rs.sig);
    if (rs.regs_changed) *ret = (int64_t)rs.regs.rax;
}

void lkd_ptrace_syscall_exit_replaced(lkd_task *t)
{
    struct pt_local *L = pt_local_of(t, 0);
    if (L && L->emu) { L->emu = 0; return; }
    int i = pt_my_slot(t);
    if (i < 0) { pt_sync_flags(t); return; }
    if (__atomic_load_n(&pt_slot_at(i)->mode, __ATOMIC_ACQUIRE) != PM_SYSCALL) return;
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    q.kind = PK_SYS_EXIT;
    q.code = LX_SIGTRAP | ((pt_slot_at(i)->options & LX_PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
    q.msg = 2;
    q.nr = atomic_load(&t->cur_nr);
    q.ret = 0;
    q.ctx_replaced = 1;
    struct pt_resume rs;
    int r = pt_do_stop(t, &q, &rs);
    if (r > 0 && rs.sig) pt_requeue_signal(t, rs.sig);
}

int lkd_ptrace_signal_stop(lkd_task *t, int signo, struct lx_siginfo *info)
{
    if (!(atomic_load(&t->ptrace.flags) & LKD_PT_TRACED)) return signo;
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    q.kind = PK_SIGNAL;
    q.code = signo;
    q.si = info;
    q.nr = -1;
    struct pt_resume rs;
    int r = pt_do_stop(t, &q, &rs);
    if (r < 0) return -1;
    if (r == 0) return signo;
    if (rs.si_changed) *info = rs.si;
    return rs.sig;
}

int lkd_ptrace_event(lkd_task *t, int event, uint64_t msg)
{
    int i = pt_my_slot(t);
    if (i < 0) {
        if (atomic_load(&t->ptrace.flags) & LKD_PT_TRACED) pt_sync_flags(t);
        return 0;
    }
    uint32_t opts = pt_slot_at(i)->options;
    int enabled = event >= 1 && event <= 7 && (opts & (1u << event));
    int stopped = 0;
    if (enabled && event >= LX_PTRACE_EVENT_FORK && event <= LX_PTRACE_EVENT_CLONE && msg) {
        /* Linux attaches the new task in copy_process(), before the event stop is reported. Here a forked
         * child links itself from its own host process: wait (bounded) until the link exists, so that the
         * tracer's wait4(new pid) right after GETEVENTMSG finds a tracee and not ECHILD. */
        for (int k = 0; k < 10000 && pt_find_tracee((int32_t)msg) < 0 && pt_tid_alive((int32_t)msg); k++) usleep(200);
    }
    if (enabled) {
        struct pt_stop_req q;
        memset(&q, 0, sizeof q);
        q.kind = PK_EVENT;
        q.code = (event << 8) | LX_SIGTRAP;
        q.msg = msg;
        q.nr = atomic_load(&t->cur_nr);
        struct pt_resume rs;
        int r = pt_do_stop(t, &q, &rs);
        if (r < 0) return 1; /* killed: the task dies at its next safe point */
        stopped = r > 0;     /* an ordinary signal given at an event stop is ignored (ptrace_event()) */
    }
    /* the exit stop of execve that follows is made by lkd_syscall (lkd_ptrace_syscall_exit_replaced) */
    return stopped;
}

int lkd_ptrace_stop(lkd_task *t, lkd_stop_kind kind, int signo, int *resume_signo)
{
    if (resume_signo) *resume_signo = signo;
    int i = pt_my_slot(t);
    if (i < 0) return 0;
    struct pt_slot *s = pt_slot_at(i);
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    q.nr = -1;
    switch (kind) {
    case LKD_STOP_SYSCALL_ENTRY: case LKD_STOP_SYSCALL_EXIT:
        q.kind = kind == LKD_STOP_SYSCALL_ENTRY ? PK_SYS_ENTRY : PK_SYS_EXIT;
        q.code = LX_SIGTRAP | ((s->options & LX_PTRACE_O_TRACESYSGOOD) ? 0x80 : 0);
        q.msg = kind == LKD_STOP_SYSCALL_ENTRY ? 1 : 2;
        q.nr = atomic_load(&t->cur_nr);
        break;
    case LKD_STOP_EXEC:
        q.kind = PK_EVENT;
        q.code = (LX_PTRACE_EVENT_EXEC << 8) | LX_SIGTRAP;
        q.msg = (uint64_t)t->tid;
        break;
    case LKD_STOP_EVENT:
        q.kind = PK_EVENT;
        q.code = (signo << 8) | LX_SIGTRAP;
        break;
    case LKD_STOP_GROUP:
        /* do_jobctl_trap(): seized tasks get a PTRACE_EVENT_STOP trap, others a plain stop whose
         * siginfo does not exist (GETSIGINFO fails with EINVAL) and is reported as CLD_STOPPED. */
        q.kind = PK_GROUP;
        if (s->seized) q.code = signo | (LX_PTRACE_EVENT_STOP << 8);
        else { q.code = signo; q.no_siginfo = 1; q.why = LX_CLD_STOPPED; }
        break;
    default: /* LKD_STOP_SIGNAL */
        q.kind = PK_SIGNAL;
        q.code = signo;
        break;
    }
    struct pt_resume rs;
    int r = pt_do_stop(t, &q, &rs);
    if (r < 0) return -1;
    if (r > 0 && resume_signo) *resume_signo = rs.sig;
    return 0;
}

/* PTRACE_INTERRUPT: the pending trap becomes a PTRACE_EVENT_STOP stop at the next safe point. Called by
 * the sig module when it sees LKD_TF_TRACE_STOP in lkd_signal_next (docs/requests/ptrace-1.md). Returns
 * 0, or -1 if the task was killed in the stop. */
int lkd_ptrace_trap_stop(lkd_task *t)
{
    int i = pt_my_slot(t);
    if (i < 0) { atomic_fetch_and(&t->flags, ~LKD_TF_TRACE_STOP); return 0; }
    struct pt_stop_req q;
    memset(&q, 0, sizeof q);
    q.kind = PK_GROUP;
    q.code = LX_SIGTRAP | (LX_PTRACE_EVENT_STOP << 8);
    q.nr = -1;
    struct pt_resume rs;
    return pt_do_stop(t, &q, &rs) < 0 ? -1 : 0;
}
