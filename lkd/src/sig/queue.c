/* queue.c: pending sets, siginfo queues, generation and dequeue. Owner: sig.
 * Models kernel/signal.c v6.12: prepare_signal, __send_signal_locked, complete_signal, next_signal,
 * collect_signal, __set_task_blocked/retarget_shared_pending, force_sig_info_to_task. */
#include "sig_priv.h"

void sq_init(struct sq *q)
{
    q->head = NULL;
    q->tail = &q->head;
}

void sq_flush(struct sq *q, uint64_t mask, uint32_t *queued)
{
    struct sq_node **pp = &q->head;
    while (*pp) {
        struct sq_node *n = *pp;
        int s = n->info.si_signo;
        if (s >= 1 && s <= LX_NSIG && (mask & LX_SIGBIT(s))) {
            *pp = n->next;
            free(n);
            if (queued && *queued) (*queued)--;
        } else {
            pp = &n->next;
        }
    }
    q->tail = pp;
}

/* Remove the first record of sig; returns 1 if another record of sig remains. info is filled; when no
 * record exists (out of queue space at generation, or a coalesced standard signal) the info is the
 * SI_USER placeholder of collect_signal. */
static int sq_collect(struct sq *q, int sig, struct lx_siginfo *info, uint32_t *queued)
{
    struct sq_node **pp = &q->head, *found = NULL;
    while (*pp) {
        if ((*pp)->info.si_signo == sig) { found = *pp; *pp = found->next; break; }
        pp = &(*pp)->next;
    }
    if (found) {
        if (!*pp) q->tail = pp;
        *info = found->info;
        free(found);
        if (*queued) (*queued)--;
        for (struct sq_node *n = q->head; n; n = n->next)
            if (n->info.si_signo == sig) return 1;
        return 0;
    }
    memset(info, 0, sizeof *info);
    info->si_signo = sig;
    info->si_code = LX_SI_USER;
    return 0;
}

static void sq_push(struct sq *q, struct sq_node *n)
{
    n->next = NULL;
    *q->tail = n;
    q->tail = &n->next;
}

/* next_signal: synchronous signals first, then the lowest number. */
int sig_next_signal(uint64_t bits)
{
    if (!bits) return 0;
    uint64_t x = bits & SIG_SYNC_MASK;
    if (!x) x = bits;
    return __builtin_ctzll(x) + 1;
}

static int is_traced(const lkd_task *t)
{
    return (atomic_load(&((lkd_task *)t)->ptrace.flags) & LKD_PT_TRACED) != 0;
}

/* sig_ignored + sig_task_ignored (kernel/signal.c:80-110). Called with the sig lock. */
int sig_is_ignored_locked(lkd_task *t, int sig)
{
    struct sig_task *st = sig_task_of(t);
    uint64_t bit = LX_SIGBIT(sig);
    if ((atomic_load(&t->sigmask) | st->real_blocked) & bit) return 0;
    if (is_traced(t) && sig != LX_SIGKILL) return 0;
    uint64_t h = sig_hand(t)->act[sig - 1].handler;
    if (h == LX_SIG_IGN) return 1;
    return h == LX_SIG_DFL && (SIG_KERNEL_IGNORE_MASK & bit) != 0;
}

static int deliverable_bits(lkd_task *t, struct lkd_sigproc *sp)
{
    return ((atomic_load(&t->pending.bits) | atomic_load(&sp->shared_bits)) & ~atomic_load(&t->sigmask)) != 0;
}

void sig_recalc_locked(lkd_task *t, struct lkd_sigproc *sp)
{
    if (deliverable_bits(t, sp)) atomic_fetch_or(&t->flags, LKD_TF_SIGPENDING);
    else atomic_fetch_and(&t->flags, ~LKD_TF_SIGPENDING);
}

void sig_flush_process_locked(struct lkd_proc *p, struct lkd_sigproc *sp, uint64_t mask)
{
    sq_flush(&sp->shared_q, mask, &sp->queued);
    atomic_fetch_and(&sp->shared_bits, ~mask);
    pthread_mutex_lock(&p->lock);
    for (lkd_task *x = p->threads; x; x = x->next_in_proc) {
        struct sig_task *st = sig_task_of(x);
        sq_flush(&st->q, mask, &sp->queued);
        atomic_fetch_and(&x->pending.bits, ~mask);
    }
    pthread_mutex_unlock(&p->lock);
}

/* prepare_signal: job-control side effects of stop signals and SIGCONT; returns 0 to drop. */
static int prepare_signal(lkd_task *target, struct lkd_sigproc *sp, int sig, struct sig_wakeset *ws)
{
    struct lkd_proc *p = target->proc;
    if (sp->group_exit) return sig == LX_SIGKILL && !sp->group_exit_done; /* dying: nothing to do */
    uint64_t bit = LX_SIGBIT(sig);
    if (bit & SIG_STOP_MASK) {
        sig_flush_process_locked(p, sp, LX_SIGBIT(LX_SIGCONT));
    } else if (sig == LX_SIGCONT) {
        sig_flush_process_locked(p, sp, SIG_STOP_MASK);
        int why = 0;
        if (sp->stop_state == SP_STOPPED) why = LX_CLD_CONTINUED;
        else if (sp->stop_state == SP_STOPPING) why = LX_CLD_STOPPED;
        if (why) {
            sp->cld_notify = why;
            sp->cld_status = why == LX_CLD_CONTINUED ? LX_SIGCONT : sp->stop_signo;
            sp->stop_state = SP_RUNNING;
            sp->stop_count = 0;
        }
        pthread_mutex_lock(&p->lock);
        for (lkd_task *x = p->threads; x; x = x->next_in_proc) {
            atomic_fetch_and(&x->flags, ~LKD_TF_GROUP_STOP);
            if (why) {
                /* the first thread through lkd_signal_next reports the continue to the parent */
                atomic_fetch_or(&x->flags, LKD_TF_SIGPENDING);
                sig_wakeset_add(ws, x);
            }
        }
        pthread_mutex_unlock(&p->lock);
    }
    return !sig_is_ignored_locked(target, sig);
}

static int wants_signal(lkd_task *x, int sig)
{
    uint32_t f = atomic_load(&x->flags);
    if (f & LKD_TF_EXITING) return 0;
    if (sig == LX_SIGKILL) return 1;
    if (atomic_load(&x->sigmask) & LX_SIGBIT(sig)) return 0;
    struct sig_task *st = sig_task_of(x);
    if (st->parked) return 0;
    return 1;
}

static void complete_signal(lkd_task *target, struct lkd_sigproc *sp, int sig, int thread_directed, struct sig_wakeset *ws)
{
    struct lkd_proc *p = target->proc;
    lkd_task *t = NULL;
    if (wants_signal(target, sig)) {
        t = target;
    } else if (thread_directed) {
        return;
    } else {
        pthread_mutex_lock(&p->lock);
        for (lkd_task *x = p->threads; x; x = x->next_in_proc)
            if (wants_signal(x, sig)) { t = x; break; }
        pthread_mutex_unlock(&p->lock);
        if (!t) return;
    }
    uint64_t bit = LX_SIGBIT(sig);
    uint64_t h = sig_hand(t)->act[sig - 1].handler;
    int fatal = !(bit & (SIG_KERNEL_IGNORE_MASK | SIG_STOP_MASK)) && (h == LX_SIG_DFL || sig == LX_SIGKILL);
    if (fatal && !sp->group_exit && !(sig_task_of(t)->real_blocked & bit) &&
        (sig == LX_SIGKILL || !is_traced(t)) && !(bit & SIG_COREDUMP_MASK)) {
        /* The whole process dies now (complete_signal's group exit): status = the signal. */
        sp->group_exit = 1;
        sp->group_exit_status = sig;
        pthread_mutex_lock(&p->lock);
        for (lkd_task *x = p->threads; x; x = x->next_in_proc) {
            atomic_fetch_or(&x->flags, LKD_TF_FATAL);
            sig_wakeset_add(ws, x);
        }
        pthread_mutex_unlock(&p->lock);
        return;
    }
    atomic_fetch_or(&t->flags, LKD_TF_SIGPENDING);
    sig_wakeset_add(ws, t);
}

static uint32_t queue_limit(struct lkd_proc *p)
{
    uint64_t lim = p->rlimits[LX_RLIMIT_SIGPENDING].rlim_cur;
    if (lim == 0 && p->rlimits[LX_RLIMIT_SIGPENDING].rlim_max == 0) lim = LX_RLIM_INFINITY; /* not set */
    return lim > SIG_QUEUE_HARD_MAX ? SIG_QUEUE_HARD_MAX : (uint32_t)lim;
}

int sig_generate_locked(lkd_task *target, struct lkd_sigproc *sp, int sig, const struct lx_siginfo *info,
                        int thread_directed, struct sig_wakeset *ws)
{
    if (!prepare_signal(target, sp, sig, ws)) return 0;
    struct sq *q;
    _Atomic uint64_t *bits;
    if (thread_directed) {
        q = &sig_task_of(target)->q;
        bits = &target->pending.bits;
    } else {
        q = &sp->shared_q;
        bits = &sp->shared_bits;
    }
    uint64_t bit = LX_SIGBIT(sig);
    /* legacy_queue: a standard signal already pending is not queued twice */
    if (sig < LX_SIGRTMIN && (atomic_load(bits) & bit)) return 0;
    if (sig != LX_SIGKILL) {
        int kernel = !info || info->si_code == LX_SI_KERNEL;
        int override = sig < LX_SIGRTMIN && (kernel || info->si_code >= 0);
        struct sq_node *n = NULL;
        if (override || sp->queued < queue_limit(target->proc)) n = malloc(sizeof *n);
        if (n) {
            if (info) n->info = *info;
            else { memset(&n->info, 0, sizeof n->info); n->info.si_code = LX_SI_KERNEL; }
            n->info.si_signo = sig;
            sq_push(q, n);
            sp->queued++;
        } else if (!kernel && sig >= LX_SIGRTMIN && info->si_code != LX_SI_USER) {
            return -LX_EAGAIN; /* queue overflow for sigqueue-style signals */
        }
        /* else: silent loss of the siginfo, the signal itself stays pending */
    }
    atomic_fetch_or(bits, bit);
    sig_sfd_notify_locked(sp, sig);
    int dying = sp->group_exit;
    complete_signal(target, sp, sig, thread_directed, ws);
    if (!dying && sp->group_exit && lkd_trace_level()) sig_trace_death(target, sig, info, 0);
    return 0;
}

int sig_dequeue_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t allowed, struct lx_siginfo *info)
{
    struct sig_task *st = sig_task_of(t);
    uint64_t b = atomic_load(&t->pending.bits) & allowed;
    if (b) {
        int s = sig_next_signal(b);
        int more = sq_collect(&st->q, s, info, &sp->queued);
        if (!more) atomic_fetch_and(&t->pending.bits, ~LX_SIGBIT(s));
        return s;
    }
    b = atomic_load(&sp->shared_bits) & allowed;
    if (b) {
        int s = sig_next_signal(b);
        int more = sq_collect(&sp->shared_q, s, info, &sp->queued);
        if (!more) atomic_fetch_and(&sp->shared_bits, ~LX_SIGBIT(s));
        return s;
    }
    return 0;
}

int sig_peek_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t allowed)
{
    uint64_t b = atomic_load(&t->pending.bits) & allowed;
    if (b) return sig_next_signal(b);
    return sig_next_signal(atomic_load(&sp->shared_bits) & allowed);
}

void sig_set_blocked_locked(lkd_task *t, struct lkd_sigproc *sp, uint64_t mask, struct sig_wakeset *ws)
{
    mask &= ~LX_SIG_UNBLOCKABLE;
    uint64_t old = atomic_load(&t->sigmask);
    uint64_t newly = mask & ~old;
    uint64_t shared = atomic_load(&sp->shared_bits) & newly;
    atomic_store(&t->sigmask, mask);
    if (shared && ws) {
        /* retarget_shared_pending: another thread that does not block them takes them */
        pthread_mutex_lock(&t->proc->lock);
        for (lkd_task *x = t->proc->threads; x && shared; x = x->next_in_proc) {
            if (x == t || (atomic_load(&x->flags) & LKD_TF_EXITING)) continue;
            uint64_t take = shared & ~atomic_load(&x->sigmask);
            if (!take) continue;
            shared &= ~take;
            atomic_fetch_or(&x->flags, LKD_TF_SIGPENDING);
            sig_wakeset_add(ws, x);
        }
        pthread_mutex_unlock(&t->proc->lock);
    }
    sig_recalc_locked(t, sp);
}

void sig_force_locked(lkd_task *t, struct lkd_sigproc *sp, int sig, const struct lx_siginfo *info, struct sig_wakeset *ws)
{
    struct lkd_sighand *h = sig_hand(t);
    uint64_t bit = LX_SIGBIT(sig);
    int blocked = (atomic_load(&t->sigmask) & bit) != 0;
    int ignored = h->act[sig - 1].handler == LX_SIG_IGN;
    if (blocked || ignored) {
        pthread_mutex_lock(&h->lock);
        h->act[sig - 1].handler = LX_SIG_DFL;
        sig_hand_update_masks(h);
        pthread_mutex_unlock(&h->lock);
        if (blocked) atomic_fetch_and(&t->sigmask, ~bit);
    }
    (void)sig_generate_locked(t, sp, sig, info, 1, ws);
    sig_recalc_locked(t, sp);
}

void sig_send_task_full(lkd_task *target, int sig, const struct lx_siginfo *info, int thread_directed, int force)
{
    if (!target || sig < 1 || sig > LX_NSIG) return;
    struct lkd_sigproc *sp = sig_proc(target->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    if (force) sig_force_locked(target, sp, sig, info, &ws);
    else (void)sig_generate_locked(target, sp, sig, info, thread_directed, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
    lkd_net_signal_queued(target);
}

void lkd_sig_send_task(lkd_task *target, int signo, const struct lx_siginfo *info, int thread_directed)
{
    sig_send_task_full(target, signo, info, thread_directed, 0);
}
