/* stubs_fs.c: weak placeholders for the fs module (fd table, fd objects, vfs). Owner: core until fs
 * overrides them. The table and objects work (simple array, mutex) so that other modules can unit-test
 * with fake descriptors; the vfs returns -ENOSYS. */
#include <fcntl.h>
#include <unistd.h>

#include "core_priv.h"
#include "fd.h"
#include "net.h"
#include "task.h"
#include "vfs.h"

struct stub_entry { lkd_fdobj *obj; int cloexec; };
struct lkd_fdtable {
    _Atomic int refs;
    pthread_mutex_t lock;
    struct stub_entry *e;
    int cap;
};
#define STUB_FD_MAX 65536

LKD_WEAK lkd_fdobj *lkd_fdobj_new(lkd_fd_kind kind, const lkd_fd_ops *ops, int host_fd, uint32_t status_flags, void *priv)
{
    lkd_fdobj *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    atomic_init(&o->refs, 1);
    o->kind = kind; o->ops = ops; o->host_fd = host_fd; o->notify_fd = -1;
    atomic_init(&o->status_flags, status_flags);
    pthread_mutex_init(&o->lock, NULL);
    o->priv = priv;
    return o;
}
LKD_WEAK lkd_fdobj *lkd_fdobj_ref(lkd_fdobj *o) { atomic_fetch_add(&o->refs, 1); return o; }
LKD_WEAK void lkd_fdobj_put(lkd_fdobj *o)
{
    if (!o || atomic_fetch_sub(&o->refs, 1) != 1) return;
    lkd_net_object_released(o);
    if (o->ops && o->ops->release) o->ops->release(o);
    if (o->host_fd >= 0) close(o->host_fd);
    if (o->notify_fd >= 0) close(o->notify_fd);
    pthread_mutex_destroy(&o->lock);
    free(o->gpath);
    free(o);
}
LKD_WEAK int lkd_fdobj_reopen(lkd_task *t, lkd_fdobj *o, uint32_t lx_flags, lkd_fdobj **out)
{
    (void)t; (void)o; (void)lx_flags; (void)out;
    return -LX_ENOSYS;
}

LKD_WEAK lkd_fdtable *lkd_fdtable_new(void)
{
    lkd_fdtable *tab = calloc(1, sizeof *tab);
    if (!tab) return NULL;
    atomic_init(&tab->refs, 1);
    pthread_mutex_init(&tab->lock, NULL);
    tab->cap = 64;
    tab->e = calloc((size_t)tab->cap, sizeof *tab->e);
    if (!tab->e) { free(tab); return NULL; }
    return tab;
}
LKD_WEAK lkd_fdtable *lkd_fdtable_clone(lkd_fdtable *parent, uint64_t clone_flags)
{
    if (!parent) return NULL;
    if (clone_flags & LX_CLONE_FILES) { atomic_fetch_add(&parent->refs, 1); return parent; }
    lkd_fdtable *c = lkd_fdtable_new();
    if (!c) return NULL;
    pthread_mutex_lock(&parent->lock);
    free(c->e);
    c->cap = parent->cap;
    c->e = calloc((size_t)c->cap, sizeof *c->e);
    for (int i = 0; i < parent->cap; i++)
        if (parent->e[i].obj) { c->e[i].obj = lkd_fdobj_ref(parent->e[i].obj); c->e[i].cloexec = parent->e[i].cloexec; }
    pthread_mutex_unlock(&parent->lock);
    return c;
}
LKD_WEAK void lkd_fdtable_put(lkd_fdtable *tab)
{
    if (!tab || atomic_fetch_sub(&tab->refs, 1) != 1) return;
    for (int i = 0; i < tab->cap; i++) lkd_fdobj_put(tab->e[i].obj);
    pthread_mutex_destroy(&tab->lock);
    free(tab->e);
    free(tab);
}

static int grow(lkd_fdtable *tab, int need)
{
    if (need < tab->cap) return 0;
    if (need >= STUB_FD_MAX) return -1;
    int ncap = tab->cap;
    while (ncap <= need) ncap *= 2;
    struct stub_entry *ne = realloc(tab->e, (size_t)ncap * sizeof *ne);
    if (!ne) return -1;
    memset(ne + tab->cap, 0, (size_t)(ncap - tab->cap) * sizeof *ne);
    tab->e = ne;
    tab->cap = ncap;
    return 0;
}

LKD_WEAK int lkd_fd_alloc(lkd_task *t, lkd_fdobj *o, int min_fd, int cloexec)
{
    lkd_fdtable *tab = t->fdt;
    if (!tab || min_fd < 0) return -LX_EINVAL;
    pthread_mutex_lock(&tab->lock);
    int fd = min_fd;
    for (;; fd++) {
        if (grow(tab, fd) < 0) { pthread_mutex_unlock(&tab->lock); return -LX_EMFILE; }
        if (!tab->e[fd].obj) break;
    }
    tab->e[fd].obj = lkd_fdobj_ref(o);
    tab->e[fd].cloexec = cloexec;
    pthread_mutex_unlock(&tab->lock);
    return fd;
}
LKD_WEAK int lkd_fd_install_at(lkd_task *t, int fd, lkd_fdobj *o, int cloexec)
{
    lkd_fdtable *tab = t->fdt;
    if (!tab || fd < 0) return -LX_EBADF;
    pthread_mutex_lock(&tab->lock);
    if (grow(tab, fd) < 0) { pthread_mutex_unlock(&tab->lock); return -LX_EMFILE; }
    lkd_fdobj *old = tab->e[fd].obj;
    tab->e[fd].obj = lkd_fdobj_ref(o);
    tab->e[fd].cloexec = cloexec;
    pthread_mutex_unlock(&tab->lock);
    lkd_fdobj_put(old);
    return fd;
}
LKD_WEAK lkd_fdobj *lkd_fd_get(lkd_task *t, int fd, int *cloexec_out)
{
    lkd_fdtable *tab = t->fdt;
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
LKD_WEAK int lkd_fd_close(lkd_task *t, int fd)
{
    lkd_fdtable *tab = t->fdt;
    if (!tab || fd < 0) return -LX_EBADF;
    pthread_mutex_lock(&tab->lock);
    lkd_fdobj *o = fd < tab->cap ? tab->e[fd].obj : NULL;
    if (o) tab->e[fd].obj = NULL;
    pthread_mutex_unlock(&tab->lock);
    if (!o) return -LX_EBADF;
    lkd_fdobj_put(o);
    return 0;
}
LKD_WEAK int lkd_fd_dup(lkd_task *t, int oldfd, int newfd, int min_fd, int cloexec)
{
    lkd_fdobj *o = lkd_fd_get(t, oldfd, NULL);
    if (!o) return -LX_EBADF;
    int r = newfd >= 0 ? lkd_fd_install_at(t, newfd, o, cloexec) : lkd_fd_alloc(t, o, min_fd, cloexec);
    lkd_fdobj_put(o);
    return r;
}
LKD_WEAK int lkd_fd_set_cloexec(lkd_task *t, int fd, int cloexec)
{
    lkd_fdtable *tab = t->fdt;
    int r = -LX_EBADF;
    if (!tab || fd < 0) return r;
    pthread_mutex_lock(&tab->lock);
    if (fd < tab->cap && tab->e[fd].obj) { tab->e[fd].cloexec = cloexec; r = 0; }
    pthread_mutex_unlock(&tab->lock);
    return r;
}
LKD_WEAK void lkd_fd_close_on_exec(lkd_task *t)
{
    lkd_fdtable *tab = t->fdt;
    if (!tab) return;
    for (int i = 0; i < tab->cap; i++) {
        lkd_fdobj *o = NULL;
        pthread_mutex_lock(&tab->lock);
        if (tab->e[i].obj && tab->e[i].cloexec) { o = tab->e[i].obj; tab->e[i].obj = NULL; }
        pthread_mutex_unlock(&tab->lock);
        lkd_fdobj_put(o);
    }
}
LKD_WEAK int lkd_fd_count(lkd_task *t)
{
    lkd_fdtable *tab = t->fdt;
    int n = 0;
    if (!tab) return 0;
    pthread_mutex_lock(&tab->lock);
    for (int i = 0; i < tab->cap; i++) n += tab->e[i].obj != NULL;
    pthread_mutex_unlock(&tab->lock);
    return n;
}
LKD_WEAK int lkd_fd_list(lkd_task *t, int *fds, int max)
{
    lkd_fdtable *tab = t->fdt;
    int n = 0;
    if (!tab) return 0;
    pthread_mutex_lock(&tab->lock);
    for (int i = 0; i < tab->cap && n < max; i++) if (tab->e[i].obj) fds[n++] = i;
    pthread_mutex_unlock(&tab->lock);
    return n;
}

/* vfs: not implemented in the placeholder. */
LKD_WEAK lkd_fsctx *lkd_fsctx_new(void) { return NULL; }
LKD_WEAK lkd_fsctx *lkd_fsctx_clone(lkd_fsctx *parent, uint64_t clone_flags) { (void)clone_flags; return parent; }
LKD_WEAK void lkd_fsctx_put(lkd_fsctx *ctx) { (void)ctx; }
LKD_WEAK int lkd_vfs_getcwd(lkd_task *t, char *out, size_t cap)
{
    (void)t;
    if (cap < 2) return -LX_ERANGE;
    memcpy(out, "/", 2);
    return 1;
}
LKD_WEAK uint32_t lkd_vfs_umask(lkd_task *t) { (void)t; return 022; }
LKD_WEAK int lkd_vfs_open(lkd_task *t, int dirfd, const char *p, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{ (void)t; (void)dirfd; (void)p; (void)fl; (void)mode; (void)out; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_stat(lkd_task *t, int dirfd, const char *p, int fl, struct lx_stat *out)
{ (void)t; (void)dirfd; (void)p; (void)fl; (void)out; return -LX_ENOSYS; }
LKD_WEAK int64_t lkd_vfs_readlink(lkd_task *t, int dirfd, const char *p, char *buf, size_t cap)
{ (void)t; (void)dirfd; (void)p; (void)buf; (void)cap; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_realpath(lkd_task *t, int dirfd, const char *p, char *out, size_t cap)
{ (void)t; (void)dirfd; (void)p; (void)out; (void)cap; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_to_host(lkd_task *t, int dirfd, const char *p, int fl, char *out, size_t cap)
{ (void)t; (void)dirfd; (void)p; (void)fl; (void)out; (void)cap; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_set_root(const char *host_dir) { (void)host_dir; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_mount_bind(const char *g, const char *h, int ro) { (void)g; (void)h; (void)ro; return -LX_ENOSYS; }
LKD_WEAK int lkd_vfs_mount_virtual(const char *g, const lkd_vmount_ops *ops, void *priv)
{ (void)g; (void)ops; (void)priv; return -LX_ENOSYS; }
LKD_WEAK char *lkd_vfs_render_mounts(lkd_task *t, int mountinfo, size_t *len_out)
{ (void)t; (void)mountinfo; if (len_out) *len_out = 0; return NULL; }

/* fs service used by mm (mprotect of a sealed memfd, request fs-mm): no fs module, no memfd. */
struct lkd_fdobj;
LKD_WEAK int lkd_fs_memfd_seals(struct lkd_fdobj *o) { (void)o; return -1; }
LKD_WEAK int lkd_fs_map_phase(struct lkd_fdobj *o, int64_t want) { (void)o; return want <= 0 ? 0 : -LX_EINVAL; }
LKD_WEAK int lkd_fs_pin_raw(struct lkd_fdobj *o) { (void)o; return 0; }
