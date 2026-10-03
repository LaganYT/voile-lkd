/* fdtable.c: open file description objects and the per-process descriptor table (fd.h).
 *
 * The table grows on demand (doubling) up to RLIMIT_NOFILE of the process (rlim_cur), with a hard
 * ceiling of FS_FD_HARD_MAX like Linux fs.nr_open. Descriptor flags (FD_CLOEXEC) live in the entry and
 * are mirrored on the host fd. Objects are shared by dup and CLONE_FILES; a fork child inherits the host
 * fds and gets its own copy of the objects (fd.h model).
 */
#include <fcntl.h>
#include <unistd.h>

#include "fs_priv.h"
#include "net.h"

struct fdent { lkd_fdobj *obj; uint8_t cloexec; };

struct lkd_fdtable {
    _Atomic int refs;
    pthread_mutex_t lock;       /* LKD_RANK_FDTAB */
    struct fdent *e;
    int cap;
    struct lkd_fdtable *next, *prev; /* registry of live tables (fork hooks) */
};

static pthread_mutex_t g_tables_lock = PTHREAD_MUTEX_INITIALIZER;
static struct lkd_fdtable *g_tables;
static _Atomic uint64_t g_obj_ino = 1;

/* ---- Objects ----------------------------------------------------------------------------------- */

lkd_fdobj *lkd_fdobj_new(lkd_fd_kind kind, const lkd_fd_ops *ops, int host_fd, uint32_t status_flags, void *priv)
{
    lkd_fdobj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    atomic_init(&o->refs, 1);
    o->kind = kind;
    o->ops = ops;
    o->host_fd = host_fd;
    o->notify_fd = -1;
    atomic_init(&o->status_flags, status_flags);
    pthread_mutex_init(&o->lock, NULL);
    o->priv = priv;
    o->ino = atomic_fetch_add(&g_obj_ino, 1);
    return o;
}

lkd_fdobj *lkd_fdobj_ref(lkd_fdobj *o)
{
    atomic_fetch_add(&o->refs, 1);
    return o;
}

void lkd_fdobj_put(lkd_fdobj *o)
{
    if (!o || atomic_fetch_sub(&o->refs, 1) != 1) return;
    lkd_net_object_released(o);
    fs_side_forget(o);
    if (o->ops && o->ops->release) o->ops->release(o);
    if (o->host_fd >= 0) close(o->host_fd);
    if (o->notify_fd >= 0) close(o->notify_fd);
    pthread_mutex_destroy(&o->lock);
    free(o->gpath);
    free(o);
}

/* ---- Table ------------------------------------------------------------------------------------- */

static void reg_add(lkd_fdtable *tab)
{
    pthread_mutex_lock(&g_tables_lock);
    tab->next = g_tables;
    tab->prev = NULL;
    if (g_tables) g_tables->prev = tab;
    g_tables = tab;
    pthread_mutex_unlock(&g_tables_lock);
}

static void reg_del(lkd_fdtable *tab)
{
    pthread_mutex_lock(&g_tables_lock);
    if (tab->prev) tab->prev->next = tab->next; else g_tables = tab->next;
    if (tab->next) tab->next->prev = tab->prev;
    pthread_mutex_unlock(&g_tables_lock);
}

lkd_fdtable *lkd_fdtable_new(void)
{
    lkd_fdtable *tab = calloc(1, sizeof *tab);
    if (!tab) return NULL;
    atomic_init(&tab->refs, 1);
    pthread_mutex_init(&tab->lock, NULL);
    tab->cap = 64;
    tab->e = calloc((size_t)tab->cap, sizeof *tab->e);
    if (!tab->e) { free(tab); return NULL; }
    reg_add(tab);
    return tab;
}

lkd_fdtable *lkd_fdtable_clone(lkd_fdtable *parent, uint64_t clone_flags)
{
    if (!parent) return lkd_fdtable_new();
    if (clone_flags & LX_CLONE_FILES) { atomic_fetch_add(&parent->refs, 1); return parent; }
    lkd_fdtable *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    atomic_init(&c->refs, 1);
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_lock(&parent->lock);
    c->cap = parent->cap;
    c->e = calloc((size_t)c->cap, sizeof *c->e);
    if (c->e)
        for (int i = 0; i < parent->cap; i++)
            if (parent->e[i].obj) { c->e[i].obj = lkd_fdobj_ref(parent->e[i].obj); c->e[i].cloexec = parent->e[i].cloexec; }
    pthread_mutex_unlock(&parent->lock);
    if (!c->e) { free(c); return NULL; }
    reg_add(c);
    return c;
}

void lkd_fdtable_put(lkd_fdtable *tab)
{
    if (!tab || atomic_fetch_sub(&tab->refs, 1) != 1) return;
    reg_del(tab);
    for (int i = 0; i < tab->cap; i++) lkd_fdobj_put(tab->e[i].obj);
    pthread_mutex_destroy(&tab->lock);
    free(tab->e);
    free(tab);
}

/* RLIMIT_NOFILE soft limit of the process, capped at FS_FD_HARD_MAX. */
int fs_fd_limit(lkd_task *t)
{
    uint64_t cur = FS_FD_HARD_MAX;
    if (t && t->proc) {
        pthread_mutex_lock(&t->proc->lock);
        cur = t->proc->rlimits[LX_RLIMIT_NOFILE].rlim_cur;
        pthread_mutex_unlock(&t->proc->lock);
    }
    return cur > FS_FD_HARD_MAX ? FS_FD_HARD_MAX : (int)cur;
}

static int grow(lkd_fdtable *tab, int need)
{
    if (need < tab->cap) return 0;
    if (need >= FS_FD_HARD_MAX) return -LX_EMFILE;
    int ncap = tab->cap;
    while (ncap <= need) ncap *= 2;
    if (ncap > FS_FD_HARD_MAX) ncap = FS_FD_HARD_MAX;
    struct fdent *ne = realloc(tab->e, (size_t)ncap * sizeof *ne);
    if (!ne) return -LX_ENOMEM;
    memset(ne + tab->cap, 0, (size_t)(ncap - tab->cap) * sizeof *ne);
    tab->e = ne;
    tab->cap = ncap;
    return 0;
}

static void mirror_cloexec(lkd_fdobj *o, int cloexec)
{
    if (o->host_fd >= 0) fcntl(o->host_fd, F_SETFD, cloexec ? FD_CLOEXEC : 0);
}

/* Linux alloc_fd (fs/file.c): lowest free >= min_fd below the limit, else EMFILE. */
int lkd_fd_alloc(lkd_task *t, lkd_fdobj *o, int min_fd, int cloexec)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    if (!tab || !o) return -LX_EBADF;
    if (min_fd < 0) return -LX_EINVAL;
    int limit = fs_fd_limit(t);
    if (min_fd >= limit) return -LX_EMFILE;
    pthread_mutex_lock(&tab->lock);
    int fd = min_fd;
    for (; fd < limit; fd++) {
        if (fd >= tab->cap) {
            int rc = grow(tab, fd);
            if (rc < 0) { pthread_mutex_unlock(&tab->lock); return rc; }
        }
        if (!tab->e[fd].obj) break;
    }
    if (fd >= limit) { pthread_mutex_unlock(&tab->lock); return -LX_EMFILE; }
    tab->e[fd].obj = lkd_fdobj_ref(o);
    tab->e[fd].cloexec = cloexec ? 1 : 0;
    pthread_mutex_unlock(&tab->lock);
    mirror_cloexec(o, cloexec);
    return fd;
}

/* dup2 target: fd must be below RLIMIT_NOFILE (-EBADF otherwise, fs/file.c do_dup2 / ksys_dup3). */
int lkd_fd_install_at(lkd_task *t, int fd, lkd_fdobj *o, int cloexec)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    if (!tab || fd < 0 || fd >= fs_fd_limit(t)) return -LX_EBADF;
    pthread_mutex_lock(&tab->lock);
    int rc = grow(tab, fd);
    if (rc < 0) { pthread_mutex_unlock(&tab->lock); return rc; }
    lkd_fdobj *old = tab->e[fd].obj;
    tab->e[fd].obj = lkd_fdobj_ref(o);
    tab->e[fd].cloexec = cloexec ? 1 : 0;
    pthread_mutex_unlock(&tab->lock);
    mirror_cloexec(o, cloexec);
    lkd_fdobj_put(old);
    return fd;
}

lkd_fdobj *lkd_fd_get(lkd_task *t, int fd, int *cloexec_out)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    lkd_fdobj *o = NULL;
    if (!tab || fd < 0) return NULL;
    pthread_mutex_lock(&tab->lock);
    if (fd < tab->cap && tab->e[fd].obj) {
        o = lkd_fdobj_ref(tab->e[fd].obj);
        if (cloexec_out) *cloexec_out = tab->e[fd].cloexec;
    }
    pthread_mutex_unlock(&tab->lock);
    return o;
}

int fs_fd_get_cloexec(lkd_task *t, int fd)
{
    int c = 0;
    lkd_fdobj *o = lkd_fd_get(t, fd, &c);
    if (!o) return -LX_EBADF;
    lkd_fdobj_put(o);
    return c;
}

int lkd_fd_close(lkd_task *t, int fd)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    if (!tab || fd < 0) return -LX_EBADF;
    pthread_mutex_lock(&tab->lock);
    lkd_fdobj *o = fd < tab->cap ? tab->e[fd].obj : NULL;
    if (o) { tab->e[fd].obj = NULL; tab->e[fd].cloexec = 0; }
    pthread_mutex_unlock(&tab->lock);
    if (!o) return -LX_EBADF;
    lkd_fdobj_put(o);
    return 0;
}

int lkd_fd_dup(lkd_task *t, int oldfd, int newfd, int min_fd, int cloexec)
{
    lkd_fdobj *o = lkd_fd_get(t, oldfd, NULL);
    if (!o) return -LX_EBADF;
    int r = newfd >= 0 ? lkd_fd_install_at(t, newfd, o, cloexec) : lkd_fd_alloc(t, o, min_fd, cloexec);
    lkd_fdobj_put(o);
    return r;
}

int lkd_fd_set_cloexec(lkd_task *t, int fd, int cloexec)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    int r = -LX_EBADF;
    lkd_fdobj *o = NULL;
    if (!tab || fd < 0) return r;
    pthread_mutex_lock(&tab->lock);
    if (fd < tab->cap && tab->e[fd].obj) {
        tab->e[fd].cloexec = cloexec ? 1 : 0;
        o = lkd_fdobj_ref(tab->e[fd].obj);
        r = 0;
    }
    pthread_mutex_unlock(&tab->lock);
    if (o) { mirror_cloexec(o, cloexec); lkd_fdobj_put(o); }
    return r;
}

void lkd_fd_close_on_exec(lkd_task *t)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    if (!tab) return;
    (void)fs_fdtable_close_range(t, 0, ~0u, 0 /* close */ | 2 /* only cloexec */);
}

/* close_range(2) core: close (or mark CLOEXEC when cloexec_only == 1) every fd in [first, last].
 * Mode 2: close only descriptors that have FD_CLOEXEC (exec). */
int fs_fdtable_close_range(lkd_task *t, unsigned first, unsigned last, int mode)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    if (!tab) return -LX_EBADF;
    for (;;) {
        lkd_fdobj *batch[64];
        int nb = 0;
        pthread_mutex_lock(&tab->lock);
        unsigned end = last < (unsigned)tab->cap ? last : (unsigned)tab->cap - 1;
        unsigned i = first;
        for (; i <= end && (int)i < tab->cap && nb < 64; i++) {
            if (!tab->e[i].obj) continue;
            if (mode == 1) { tab->e[i].cloexec = 1; mirror_cloexec(tab->e[i].obj, 1); continue; }
            if (mode == 2 && !tab->e[i].cloexec) continue;
            batch[nb++] = tab->e[i].obj;
            tab->e[i].obj = NULL;
            tab->e[i].cloexec = 0;
        }
        pthread_mutex_unlock(&tab->lock);
        for (int k = 0; k < nb; k++) lkd_fdobj_put(batch[k]);
        if (nb < 64 || i > end) break;
        first = i;
    }
    return 0;
}

int lkd_fd_count(lkd_task *t)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    int n = 0;
    if (!tab) return 0;
    pthread_mutex_lock(&tab->lock);
    for (int i = 0; i < tab->cap; i++) n += tab->e[i].obj != NULL;
    pthread_mutex_unlock(&tab->lock);
    return n;
}

int lkd_fd_list(lkd_task *t, int *fds, int max)
{
    lkd_fdtable *tab = t ? t->fdt : NULL;
    int n = 0;
    if (!tab) return 0;
    pthread_mutex_lock(&tab->lock);
    for (int i = 0; i < tab->cap && n < max; i++) if (tab->e[i].obj) fds[n++] = i;
    pthread_mutex_unlock(&tab->lock);
    return n;
}

/* ---- Fork -------------------------------------------------------------------------------------- */

void fs_fdtables_fork_prepare(void)
{
    pthread_mutex_lock(&g_tables_lock);
    for (lkd_fdtable *tab = g_tables; tab; tab = tab->next) pthread_mutex_lock(&tab->lock);
}

void fs_fdtables_fork_parent(void)
{
    for (lkd_fdtable *tab = g_tables; tab; tab = tab->next) pthread_mutex_unlock(&tab->lock);
    pthread_mutex_unlock(&g_tables_lock);
}

/* Child: only the forking thread exists. Re-initialize every table lock and the lock of every object
 * reachable from a table (a lock held by a thread that no longer exists would stay held forever). */
void fs_fdtables_fork_child(void)
{
    lkd_fork_reinit_mutex(&g_tables_lock, 0);
    for (lkd_fdtable *tab = g_tables; tab; tab = tab->next) {
        lkd_fork_reinit_mutex(&tab->lock, 0);
        for (int i = 0; i < tab->cap; i++)
            if (tab->e[i].obj) lkd_fork_reinit_mutex(&tab->e[i].obj->lock, 0);
    }
}
