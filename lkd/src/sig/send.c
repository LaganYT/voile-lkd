/* send.c: sending signals to processes, threads and groups, inside this host process or to another
 * process of the family through the family service; SIGCHLD to parents. Owner: sig.
 * Models kernel/signal.c v6.12: check_kill_permission, kill_ok_by_cred, kill_something_info,
 * __kill_pgrp_info, do_send_specific, do_rt_sigqueueinfo, do_notify_parent, do_notify_parent_cldstop. */
#include <unistd.h>

#include "net.h"
#include "sig_priv.h"

enum { MODE_PID = 1, MODE_TID = 2 };

/* QUEUE_SIGNAL payload (fixed layout: every process of a family runs the same build). */
struct sig_remote {
    int32_t mode, tgid, tid, sig;
    int32_t has_info, _pad;
    struct sig_sender sender;
    struct lx_siginfo info;
};

/* CHILD_EVENT payload. */
struct sig_child_remote {
    int32_t parent_pid, child_pid;
    uint32_t child_uid;
    int32_t code, status;
};

void sig_sender_of(lkd_task *t, struct sig_sender *s)
{
    memset(s, 0, sizeof *s);
    if (!t || t->magic != LKD_TASK_MAGIC) { s->is_kernel = 1; return; }
    s->pid = t->proc->pid;
    s->tid = t->tid;
    s->sid = t->proc->sid;
    s->uid = t->proc->cred.uid;
    s->euid = t->proc->cred.euid;
    s->cap_kill = (int)((t->proc->cred.cap_eff >> 5) & 1); /* CAP_KILL */
}

void sig_info_user(struct lx_siginfo *info, int sig, int code, const struct sig_sender *s)
{
    memset(info, 0, sizeof *info);
    info->si_signo = sig;
    info->si_code = code;
    if (s && !s->is_kernel) {
        info->u.kill.pid = s->pid;
        info->u.kill.uid = s->uid;
    }
}

/* check_kill_permission for a target process whose credentials we hold. */
static int kill_permitted(const struct sig_sender *s, int32_t tpid, int32_t tsid, uint32_t tuid, uint32_t tsuid,
                          int sig, const struct lx_siginfo *info)
{
    if (s->is_kernel) return 0;
    if (info && info->si_code > 0) return 0;      /* !si_fromuser: kernel-generated */
    if (s->pid == tpid) return 0;                 /* same_thread_group */
    if (s->euid == tsuid || s->euid == tuid || s->uid == tsuid || s->uid == tuid || s->cap_kill) return 0;
    if (sig == LX_SIGCONT && (tsid == 0 || tsid == s->sid)) return 0;
    return -LX_EPERM;
}

/* A task of THIS host process for the given ids, with a reference, or NULL. */
static lkd_task *local_task(int mode, int32_t tgid, int32_t tid)
{
    lkd_task *t = NULL;
    if (mode == MODE_TID) {
        t = lkd_task_lookup(tid);
        if (t && tgid > 0 && t->proc->pid != tgid) { lkd_task_put(t); t = NULL; }
    } else {
        t = lkd_task_lookup(tgid);
        if (t && t->proc->pid != tgid) { lkd_task_put(t); t = NULL; }
        if (!t) {
            int32_t tids[64];
            int n = lkd_tid_list(tgid, tids, 64);
            for (int i = 0; i < n && !t; i++) {
                t = lkd_task_lookup(tids[i]);
                if (t && t->proc->pid != tgid) { lkd_task_put(t); t = NULL; }
            }
        }
    }
    if (t && (t->magic != LKD_TASK_MAGIC || t->proc->host_pid != getpid())) { lkd_task_put(t); t = NULL; }
    return t;
}

/* Deliver in this host process. info NULL: SI_USER (process) or SI_TKILL (thread) from the sender. */
static int deliver_local(const struct sig_sender *s, int mode, int32_t tgid, int32_t tid, int sig,
                         const struct lx_siginfo *info_in)
{
    lkd_task *t = local_task(mode, tgid, tid);
    if (!t) return -LX_ESRCH;
    struct lkd_proc *p = t->proc;
    int e = kill_permitted(s, p->pid, p->sid, p->cred.uid, p->cred.suid, sig, info_in);
    if (e < 0 || sig == 0) { lkd_task_put(t); return e; }
    struct lx_siginfo info;
    const struct lx_siginfo *ip = NULL;
    if (info_in) { info = *info_in; info.si_signo = sig; ip = &info; }
    else if (!s->is_kernel) { sig_info_user(&info, sig, mode == MODE_TID ? LX_SI_TKILL : LX_SI_USER, s); ip = &info; }
    struct lkd_sigproc *sp = sig_proc(p);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    e = sig_generate_locked(t, sp, sig, ip, mode == MODE_TID, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
    lkd_net_signal_queued(t);
    lkd_task_put(t);
    return e;
}

static void op_queue_signal(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    if (req->len != sizeof(struct sig_remote)) { resp->status = -LX_EPROTO; return; }
    struct sig_remote r;
    memcpy(&r, req->data, sizeof r);
    if (r.sig < 0 || r.sig > LX_NSIG || (r.mode != MODE_PID && r.mode != MODE_TID)) { resp->status = -LX_EINVAL; return; }
    resp->status = deliver_local(&r.sender, r.mode, r.tgid, r.tid, r.sig, r.has_info ? &r.info : NULL);
}

static int send_remote(int32_t host_pid, const struct sig_sender *s, int mode, int32_t tgid, int32_t tid, int sig,
                       const struct lx_siginfo *info)
{
    struct sig_remote r;
    memset(&r, 0, sizeof r);
    r.mode = mode; r.tgid = tgid; r.tid = tid; r.sig = sig;
    r.sender = *s;
    if (info) { r.has_info = 1; r.info = *info; }
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_QUEUE_SIGNAL;
    req.target_pid = tgid;
    req.target_tid = tid;
    req.caller_pid = s->pid;
    req.caller_tid = s->tid;
    req.caller_uid = s->euid;
    req.data = &r;
    req.len = sizeof r;
    int e = lkd_famsvc_call(host_pid, &req, &resp, 0);
    lkd_famsvc_msg_free(&resp);
    return e;
}

/* Signal a whole process (group_send_sig_info semantics). */
static int send_to_pid(const struct sig_sender *s, int32_t pid, int sig, const struct lx_siginfo *info)
{
    if (pid <= 0) return -LX_ESRCH;
    lkd_proc_info *pi = malloc(sizeof *pi);
    if (!pi) return -LX_ENOMEM;
    int e = lkd_proc_lookup(pid, pi);
    if (e < 0) {
        free(pi);
        /* The registry may not list a task of this process yet (unit tests, races): look locally. */
        return deliver_local(s, MODE_PID, pid, 0, sig, info);
    }
    if (pi->state == LKD_PS_ZOMBIE || pi->state == LKD_PS_DEAD) {
        /* A zombie still has its signal state until reaped: permission check, then nothing to do. */
        e = kill_permitted(s, pi->pid, pi->sid, pi->uid, pi->euid, sig, info);
        free(pi);
        return e;
    }
    int32_t host = pi->host_pid;
    free(pi);
    if (host == getpid()) return deliver_local(s, MODE_PID, pid, 0, sig, info);
    e = send_remote(host, s, MODE_PID, pid, 0, sig, info);
    if (e == -LX_ESRCH) {
        /* The service is gone: the process exited meanwhile; a zombie answers 0 like Linux. */
        lkd_proc_info *p2 = malloc(sizeof *p2);
        if (p2 && lkd_proc_lookup(pid, p2) == 0 && (p2->state == LKD_PS_ZOMBIE || p2->state == LKD_PS_DEAD))
            e = kill_permitted(s, p2->pid, p2->sid, p2->uid, p2->euid, sig, info);
        free(p2);
    }
    return e;
}

static int send_to_tid(const struct sig_sender *s, int32_t tgid, int32_t tid, int sig, const struct lx_siginfo *info)
{
    if (tid <= 0) return -LX_ESRCH;
    lkd_tid_info ti;
    if (lkd_tid_lookup(tid, &ti) < 0) return deliver_local(s, MODE_TID, tgid, tid, sig, info);
    if (tgid > 0 && ti.tgid != tgid) return -LX_ESRCH;
    lkd_proc_info *pi = malloc(sizeof *pi);
    if (!pi) return -LX_ENOMEM;
    int e = lkd_proc_lookup(ti.tgid, pi);
    int32_t host = e == 0 ? pi->host_pid : getpid();
    free(pi);
    if (host == getpid()) return deliver_local(s, MODE_TID, tgid, tid, sig, info);
    return send_remote(host, s, MODE_TID, tgid, tid, sig, info);
}

/* ---- public API (sig.h) ----------------------------------------------------------------------- */

int lkd_sig_send_pid(lkd_task *sender, int32_t pid, int signo, const struct lx_siginfo *info)
{
    if (signo < 0 || signo > LX_NSIG) return -LX_EINVAL;
    struct sig_sender s;
    sig_sender_of(sender, &s);
    return send_to_pid(&s, pid, signo, info);
}

int lkd_sig_send_tid(lkd_task *sender, int32_t tgid, int32_t tid, int signo, const struct lx_siginfo *info)
{
    if (signo < 0 || signo > LX_NSIG) return -LX_EINVAL;
    struct sig_sender s;
    sig_sender_of(sender, &s);
    return send_to_tid(&s, tgid, tid, signo, info);
}

#define SIG_LIST_MAX 512

/* kill(-1) and kill(-pgrp) with the Linux result; *count gets the number of processes reached. */
static int kill_group(const struct sig_sender *s, int32_t pgrp, int sig, const struct lx_siginfo *info, int *count)
{
    lkd_proc_info *list = malloc(sizeof *list * SIG_LIST_MAX);
    if (!list) return -LX_ENOMEM;
    int n = lkd_proc_list(list, SIG_LIST_MAX);
    int ok = 0;
    *count = 0;
    int ret;
    if (pgrp == -1) {
        int retval = 0;
        for (int i = 0; i < n; i++) {
            if (list[i].pid <= 1 || list[i].pid == s->pid) continue;
            int err = send_to_pid(s, list[i].pid, sig, info);
            ++*count;
            if (!err) ok++;
            if (err != -LX_EPERM) retval = err;
        }
        ret = *count ? retval : -LX_ESRCH;
    } else {
        ret = -LX_ESRCH;
        for (int i = 0; i < n; i++) {
            if (list[i].pgid != pgrp) continue;
            int err = send_to_pid(s, list[i].pid, sig, info);
            ++*count;
            if (!err) ok++;
            if (ret) ret = err;
        }
    }
    free(list);
    *count = ok;
    return ret;
}

int lkd_sig_send_pgrp(lkd_task *sender, int32_t pgrp, int signo, const struct lx_siginfo *info)
{
    if (signo < 0 || signo > LX_NSIG) return -LX_EINVAL;
    if (pgrp == 0 || pgrp < -1) return -LX_EINVAL;
    struct sig_sender s;
    sig_sender_of(sender, &s);
    int count = 0;
    int r = kill_group(&s, pgrp, signo, info, &count);
    return r < 0 ? r : count;
}

/* The kill syscall body (pid semantics of kill(2)), used by syscalls.c. */
int sig_kill(lkd_task *t, int32_t pid, int sig, const struct lx_siginfo *info);
int sig_kill(lkd_task *t, int32_t pid, int sig, const struct lx_siginfo *info)
{
    struct sig_sender s;
    sig_sender_of(t, &s);
    if (pid > 0) return send_to_pid(&s, pid, sig, info);
    if (pid == INT32_MIN) return -LX_ESRCH;
    int count;
    if (pid == -1) return kill_group(&s, -1, sig, info, &count);
    int32_t pg = pid == 0 ? (t ? t->proc->pgid : 0) : -pid;
    if (pid == 0 && t) lkd_proc_refresh_ids(t->proc), pg = t->proc->pgid;
    return kill_group(&s, pg, sig, info, &count);
}

int sig_tkill(lkd_task *t, int32_t tgid, int32_t tid, int sig);
int sig_tkill(lkd_task *t, int32_t tgid, int32_t tid, int sig)
{
    struct sig_sender s;
    sig_sender_of(t, &s);
    int e = send_to_tid(&s, tgid, tid, sig, NULL);
    return e;
}

/* ---- SIGCHLD to the parent -------------------------------------------------------------------- */

/* Runs in the parent's host process. Returns 1 when the child is to be reaped automatically. */
static int child_event_local(int32_t parent_pid, int32_t child_pid, uint32_t child_uid, int code, int status)
{
    lkd_task *pt = local_task(MODE_PID, parent_pid, 0);
    if (!pt) return 0;
    struct lkd_sigproc *sp = sig_proc(pt->proc);
    struct sig_wakeset ws = { 0 };
    struct lx_siginfo info;
    memset(&info, 0, sizeof info);
    info.si_signo = LX_SIGCHLD;
    info.si_code = code;
    info.u.chld.pid = child_pid;
    info.u.chld.uid = child_uid;
    info.u.chld.status = status;
    int autoreap = 0;
    sig_lock(sp);
    const struct lx_sigaction *a = &sig_hand(pt)->act[LX_SIGCHLD - 1];
    int exited = code == LX_CLD_EXITED || code == LX_CLD_KILLED || code == LX_CLD_DUMPED;
    int send = 1;
    if (exited) {
        /* do_notify_parent: SIG_IGN or SA_NOCLDWAIT make the child reaped; SIG_IGN sends nothing */
        if (a->handler == LX_SIG_IGN || (a->flags & LX_SA_NOCLDWAIT)) {
            autoreap = 1;
            if (a->handler == LX_SIG_IGN) send = 0;
        }
    } else {
        /* do_notify_parent_cldstop: nothing with SIG_IGN or SA_NOCLDSTOP */
        if (a->handler == LX_SIG_IGN || (a->flags & LX_SA_NOCLDSTOP)) send = 0;
    }
    if (send) (void)sig_generate_locked(pt, sp, LX_SIGCHLD, &info, 0, &ws);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
    if (send) lkd_net_signal_queued(pt);
    lkd_task_put(pt);
    return autoreap;
}

static void op_child_event(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    if (req->len != sizeof(struct sig_child_remote)) { resp->status = -LX_EPROTO; return; }
    struct sig_child_remote r;
    memcpy(&r, req->data, sizeof r);
    resp->status = child_event_local(r.parent_pid, r.child_pid, r.child_uid, r.code, r.status);
}

int lkd_sig_child_event(int32_t parent_pid, int32_t child_pid, uint32_t child_uid, int code, int status)
{
    if (parent_pid <= 0) return 0;
    lkd_proc_info *pi = malloc(sizeof *pi);
    if (!pi) return 0;
    int e = lkd_proc_lookup(parent_pid, pi);
    int32_t host = e == 0 ? pi->host_pid : getpid();
    char state = e == 0 ? pi->state : LKD_PS_RUNNING;
    free(pi);
    if (state == LKD_PS_ZOMBIE || state == LKD_PS_DEAD) return 0;
    if (host == getpid()) return child_event_local(parent_pid, child_pid, child_uid, code, status);
    struct sig_child_remote r = { parent_pid, child_pid, child_uid, code, status };
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    req.op = LKD_FAMSVC_OP_CHILD_EVENT;
    req.target_pid = parent_pid;
    req.data = &r;
    req.len = sizeof r;
    int s = lkd_famsvc_call(host, &req, &resp, 0);
    lkd_famsvc_msg_free(&resp);
    return s > 0 ? 1 : 0;
}

void sig_notify_parent(struct lkd_proc *p, int code, int status)
{
    lkd_proc_refresh_ids(p);
    (void)lkd_sig_child_event(p->ppid, p->pid, p->cred.uid, code, status);
    lkd_task_wake_waiters(p->ppid);
}

/* ---- the whole process dies -------------------------------------------------------------------- */

void lkd_sig_kill_all_threads(lkd_task *t)
{
    struct lkd_sigproc *sp = sig_proc(t->proc);
    struct sig_wakeset ws = { 0 };
    sig_lock(sp);
    if (!sp->group_exit) {
        sp->group_exit = 1;
        sp->group_exit_status = (int)t->proc->exit_code;
    }
    sp->group_exit_done = 1; /* the caller (task) runs the exit itself */
    pthread_mutex_lock(&t->proc->lock);
    for (lkd_task *x = t->proc->threads; x; x = x->next_in_proc) {
        atomic_fetch_or(&x->flags, LKD_TF_FATAL);
        sig_wakeset_add(&ws, x);
    }
    pthread_mutex_unlock(&t->proc->lock);
    sig_unlock(sp);
    sig_wakeset_run(&ws);
}

/* ---- status of a remote task (procfs) ---------------------------------------------------------- */

static void op_sig_status(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    lkd_task *t = local_task(req->target_tid ? MODE_TID : MODE_PID, req->target_pid, req->target_tid);
    if (!t) { resp->status = -LX_ESRCH; return; }
    struct lkd_sig_status *st = malloc(sizeof *st);
    if (!st) { lkd_task_put(t); resp->status = -LX_ENOMEM; return; }
    resp->status = lkd_sig_status(t, st);
    resp->data = st;
    resp->len = sizeof *st;
    lkd_task_put(t);
}

void sig_send_init(void)
{
    lkd_famsvc_register(LKD_FAMSVC_OP_QUEUE_SIGNAL, op_queue_signal);
    lkd_famsvc_register(LKD_FAMSVC_OP_CHILD_EVENT, op_child_event);
    lkd_famsvc_register(LKD_FAMSVC_OP_SIG_STATUS, op_sig_status);
}
