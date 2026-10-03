/* transport.c: the shared ptrace table, its lock and the cross-process notifications. Owner: ptrace.
 *
 * imported from HL proc_ptrace.c (the durable shared record plus fcntl byte lock plus process-local mutex
 * scheme, lock_state()/unlock_state(), and "notifications carry no state: the shared record is
 * authoritative if a wake is lost"). Adapted: one family-wide table of slots instead of one file per
 * fork, so that ATTACH/SEIZE of a non-child and tracer lookups work from any process of the family; the
 * private notification socket pair is replaced by the family service (famsvc.h), opcodes 64 and 65.
 * SPDX-License-Identifier: Apache-2.0 (HyperLinux, Copyright 2025 Moritz Angermann, zw3rk pte. ltd.)
 *
 * The table is an unlinked temporary file mapped MAP_SHARED, created by the first process of the family
 * in lkd_ptrace_init and inherited by fork (mapping and descriptor). Its descriptor is not close-on-exec
 * and LKD_PTRACE_FD names it, so a host re-execution of the runtime re-attaches (like LKD_FAMILY_FD).
 * A process must never open a second descriptor of the file: closing any descriptor of a file drops the
 * process' fcntl locks on it.
 */
#include <fcntl.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pt_priv.h"

#define PT_HDR 4096u
#define OP_WAKE (LKD_FAMSVC_OP_PTRACE_FIRST + 0)
#define OP_NOTIFY (LKD_FAMSVC_OP_PTRACE_FIRST + 1)

struct pt_hdr { uint64_t magic; uint32_t version, nslots, stride, hdr; };

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;      /* rank LKD_RANK_PTRACE */
static pthread_mutex_t g_attach_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_fd = -1;
static uint8_t *g_map;
static size_t g_size;
struct pt_stats pt_stats_g;

static size_t stride(void) { return (sizeof(struct pt_slot) + 63u) & ~(size_t)63u; }
static size_t table_size(void) { return PT_HDR + PT_SLOTS * stride(); }

int pt_table_ready(void) { return g_map != NULL; }

struct pt_slot *pt_slot_at(int i)
{
    return (struct pt_slot *)(g_map + PT_HDR + (size_t)i * stride());
}

static int map_fd(int fd, int fresh)
{
    void *m = mmap(NULL, table_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) return (int)LKD_HOST_ERR();
    struct pt_hdr *h = m;
    if (fresh) {
        h->version = PT_VERSION;
        h->nslots = PT_SLOTS;
        h->stride = (uint32_t)stride();
        h->hdr = PT_HDR;
        atomic_thread_fence(memory_order_release);
        h->magic = PT_MAGIC;
    } else if (h->magic != PT_MAGIC || h->version != PT_VERSION || h->nslots != PT_SLOTS ||
               h->stride != stride() || h->hdr != PT_HDR) {
        munmap(m, table_size());
        return -LX_EINVAL;
    }
    g_map = m;
    g_size = table_size();
    g_fd = fd;
    return 0;
}

int pt_table_attach(void)
{
    pthread_mutex_lock(&g_attach_mu);
    int r = 0;
    if (g_map) goto out;
    const char *env = getenv("LKD_PTRACE_FD");
    if (env && *env) {
        int fd = atoi(env);
        struct stat st;
        if (fd > 2 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (size_t)st.st_size == table_size() &&
            map_fd(fd, 0) == 0)
            goto out;
        /* stale variable (not ours, or another build): start a new table */
    }
    const char *tmp = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/lkdpt.XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) { r = (int)LKD_HOST_ERR(); goto out; }
    unlink(path);
    if (ftruncate(fd, (off_t)table_size()) < 0) { r = (int)LKD_HOST_ERR(); close(fd); goto out; }
    r = map_fd(fd, 1);
    if (r < 0) { close(fd); goto out; }
    char v[16];
    snprintf(v, sizeof v, "%d", fd);
    setenv("LKD_PTRACE_FD", v, 1);
out:
    pthread_mutex_unlock(&g_attach_mu);
    return r;
}

/* imported from HL proc_ptrace.c lock_state()/unlock_state(): local mutex, then the byte lock that the
 * host kernel drops if the holder dies. */
void pt_lock(void)
{
    pthread_mutex_lock(&g_mu);
    struct flock fl = { .l_start = 0, .l_len = 1, .l_type = F_WRLCK, .l_whence = SEEK_SET };
    while (fcntl(g_fd, F_SETLKW, &fl) < 0) {
        if (errno == EINTR) continue;
        lkd_log_once("ptrace: table lock failed (errno %d); continuing with the local lock only", errno);
        break;
    }
    atomic_thread_fence(memory_order_acquire);
}

void pt_unlock(void)
{
    atomic_thread_fence(memory_order_release);
    struct flock fl = { .l_start = 0, .l_len = 1, .l_type = F_UNLCK, .l_whence = SEEK_SET };
    while (fcntl(g_fd, F_SETLK, &fl) < 0 && errno == EINTR) { }
    pthread_mutex_unlock(&g_mu);
}

int pt_alloc_locked(void)
{
    for (int i = 0; i < PT_SLOTS; i++) {
        struct pt_slot *s = pt_slot_at(i);
        if (atomic_load(&s->link) != PL_FREE) continue;
        uint32_t gen = s->gen + 1;
        memset((char *)s + offsetof(struct pt_slot, gen), 0, sizeof *s - offsetof(struct pt_slot, gen));
        s->gen = gen;
        atomic_store(&s->tid, 0);
        atomic_store(&s->tracer_pid, 0);
        atomic_store(&s->tracer_tid, 0);
        s->sys_nr = -1;
        atomic_store(&s->link, PL_ATTACHED);
        return i;
    }
    /* Reclaim links whose host process is gone without having run its exit path: a detached link whose
     * tracee died, an exit report nobody will collect because the tracer died. */
    for (int i = 0; i < PT_SLOTS; i++) {
        struct pt_slot *s = pt_slot_at(i);
        uint32_t l = atomic_load(&s->link);
        if ((l == PL_DETACHED && s->tracee_host > 0 && !pt_host_alive(s->tracee_host)) ||
            (l == PL_EXITED && s->tracer_host > 0 && !pt_host_alive(s->tracer_host)) ||
            (l == PL_ATTACHED && s->tracee_host > 0 && !pt_host_alive(s->tracee_host) &&
             s->tracer_host > 0 && !pt_host_alive(s->tracer_host))) {
            pt_free_locked(i);
            return pt_alloc_locked();
        }
    }
    return -1;
}

void pt_free_locked(int i)
{
    struct pt_slot *s = pt_slot_at(i);
    atomic_store(&s->tid, 0);
    atomic_store(&s->tracer_pid, 0);
    atomic_store(&s->tracer_tid, 0);
    s->stopped = 0;
    atomic_store(&s->link, PL_FREE);
}

int pt_find_tracee(int32_t tid)
{
    if (!g_map || tid <= 0) return -1;
    for (int i = 0; i < PT_SLOTS; i++) {
        struct pt_slot *s = pt_slot_at(i);
        uint32_t l = atomic_load(&s->link);
        if (l == PL_ATTACHED && atomic_load(&s->tid) == tid) return i;
    }
    return -1;
}

int pt_host_alive(int32_t host_pid)
{
    if (host_pid <= 0) return 0;
    /* sysctl(KERN_PROC_PID) sees zombies (proc_pidinfo does not always): a process that was killed and not
     * yet waited for by its parent is dead for us. */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, host_pid };
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    memset(&kp, 0, sizeof kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) == 0 && len >= sizeof kp)
        return kp.kp_proc.p_stat != SZOMB && kp.kp_proc.p_pid == host_pid;
    return kill(host_pid, 0) == 0 || errno == EPERM;
}

int pt_tid_alive(int32_t tid)
{
    lkd_tid_info ti;
    if (lkd_tid_lookup(tid, &ti) < 0) return 0;
    return ti.state != LKD_PS_ZOMBIE && ti.state != LKD_PS_DEAD;
}

/* ---- wake of a tracee ----------------------------------------------------------------------------- */

static int wake_local(lkd_task *t, unsigned how)
{
    if (how & PT_WAKE_SYNC) pt_sync_flags(t);
    if (how & PT_WAKE_TRAP) atomic_fetch_or(&t->flags, LKD_TF_TRACE_STOP);
    lkd_task_wake(t);
    if ((how & PT_WAKE_KICK) && lkd_engine() && lkd_engine()->kick) lkd_engine()->kick(t);
    return 0;
}

static void op_wake(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    uint32_t how = 0;
    if (req->len == sizeof how) memcpy(&how, req->data, sizeof how);
    lkd_task *t = lkd_task_lookup(req->target_tid);
    if (!t || t->proc->pid != req->target_pid || t->proc->host_pid != getpid()) {
        if (t) lkd_task_put(t);
        resp->status = -LX_ESRCH;
        return;
    }
    resp->status = wake_local(t, how);
    lkd_task_put(t);
}

int pt_wake_tracee(int32_t tgid, int32_t tid, unsigned how)
{
    pt_stats_g.wakes++;
    lkd_task *t = lkd_task_lookup(tid);
    if (t) {
        int local = t->proc->pid == tgid && t->proc->host_pid == getpid();
        int r = local ? wake_local(t, how) : 0;
        lkd_task_put(t);
        if (local) return r;
    }
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = OP_WAKE;
    req.target_pid = tgid;
    req.target_tid = tid;
    req.data = &how;
    req.len = sizeof how;
    int r = lkd_famsvc_call_pid(tgid, &req, &resp, 2000);
    lkd_famsvc_msg_free(&resp);
    return r;
}

/* ---- notification of a tracer --------------------------------------------------------------------- */

struct notify_msg { int32_t tracer_pid, tid; uint32_t uid; int32_t code, status; };

static void notify_local(const struct notify_msg *m)
{

    /* do_notify_parent_cldstop(tsk, true, CLD_TRAPPED) or the tracee's death: SIGCHLD to the tracer. */
    if (m->code) (void)lkd_sig_child_event(m->tracer_pid, m->tid, m->uid, m->code, m->status);
    lkd_task_wake_waiters(m->tracer_pid);
}

static void op_notify(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    if (req->len != sizeof(struct notify_msg)) { resp->status = -LX_EPROTO; return; }
    struct notify_msg m;
    memcpy(&m, req->data, sizeof m);
    notify_local(&m);
    resp->status = 0;
}

void pt_notify_tracer(int32_t tracer_pid, int32_t tracee_tid, uint32_t uid, int cld_code, int cld_status)
{
    pt_stats_g.notifies++;
    struct notify_msg m = { tracer_pid, tracee_tid, uid, cld_code, cld_status };
    lkd_proc_info *pi = malloc(sizeof *pi);
    int32_t host = 0;
    if (pi && lkd_proc_lookup(tracer_pid, pi) == 0) host = pi->host_pid;
    free(pi);
    if (host == 0 || host == getpid()) {
        notify_local(&m);
        if (host == 0) lkd_task_wake_waiters(tracer_pid); /* still bump the family counter */
        return;
    }
    lkd_task_wake_waiters(tracer_pid); /* family change counter: a waiter polling it rescans */
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = OP_NOTIFY;
    req.target_pid = tracer_pid;
    req.data = &m;
    req.len = sizeof m;
    (void)lkd_famsvc_call(host, &req, &resp, 2000);
    lkd_famsvc_msg_free(&resp);
}

/* ---- init and fork --------------------------------------------------------------------------------- */

void pt_transport_init(void)
{
    int r = pt_table_attach();
    if (r < 0) lkd_log("ptrace: cannot map the family ptrace table (%d): ptrace requests will fail", r);
    lkd_famsvc_register(OP_WAKE, op_wake);
    lkd_famsvc_register(OP_NOTIFY, op_notify);
}

void pt_transport_fork_prepare(void) { pthread_mutex_lock(&g_mu); }
void pt_transport_fork_parent(void) { pthread_mutex_unlock(&g_mu); }
/* The forking thread is the child's only thread and still owns the mutex. fcntl locks are not
 * inherited by fork (none is held across it anyway). */
void pt_transport_fork_child(void)
{
    lkd_fork_reinit_mutex(&g_mu, 0);
    lkd_fork_reinit_mutex(&g_attach_mu, 0); /* may be held by another thread of the parent at fork */
}

void pt_get_stats(struct pt_stats *out) { *out = pt_stats_g; }

/* PTRACE_INTERRUPT needs lkd_signal_next() to stop a task that has LKD_TF_TRACE_STOP (request ptrace-1).
 * Until sig says so, setting the flag would only make every blocking wait of the tracee return at once. */
LKD_WEAK int lkd_sig_trace_stop_supported(void) { return 0; }
