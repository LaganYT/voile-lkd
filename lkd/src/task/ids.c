/* ids.c: process identity syscalls. Rules follow kernel/sys.c (v6.12) for a task without capabilities:
 * the guest runs as the configured non-root user (lkd_config uid/gid, default 1000), so every
 * privileged change answers -EPERM as Linux does for such a user. Process groups and sessions live in
 * the family registry, so a parent's setpgid on its child is seen by the child. */
#include "ptrace.h"
#include "task_priv.h"

#define UID_INVALID 0xffffffffu

LKD_SYSCALL(getpid) { return t->proc->pid; }
LKD_SYSCALL(gettid) { return t->tid; }

LKD_SYSCALL(getppid)
{
    lkd_proc_refresh_ids(t->proc);
    return t->proc->ppid;
}

LKD_SYSCALL(getpgrp)
{
    lkd_proc_refresh_ids(t->proc);
    return t->proc->pgid;
}

static int64_t lookup_ids(lkd_task *t, int32_t pid, int want_sid)
{
    if (pid < 0) return -LX_ESRCH; /* find_task_by_vpid of a negative id finds nothing */
    if (pid == 0 || pid == t->proc->pid) {
        lkd_proc_refresh_ids(t->proc);
        return want_sid ? t->proc->sid : t->proc->pgid;
    }
    int64_t r = -LX_ESRCH;
    fam_lock();
    struct fam_proc *p = fam_proc_find(pid);
    if (p) r = want_sid ? p->info.sid : p->info.pgid;
    else {
        /* a thread id: getpgid/getsid accept any task and answer for its group */
        struct fam_tid *tr = fam_tid_find(pid);
        struct fam_proc *g = tr ? fam_proc_find(tr->info.tgid) : NULL;
        if (g) r = want_sid ? g->info.sid : g->info.pgid;
    }
    fam_unlock();
    return r;
}

LKD_SYSCALL(getpgid) { return lookup_ids(t, LKD_ARGI(0), 0); }
LKD_SYSCALL(getsid) { return lookup_ids(t, LKD_ARGI(0), 1); }

LKD_SYSCALL(setpgid)
{
    int32_t pid = LKD_ARGI(0), pgid = LKD_ARGI(1);
    int32_t me = t->proc->pid;
    if (!pid) pid = me;
    if (!pgid) pgid = pid;
    if (pgid < 0) return -LX_EINVAL;
    lkd_proc_refresh_ids(t->proc);
    int64_t err;
    fam_lock();
    struct fam_proc *p = fam_proc_find(pid);
    struct fam_proc *self = fam_proc_find(me);
    if (!p || (atomic_load(&p->flags) & RF_ZOMBIE)) {
        err = fam_tid_find(pid) ? -LX_EINVAL : -LX_ESRCH; /* a non-leader thread: EINVAL */
        goto out;
    }
    if (!self) { err = -LX_ESRCH; goto out; }
    if (p->info.ppid == me) {
        err = -LX_EPERM;
        if (p->info.sid != self->info.sid) goto out;
        err = -LX_EACCES;
        if (atomic_load(&p->flags) & RF_DID_EXEC) goto out;
    } else if (pid != me) {
        err = -LX_ESRCH;
        goto out;
    }
    err = -LX_EPERM;
    if (p->info.sid == p->info.pid) goto out; /* session leader */
    if (pgid != pid) {
        int found = 0;
        for (int i = 0; i < FAM_MAX_PROCS; i++) {
            struct fam_proc *g = fam_proc_at(i);
            if (g->info.pid && g->info.pgid == pgid && g->info.sid == self->info.sid) { found = 1; break; }
        }
        if (!found) goto out;
    }
    __atomic_store_n(&p->info.pgid, pgid, __ATOMIC_RELEASE);
    err = 0;
out:
    fam_unlock();
    lkd_proc_refresh_ids(t->proc);
    return err;
}

LKD_SYSCALL(setsid)
{
    int32_t me = t->proc->pid;
    int64_t err = -LX_EPERM;
    fam_lock();
    /* ksys_setsid(): fail if a process group with our pid already exists (we lead one). */
    int busy = 0;
    for (int i = 0; i < FAM_MAX_PROCS; i++) {
        struct fam_proc *g = fam_proc_at(i);
        if (g->info.pid && g->info.pgid == me) { busy = 1; break; }
    }
    struct fam_proc *self = fam_proc_find(me);
    if (!busy && self) {
        __atomic_store_n(&self->info.sid, me, __ATOMIC_RELEASE);
        __atomic_store_n(&self->info.pgid, me, __ATOMIC_RELEASE);
        err = me;
    }
    fam_unlock();
    lkd_proc_refresh_ids(t->proc);
    return err;
}

/* ---- uid / gid --------------------------------------------------------------------------------- */

static void publish_creds(lkd_task *t) { lkd_registry_publish(t); }

LKD_SYSCALL(getuid) { return t->proc->cred.uid; }
LKD_SYSCALL(geteuid) { return t->proc->cred.euid; }
LKD_SYSCALL(getgid) { return t->proc->cred.gid; }
LKD_SYSCALL(getegid) { return t->proc->cred.egid; }

static int64_t put3(uint64_t a, uint64_t b, uint64_t c, uint32_t x, uint32_t y, uint32_t z)
{
    if (lkd_user_write(a, &x, 4) || lkd_user_write(b, &y, 4) || lkd_user_write(c, &z, 4)) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(getresuid)
{
    struct lkd_creds *c = &t->proc->cred;
    return put3(LKD_ARGP(0), LKD_ARGP(1), LKD_ARGP(2), c->uid, c->euid, c->suid);
}

LKD_SYSCALL(getresgid)
{
    struct lkd_creds *c = &t->proc->cred;
    return put3(LKD_ARGP(0), LKD_ARGP(1), LKD_ARGP(2), c->gid, c->egid, c->sgid);
}

static int is_root(const struct lkd_creds *c) { return c->euid == 0; } /* never with the default config */

/* The uid and gid rule sets are identical; *_ids point to {real, effective, saved, fs}. */
static int64_t do_setid(lkd_task *t, uint32_t id, int gid)
{
    struct lkd_proc *p = t->proc;
    if (id == UID_INVALID) return -LX_EINVAL;
    pthread_mutex_lock(&p->lock);
    uint32_t *r = gid ? &p->cred.gid : &p->cred.uid, *e = gid ? &p->cred.egid : &p->cred.euid;
    uint32_t *s = gid ? &p->cred.sgid : &p->cred.suid, *f = gid ? &p->cred.fsgid : &p->cred.fsuid;
    int64_t err = 0;
    if (is_root(&p->cred)) { *r = *e = *s = *f = id; }
    else if (id == *r || id == *s) { *e = *f = id; }
    else err = -LX_EPERM;
    pthread_mutex_unlock(&p->lock);
    if (!err) publish_creds(t);
    return err;
}

static int64_t do_setreid(lkd_task *t, uint32_t rid, uint32_t eid, int gid)
{
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    uint32_t *r = gid ? &p->cred.gid : &p->cred.uid, *e = gid ? &p->cred.egid : &p->cred.euid;
    uint32_t *s = gid ? &p->cred.sgid : &p->cred.suid, *f = gid ? &p->cred.fsgid : &p->cred.fsuid;
    int root = is_root(&p->cred);
    uint32_t nr = *r, ne = *e, ns = *s;
    int64_t err = 0;
    if (rid != UID_INVALID) {
        if (!root && rid != *r && rid != *e) err = -LX_EPERM;
        nr = rid;
    }
    if (!err && eid != UID_INVALID) {
        if (!root && eid != *r && eid != *e && eid != *s) err = -LX_EPERM;
        ne = eid;
    }
    if (!err) {
        if (rid != UID_INVALID || (eid != UID_INVALID && eid != *r)) ns = ne;
        *r = nr; *e = ne; *s = ns; *f = ne;
    }
    pthread_mutex_unlock(&p->lock);
    if (!err) publish_creds(t);
    return err;
}

static int64_t do_setresid(lkd_task *t, uint32_t rid, uint32_t eid, uint32_t sid, int gid)
{
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    uint32_t *r = gid ? &p->cred.gid : &p->cred.uid, *e = gid ? &p->cred.egid : &p->cred.euid;
    uint32_t *s = gid ? &p->cred.sgid : &p->cred.suid, *f = gid ? &p->cred.fsgid : &p->cred.fsuid;
    int64_t err = 0;
    if (!is_root(&p->cred)) {
        uint32_t want[3] = { rid, eid, sid };
        for (int i = 0; i < 3; i++)
            if (want[i] != UID_INVALID && want[i] != *r && want[i] != *e && want[i] != *s) err = -LX_EPERM;
    }
    if (!err) {
        if (rid != UID_INVALID) *r = rid;
        if (eid != UID_INVALID) *e = eid;
        if (sid != UID_INVALID) *s = sid;
        *f = *e;
    }
    pthread_mutex_unlock(&p->lock);
    if (!err) publish_creds(t);
    return err;
}

static int64_t do_setfsid(lkd_task *t, uint32_t id, int gid)
{
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    uint32_t *r = gid ? &p->cred.gid : &p->cred.uid, *e = gid ? &p->cred.egid : &p->cred.euid;
    uint32_t *s = gid ? &p->cred.sgid : &p->cred.suid, *f = gid ? &p->cred.fsgid : &p->cred.fsuid;
    uint32_t old = *f;
    /* __sys_setfsuid(): always returns the old value; changes only to an id the task already holds. */
    if (id != UID_INVALID && (is_root(&p->cred) || id == *r || id == *e || id == *s || id == *f)) *f = id;
    pthread_mutex_unlock(&p->lock);
    return old;
}

LKD_SYSCALL(setuid) { return do_setid(t, LKD_ARGUI(0), 0); }
LKD_SYSCALL(setgid) { return do_setid(t, LKD_ARGUI(0), 1); }
LKD_SYSCALL(setreuid) { return do_setreid(t, LKD_ARGUI(0), LKD_ARGUI(1), 0); }
LKD_SYSCALL(setregid) { return do_setreid(t, LKD_ARGUI(0), LKD_ARGUI(1), 1); }
LKD_SYSCALL(setresuid) { return do_setresid(t, LKD_ARGUI(0), LKD_ARGUI(1), LKD_ARGUI(2), 0); }
LKD_SYSCALL(setresgid) { return do_setresid(t, LKD_ARGUI(0), LKD_ARGUI(1), LKD_ARGUI(2), 1); }
LKD_SYSCALL(setfsuid) { return do_setfsid(t, LKD_ARGUI(0), 0); }
LKD_SYSCALL(setfsgid) { return do_setfsid(t, LKD_ARGUI(0), 1); }

LKD_SYSCALL(getgroups)
{
    int size = LKD_ARGI(0);
    struct lkd_creds *c = &t->proc->cred;
    if (size < 0) return -LX_EINVAL;
    if (size == 0) return c->ngroups;
    if ((uint32_t)size < c->ngroups) return -LX_EINVAL;
    if (c->ngroups && lkd_user_write(LKD_ARGP(1), c->groups, c->ngroups * sizeof(uint32_t))) return -LX_EFAULT;
    return c->ngroups;
}

LKD_SYSCALL(setgroups)
{
    /* SYSCALL_DEFINE2(setgroups): may_setgroups() (CAP_SETGID) is checked first. */
    if (!is_root(&t->proc->cred)) return -LX_EPERM;
    if (LKD_ARGUI(0) > 65536) return -LX_EINVAL;
    return -LX_EPERM; /* a root guest is not a supported configuration */
}

/* ---- capabilities -------------------------------------------------------------------------------- */

#define CAP_V1 0x19980330u
#define CAP_V2 0x20071026u
#define CAP_V3 0x20080522u

static int cap_words(lkd_task *t, uint64_t hdr, struct lx_cap_header *h)
{
    if (lkd_user_read(hdr, h, sizeof *h)) return -LX_EFAULT;
    if (h->version == CAP_V1) return 1;
    if (h->version == CAP_V2 || h->version == CAP_V3) return 2;
    uint32_t v = CAP_V3; /* cap_validate_magic(): tell the caller the preferred version */
    if (lkd_user_write(hdr, &v, sizeof v)) return -LX_EFAULT;
    return -LX_EINVAL;
}

LKD_SYSCALL(capget)
{
    struct lx_cap_header h;
    int words = cap_words(t, LKD_ARGP(0), &h);
    uint64_t data = LKD_ARGP(1);
    if (!data || words < 0) return (!data && words == -LX_EINVAL) ? 0 : (words < 0 ? words : 0);
    if (h.pid < 0) return -LX_EINVAL;
    struct lkd_creds c = t->proc->cred;
    if (h.pid != 0 && h.pid != t->proc->pid && h.pid != t->tid) {
        lkd_proc_info pi;
        lkd_tid_info ti;
        if (lkd_proc_lookup(h.pid, &pi) < 0 && lkd_tid_lookup(h.pid, &ti) < 0) return -LX_ESRCH;
        /* every task of the family runs with the same (empty) capability sets */
    }
    struct lx_cap_data d[2];
    memset(d, 0, sizeof d);
    for (int i = 0; i < 2; i++) {
        d[i].effective = (uint32_t)(c.cap_eff >> (32 * i));
        d[i].permitted = (uint32_t)(c.cap_perm >> (32 * i));
        d[i].inheritable = (uint32_t)(c.cap_inh >> (32 * i));
    }
    if (lkd_user_write(data, d, (size_t)words * sizeof d[0])) return -LX_EFAULT;
    return 0;
}

LKD_SYSCALL(capset)
{
    struct lx_cap_header h;
    int words = cap_words(t, LKD_ARGP(0), &h);
    if (words < 0) return words;
    if (h.pid != 0 && h.pid != t->tid) return -LX_EPERM; /* may only affect current now */
    struct lx_cap_data d[2];
    memset(d, 0, sizeof d);
    if (lkd_user_read(LKD_ARGP(1), d, (size_t)words * sizeof d[0])) return -LX_EFAULT;
    uint64_t eff = 0, perm = 0, inh = 0;
    for (int i = 0; i < words; i++) {
        eff |= (uint64_t)d[i].effective << (32 * i);
        perm |= (uint64_t)d[i].permitted << (32 * i);
        inh |= (uint64_t)d[i].inheritable << (32 * i);
    }
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    struct lkd_creds *c = &p->cred;
    int64_t err = 0;
    /* cap_capset(): no new permitted bits, effective within permitted, inheritable within old
     * inheritable | permitted (no CAP_SETPCAP) and within bounding | old inheritable. */
    if ((perm & ~c->cap_perm) || (eff & ~perm) || (inh & ~(c->cap_inh | c->cap_perm)) ||
        (inh & ~(c->cap_bset | c->cap_inh)))
        err = -LX_EPERM;
    else { c->cap_eff = eff; c->cap_perm = perm; c->cap_inh = inh; c->cap_amb &= perm & inh; }
    pthread_mutex_unlock(&p->lock);
    return err;
}

LKD_SYSCALL(personality)
{
    uint32_t want = LKD_ARGUI(0);
    struct lkd_proc *p = t->proc;
    pthread_mutex_lock(&p->lock);
    uint32_t old = p->personality;
    if (want != 0xffffffffu) p->personality = want;
    pthread_mutex_unlock(&p->lock);
    return old;
}

void task_ids_register(void)
{
    LKD_REGISTER(getpid); LKD_REGISTER(gettid); LKD_REGISTER(getppid); LKD_REGISTER(getpgrp);
    LKD_REGISTER(getpgid); LKD_REGISTER(getsid); LKD_REGISTER(setpgid); LKD_REGISTER(setsid);
    LKD_REGISTER(getuid); LKD_REGISTER(geteuid); LKD_REGISTER(getgid); LKD_REGISTER(getegid);
    LKD_REGISTER(getresuid); LKD_REGISTER(getresgid); LKD_REGISTER(setuid); LKD_REGISTER(setgid);
    LKD_REGISTER(setreuid); LKD_REGISTER(setregid); LKD_REGISTER(setresuid); LKD_REGISTER(setresgid);
    LKD_REGISTER(setfsuid); LKD_REGISTER(setfsgid); LKD_REGISTER(getgroups); LKD_REGISTER(setgroups);
    LKD_REGISTER(capget); LKD_REGISTER(capset); LKD_REGISTER(personality);
}
