/* epoll.c: epoll over kqueue. Models fs/eventpoll.c of Linux 6.12 (do_epoll_create, do_epoll_ctl,
 * do_epoll_wait, ep_item_poll, ep_send_events, ep_insert/ep_modify, EPOLLEXCLUSIVE checks).
 *
 * Design. Each epoll object owns one kqueue. A watched object is registered with its wait source (the
 * host descriptor, or notify_fd for objects without kernel-visible readiness): EVFILT_READ when the
 * mask asks for input, EVFILT_WRITE when it asks for output. kqueue is only the wake-up and the
 * candidate list: the events reported to the guest are always recomputed from the object (ops->poll or
 * the host poll translated to Linux bits, net_obj_revents), masked with the registered events plus
 * EPOLLERR|EPOLLHUP, exactly as ep_item_poll does. Level triggered is the default (the knote stays
 * active while the condition holds); EPOLLET adds EV_CLEAR; EPOLLONESHOT disables the item after one
 * report (EPOLLET and EPOLLONESHOT items are reported only once per wake-up edge).
 *
 * Items are weak: Linux removes a watch when the last reference to the file goes, so
 * lkd_net_object_released() drops them. Items are found through udata = (generation, slot) so a stale
 * kevent for a removed item is ignored.
 *
 * Documented subset (docs/status/net.md): EPOLLEXCLUSIVE is validated like Linux and then treated as a
 * normal registration (every set wakes); EPOLLWAKEUP is ignored (needs CAP_BLOCK_SUSPEND, Linux clears
 * it silently); regular files, directories and memfds refuse EPOLL_CTL_ADD with -EPERM like Linux; a
 * watched object whose wanted events cannot be signalled by its wait source (EPOLLOUT on a notify-only
 * object) is polled at every epoll_wait entry. kqueue descriptors are not inherited by fork: the child
 * rebuilds every set from its table (lkd_net_fork_child); the set then diverges from the parent's copy
 * (Linux shares it).
 */
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

#include "net_priv.h"

#define EP_MAX_NESTS 4                       /* EP_MAX_NESTS */
#define EP_LEVEL_MASK (LX_EPOLLERR | LX_EPOLLHUP)
#define EP_MAX_EVENTS (INT32_MAX / 12)
#define EP_KEV_BATCH 64
#define EP_ALLOWED_MASK (LX_EPOLLIN | LX_EPOLLPRI | LX_EPOLLOUT | LX_EPOLLERR | LX_EPOLLHUP | LX_EPOLLRDNORM | \
                         LX_EPOLLRDBAND | LX_EPOLLWRNORM | LX_EPOLLWRBAND | LX_EPOLLMSG | LX_EPOLLRDHUP | \
                         LX_EPOLLEXCLUSIVE | LX_EPOLLWAKEUP | LX_EPOLLONESHOT | LX_EPOLLET)

struct ep_item {
    int fd;                                  /* guest descriptor number at registration */
    lkd_fdobj *obj;                          /* weak: removed by lkd_net_object_released */
    uint32_t events;                         /* as registered, with ERR|HUP added */
    uint64_t data;
    uint32_t gen;                            /* slot generation, 0 = free slot */
    int regs;                                /* bit 0: READ registered, bit 1: WRITE registered, bit 2: READ is edge-triggered */
    int hfd;                                 /* wait source registered with the kqueue, -1 if none */
    int disabled;                            /* EPOLLONESHOT fired */
    int poll_only;                           /* must be scanned at epoll_wait entry */
    struct ep_item *hnext;                   /* hash chain by fd */
};

struct ep {
    struct ep *next, *prev;                  /* global list, net_lock */
    lkd_fdobj *self;                         /* weak */
    struct ep_item **slots;                  /* indexed by slot number */
    uint32_t nslots, cap;
    uint32_t next_gen;
    struct ep_item *hash[256];
    uint32_t *freelist;
    uint32_t nfree;
    uint32_t count;
    struct net_kqobj kq;
};

static struct ep *g_eps;                     /* all epoll sets of this process, net_lock */

static int ep_kq(lkd_fdobj *o) { return o->host_fd; }
static struct ep *EP(lkd_fdobj *o) { return o && o->ops == &net_epoll_ops ? o->priv : NULL; }

static uint64_t udata_of(uint32_t slot, uint32_t gen) { return ((uint64_t)gen << 32) | slot; }

/* ---- table (net_lock held) -------------------------------------------------------------------- */

static struct ep_item *ep_find(struct ep *ep, int fd, lkd_fdobj *obj)
{
    for (struct ep_item *it = ep->hash[(unsigned)fd & 255]; it; it = it->hnext)
        if (it->fd == fd && it->obj == obj) return it;
    return NULL;
}

static int slot_of(struct ep *ep, struct ep_item *it)
{
    for (uint32_t i = 0; i < ep->nslots; i++) if (ep->slots[i] == it) return (int)i;
    return -1;
}

static struct ep_item *ep_alloc(struct ep *ep, uint32_t *slot)
{
    struct ep_item *it = calloc(1, sizeof *it);
    if (!it) return NULL;
    if (ep->nfree) {
        *slot = ep->freelist[--ep->nfree];
    } else {
        if (ep->nslots == ep->cap) {
            uint32_t nc = ep->cap ? ep->cap * 2 : 16;
            struct ep_item **ns = realloc(ep->slots, nc * sizeof *ns);
            uint32_t *nf = realloc(ep->freelist, nc * sizeof *nf);
            if (!ns || !nf) { free(it); if (ns) ep->slots = ns; if (nf) ep->freelist = nf; return NULL; }
            ep->slots = ns;
            ep->freelist = nf;
            ep->cap = nc;
        }
        *slot = ep->nslots++;
    }
    it->gen = ++ep->next_gen ? ep->next_gen : (ep->next_gen = 1);
    ep->slots[*slot] = it;
    ep->count++;
    return it;
}

/* Remove an item from the table and the kqueue. */
static void ep_free_item(struct ep *ep, lkd_fdobj *self, struct ep_item *it)
{
    int kq = self ? ep_kq(self) : -1;
    if (kq >= 0 && it->hfd >= 0) {
        struct kevent kev[2];
        int n = 0;
        if (it->regs & 1) EV_SET(&kev[n++], it->hfd, EVFILT_READ, EV_DELETE, 0, 0, 0);
        if (it->regs & 2) EV_SET(&kev[n++], it->hfd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
        for (int i = 0; i < n; i++) kevent(kq, &kev[i], 1, NULL, 0, NULL);   /* ENOENT when already gone */
    }
    struct ep_item **pp = &ep->hash[(unsigned)it->fd & 255];
    while (*pp && *pp != it) pp = &(*pp)->hnext;
    if (*pp) *pp = it->hnext;
    int s = slot_of(ep, it);
    if (s >= 0) { ep->slots[s] = NULL; ep->freelist[ep->nfree++] = (uint32_t)s; }
    ep->count--;
    free(it);
}

/* ---- kqueue registration ---------------------------------------------------------------------- */

static int wants_read(uint32_t ev) { return (ev & (LX_EPOLLIN | LX_EPOLLPRI | LX_EPOLLRDNORM | LX_EPOLLRDBAND | LX_EPOLLRDHUP)) != 0; }
static int wants_write(uint32_t ev) { return (ev & (LX_EPOLLOUT | LX_EPOLLWRNORM | LX_EPOLLWRBAND)) != 0; }

/* (Re)register the item's filters. Returns 0 or -errno. */
static int ep_arm(lkd_fdobj *self, struct ep_item *it, uint32_t slot, int rebuild)
{
    int kq = ep_kq(self);
    int hfd;
    short hev;
    it->poll_only = 0;
    if (!net_obj_waitsrc(it->obj, it->events, &hfd, &hev)) {
        it->hfd = -1;
        it->regs = 0;
        it->poll_only = 1;                   /* nothing can wake us: scanned at every wait */
        return 0;
    }
    int rd = wants_read(it->events), wr = wants_write(it->events);
    int notify_only = it->obj->notify_fd >= 0;
    if (notify_only) {
        rd = 1;                              /* the notify pipe is read-only */
        if (wr) it->poll_only = 1;           /* OUT cannot be signalled by the pipe */
    }
    if (!rd && !wr) rd = 1;                  /* ERR|HUP only: the read filter reports EOF */
    uint16_t add = EV_ADD | EV_ENABLE | ((it->events & LX_EPOLLET) ? EV_CLEAR : 0);
    /* A read filter that only watches for hangup (no EPOLLIN/EPOLLRDNORM: events 0, or EPOLLRDHUP alone) must not
     * be level-triggered: while unread data sits in the host fd kqueue reports it at every wait, revents finds
     * nothing that was asked for, and epoll_wait spins (wineserver, which sets events to 0 on busy client sockets:
     * about 300,000 empty passes a second in a game). Edge-triggered, it wakes once per new data or at EOF. */
    uint16_t add_rd = add | ((it->events & (LX_EPOLLIN | LX_EPOLLRDNORM)) || notify_only ? 0 : EV_CLEAR);
    uint64_t ud = udata_of(slot, it->gen);
    struct kevent kev[2];
    int n = 0, newregs = 0;
    if (rd) { EV_SET(&kev[n++], hfd, EVFILT_READ, add_rd, 0, 0, (void *)(uintptr_t)ud); newregs |= 1 | ((add_rd & EV_CLEAR) ? 4 : 0); }
    if (wr && !notify_only) { EV_SET(&kev[n++], hfd, EVFILT_WRITE, add, 0, 0, (void *)(uintptr_t)ud); newregs |= 2; }
    if (!rebuild) {
        /* drop filters that are no longer wanted, and a read filter whose trigger mode changes (re-added below) */
        if ((it->regs & 1) && (!(newregs & 1) || (it->regs & 4) != (newregs & 4))) { struct kevent d; EV_SET(&d, hfd, EVFILT_READ, EV_DELETE, 0, 0, 0); kevent(kq, &d, 1, NULL, 0, NULL); }
        if ((it->regs & 2) && !(newregs & 2)) { struct kevent d; EV_SET(&d, hfd, EVFILT_WRITE, EV_DELETE, 0, 0, 0); kevent(kq, &d, 1, NULL, 0, NULL); }
    }
    for (int i = 0; i < n; i++) {
        if (kevent(kq, &kev[i], 1, NULL, 0, NULL) < 0) {
            int e = errno;
            it->regs = 0;
            it->hfd = -1;
            errno = e;
            return (int)LKD_HOST_ERR();
        }
    }
    it->hfd = hfd;
    it->regs = newregs;
    return 0;
}

/* ---- object ----------------------------------------------------------------------------------- */

static uint32_t ep_poll(lkd_fdobj *o, uint32_t want)
{
    /* An epoll descriptor is readable when some watched item is ready (ep_eventpoll_poll). Computed by
     * a scan; nested sets are bounded by EP_MAX_NESTS. */
    struct ep *ep = EP(o);
    (void)want;
    if (!ep) return 0;
    static __thread int depth;
    if (depth >= EP_MAX_NESTS) return 0;
    depth++;
    uint32_t r = 0;
    for (uint32_t i = 0;; i++) {
        net_lock();
        struct ep_item *it = NULL;
        while (i < ep->nslots && !(it = ep->slots[i])) i++;
        if (!it) { net_unlock(); break; }
        lkd_fdobj *ob = it->obj;
        uint32_t ev = it->events;
        int dis = it->disabled;
        int ok = 0;
        int rc = atomic_load(&ob->refs);
        while (rc > 0 && !atomic_compare_exchange_weak(&ob->refs, &rc, rc + 1)) { }
        ok = rc > 0;
        net_unlock();
        if (!ok) continue;
        if (!dis && (net_obj_revents(ob, ev, 0, 0) & (ev | EP_LEVEL_MASK))) r = LX_POLLIN | LX_POLLRDNORM;
        lkd_fdobj_put(ob);
        if (r) break;
    }
    depth--;
    return r;
}

static void ep_release(lkd_fdobj *o)
{
    struct ep *ep = o->priv;
    if (!ep) return;
    net_lock();
    if (ep->prev) ep->prev->next = ep->next;
    else if (g_eps == ep) g_eps = ep->next;
    if (ep->next) ep->next->prev = ep->prev;
    net_unlock();
    net_kqobj_del(&ep->kq);
    for (uint32_t i = 0; i < ep->nslots; i++) free(ep->slots[i]);
    free(ep->slots);
    free(ep->freelist);
    free(ep);
    o->priv = NULL;
}

static int ep_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    (void)o;
    int n = snprintf(buf, cap, "anon_inode:[eventpoll]");
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static int ep_fdinfo(lkd_fdobj *o, char *buf, size_t cap)
{
    struct ep *ep = o->priv;
    size_t used = 0;
    net_lock();
    for (uint32_t i = 0; ep && i < ep->nslots && used + 80 < cap; i++) {
        struct ep_item *it = ep->slots[i];
        if (!it) continue;
        int n = snprintf(buf + used, cap - used, "tfd: %8d events: %8x data: %16llx  pos:0 ino:%llx sdev:%x\n", it->fd,
                         it->events, (unsigned long long)it->data, (unsigned long long)it->obj->ino, 0);
        if (n < 0) break;
        used += (size_t)n;
    }
    net_unlock();
    return (int)used;
}

static int ep_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out) { (void)t; return net_anon_fstat(o, out); }

const lkd_fd_ops net_epoll_ops = {
    .name = "eventpoll",
    .poll = ep_poll,
    .fstat = ep_fstat,
    .describe = ep_describe,
    .fdinfo = ep_fdinfo,
    .release = ep_release,
};

/* Fork child: new kqueue, every item registered again (order 1: after timers and inotify). */
static void ep_rebuild(lkd_fdobj *o)
{
    struct ep *ep = o->priv;
    int kq = kqueue();
    if (kq < 0) return;
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    o->host_fd = kq;                          /* the inherited number is dead (EBADF, MESURE) */
    for (uint32_t i = 0; i < ep->nslots; i++) {
        struct ep_item *it = ep->slots[i];
        if (it) ep_arm(o, it, i, 1);
    }
}

void net_epoll_forget_object(lkd_fdobj *o)
{
    net_lock();
    for (struct ep *ep = g_eps; ep; ep = ep->next) {
        for (uint32_t i = 0; i < ep->nslots; i++) {
            struct ep_item *it = ep->slots[i];
            if (it && it->obj == o) ep_free_item(ep, ep->self, it);
        }
    }
    net_unlock();
}

/* ---- epoll_create ----------------------------------------------------------------------------- */

static int64_t do_create(lkd_task *t, int flags)
{
    if (flags & ~LX_EPOLL_CLOEXEC) return -LX_EINVAL;
    struct ep *ep = calloc(1, sizeof *ep);
    if (!ep) return -LX_ENOMEM;
    int kq = kqueue();
    if (kq < 0) { free(ep); return LKD_HOST_ERR(); }
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_EPOLL, &net_epoll_ops, kq, LX_O_RDWR, ep);
    if (!o) { close(kq); free(ep); return -LX_ENOMEM; }
    ep->self = o;
    ep->kq.o = o;
    ep->kq.order = 1;
    ep->kq.rebuild = ep_rebuild;
    net_kqobj_add(&ep->kq);
    net_lock();
    ep->next = g_eps;
    if (g_eps) g_eps->prev = ep;
    g_eps = ep;
    net_unlock();
    return net_install(t, o, (flags & LX_EPOLL_CLOEXEC) != 0);
}

LKD_SYSCALL(epoll_create)
{
    if (LKD_ARGI(0) <= 0) return -LX_EINVAL;      /* the size hint must be positive */
    return do_create(t, 0);
}
LKD_SYSCALL(epoll_create1) { return do_create(t, LKD_ARGI(0)); }

/* ---- epoll_ctl -------------------------------------------------------------------------------- */

/* ep_loop_check: would adding `child` (an epoll) under `parent` create a cycle or a chain deeper than
 * EP_MAX_NESTS? */
static int ep_depth(lkd_fdobj *o, lkd_fdobj *target, int depth)
{
    if (o == target) return -1;
    if (depth > EP_MAX_NESTS) return -1;
    struct ep *ep = EP(o);
    if (!ep) return depth;
    int worst = depth;
    net_lock();
    for (uint32_t i = 0; i < ep->nslots; i++) {
        struct ep_item *it = ep->slots[i];
        if (!it || !EP(it->obj)) continue;
        lkd_fdobj *c = it->obj;
        net_unlock();
        int d = ep_depth(c, target, depth + 1);
        net_lock();
        if (d < 0) { net_unlock(); return -1; }
        if (d > worst) worst = d;
    }
    net_unlock();
    return worst;
}

LKD_SYSCALL(epoll_ctl)
{
    int epfd = LKD_ARGI(0), op = LKD_ARGI(1), fd = LKD_ARGI(2);
    uint64_t evp = LKD_ARGP(3);
    lkd_fdobj *eo = lkd_fd_get(t, epfd, NULL);
    if (!eo) return -LX_EBADF;
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    int64_t r;
    if (!o) { lkd_fdobj_put(eo); return -LX_EBADF; }
    struct lx_epoll_event ev = { 0, 0 };
    if (op != LX_EPOLL_CTL_DEL || 1) {
        /* do_epoll_ctl: the event is copied for every operation but DEL (NULL allowed for DEL) */
        if (op != LX_EPOLL_CTL_DEL && lkd_user_read(evp, &ev, sizeof ev) < 0) { r = -LX_EFAULT; goto out; }
    }
    struct ep *ep = EP(eo);
    if (!ep || eo == o) { r = -LX_EINVAL; goto out; }
    if (o->kind == LKD_FD_FILE || o->kind == LKD_FD_DIR || o->kind == LKD_FD_MEMFD ||
        (o->host_fd < 0 && o->notify_fd < 0 && !(o->ops && o->ops->poll))) {
        r = -LX_EPERM;                          /* file_can_poll() is false */
        goto out;
    }
    if (op == LX_EPOLL_CTL_ADD || op == LX_EPOLL_CTL_MOD) {
        if (ev.events & ~(uint32_t)EP_ALLOWED_MASK) { /* unknown bits are accepted by Linux; keep them out of the mask */ }
        if (ev.events & LX_EPOLLEXCLUSIVE) {
            /* ep_op_has_event / EPOLLEXCLUSIVE_OK_BITS */
            if (op == LX_EPOLL_CTL_MOD) { r = -LX_EINVAL; goto out; }
            if (ev.events & ~(LX_EPOLLIN | LX_EPOLLOUT | LX_EPOLLERR | LX_EPOLLHUP | LX_EPOLLWAKEUP | LX_EPOLLET | LX_EPOLLEXCLUSIVE)) {
                r = -LX_EINVAL;
                goto out;
            }
        }
    }
    if (op == LX_EPOLL_CTL_ADD && EP(o)) {
        /* nested epoll: refuse loops and chains deeper than EP_MAX_NESTS (ELOOP) */
        if (ep_depth(o, eo, 1) < 0) { r = -LX_ELOOP; goto out; }
    }
    uint32_t events = (ev.events & ~(uint32_t)LX_EPOLLWAKEUP) | EP_LEVEL_MASK;
    net_lock();
    struct ep_item *it = ep_find(ep, fd, o);
    switch (op) {
    case LX_EPOLL_CTL_ADD: {
        if (it) { net_unlock(); r = -LX_EEXIST; goto out; }
        uint32_t slot;
        it = ep_alloc(ep, &slot);
        if (!it) { net_unlock(); r = -LX_ENOMEM; goto out; }
        it->fd = fd;
        it->obj = o;
        it->events = events;
        it->data = ev.data;
        it->hfd = -1;
        it->hnext = ep->hash[(unsigned)fd & 255];
        ep->hash[(unsigned)fd & 255] = it;
        int ar = ep_arm(eo, it, slot, 0);
        if (ar < 0) { ep_free_item(ep, eo, it); net_unlock(); r = ar; goto out; }
        r = 0;
        break;
    }
    case LX_EPOLL_CTL_MOD: {
        if (!it) { net_unlock(); r = -LX_ENOENT; goto out; }
        it->events = events;
        it->data = ev.data;
        it->disabled = 0;
        int s = slot_of(ep, it);
        int ar = ep_arm(eo, it, (uint32_t)s, 0);
        r = ar < 0 ? ar : 0;
        break;
    }
    case LX_EPOLL_CTL_DEL:
        if (!it) { net_unlock(); r = -LX_ENOENT; goto out; }
        ep_free_item(ep, eo, it);
        r = 0;
        break;
    default:
        r = -LX_EINVAL;
    }
    net_unlock();
out:
    lkd_fdobj_put(o);
    lkd_fdobj_put(eo);
    return r;
}

/* ---- epoll_wait ------------------------------------------------------------------------------- */

struct ready { uint32_t events; uint64_t data; };

/* One pass: collect up to max ready events into out (host memory). Returns the count. */
static int ep_collect(lkd_fdobj *eo, struct ready *out, int max)
{
    struct ep *ep = eo->priv;
    int n = 0;
    struct kevent kev[EP_KEV_BATCH];
    struct { uint32_t slot, gen; } cand[EP_KEV_BATCH + 1];

    /* 1. items the wait source cannot signal completely */
    net_lock();
    for (uint32_t i = 0; i < ep->nslots && n < max; i++) {
        struct ep_item *it = ep->slots[i];
        if (!it || !it->poll_only || it->disabled) continue;
        lkd_fdobj *ob = it->obj;
        uint32_t ev = it->events, flags = ev;
        uint64_t data = it->data;
        int rc = atomic_load(&ob->refs);
        while (rc > 0 && !atomic_compare_exchange_weak(&ob->refs, &rc, rc + 1)) { }
        if (rc <= 0) continue;
        net_unlock();
        uint32_t r = net_obj_revents(ob, ev, 0, 0) & (ev | EP_LEVEL_MASK);
        lkd_fdobj_put(ob);
        net_lock();
        if (!r) continue;
        out[n].events = r;
        out[n].data = data;
        n++;
        if (flags & LX_EPOLLONESHOT) {
            struct ep_item *cur = i < ep->nslots ? ep->slots[i] : NULL;
            if (cur == it) it->disabled = 1;
        }
    }
    net_unlock();
    if (n >= max) return n;

    /* 2. kqueue candidates */
    struct timespec zero = { 0, 0 };
    int kn = kevent(ep_kq(eo), NULL, 0, kev, EP_KEV_BATCH, &zero);
    if (kn <= 0) return n;
    for (int k = 0; k < kn && n < max; k++) {
        if (kev[k].flags & EV_ERROR) continue;
        uint64_t ud = (uint64_t)(uintptr_t)kev[k].udata;
        uint32_t slot = (uint32_t)ud, gen = (uint32_t)(ud >> 32);
        int dup = 0;
        for (int j = 0; j < k; j++) if (cand[j].slot == slot && cand[j].gen == gen) dup = 1;
        cand[k].slot = slot;
        cand[k].gen = gen;
        if (dup) continue;
        net_lock();
        struct ep_item *it = slot < ep->nslots ? ep->slots[slot] : NULL;
        if (!it || it->gen != gen || it->disabled || it->poll_only) { net_unlock(); continue; }
        lkd_fdobj *ob = it->obj;
        uint32_t ev = it->events;
        uint64_t data = it->data;
        int rc = atomic_load(&ob->refs);
        while (rc > 0 && !atomic_compare_exchange_weak(&ob->refs, &rc, rc + 1)) { }
        net_unlock();
        if (rc <= 0) continue;
        uint32_t r = net_obj_revents(ob, ev, 0, 0) & (ev | EP_LEVEL_MASK);
        lkd_fdobj_put(ob);
        if (!r) continue;
        out[n].events = r;
        out[n].data = data;
        n++;
        if (ev & LX_EPOLLONESHOT) {
            net_lock();
            struct ep_item *cur = slot < ep->nslots ? ep->slots[slot] : NULL;
            if (cur && cur->gen == gen) {
                cur->disabled = 1;
                struct kevent d[2];
                int nd = 0;
                if (cur->regs & 1) EV_SET(&d[nd++], cur->hfd, EVFILT_READ, EV_DISABLE, 0, 0, 0);
                if (cur->regs & 2) EV_SET(&d[nd++], cur->hfd, EVFILT_WRITE, EV_DISABLE, 0, 0, 0);
                for (int i = 0; i < nd; i++) kevent(ep_kq(eo), &d[i], 1, NULL, 0, NULL);
            }
            net_unlock();
        }
    }
    return n;
}

static int64_t do_wait(lkd_task *t, int epfd, uint64_t evp, int maxevents, int64_t deadline, int masked)
{
    if (maxevents <= 0 || maxevents > EP_MAX_EVENTS) return -LX_EINVAL;
    if (lkd_mm_check_range(evp, (size_t)maxevents * sizeof(struct lx_epoll_event), 1) < 0) return -LX_EFAULT;
    lkd_fdobj *eo = lkd_fd_get(t, epfd, NULL);
    if (!eo) return -LX_EBADF;
    if (!EP(eo)) { lkd_fdobj_put(eo); return -LX_EINVAL; }
    int cap = maxevents < 1024 ? maxevents : 1024;
    struct ready *rd = malloc((size_t)cap * sizeof *rd);
    if (!rd) { lkd_fdobj_put(eo); return -LX_ENOMEM; }
    int64_t ret;
    for (;;) {
        int n = ep_collect(eo, rd, cap);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                struct lx_epoll_event ev = { rd[i].events, rd[i].data };
                if (lkd_user_write(evp + (uint64_t)i * sizeof ev, &ev, sizeof ev) < 0) {
                    ret = i ? i : -LX_EFAULT;
                    goto done;
                }
            }
            ret = n;
            goto done;
        }
        if (deadline == 0 || (deadline > 0 && lkd_host_mono_ns() >= deadline)) { ret = 0; goto done; }
        int w = lkd_block_wait_fd(t, ep_kq(eo), POLLIN, deadline);
        if (w == -LX_ERESTARTSYS) { ret = masked ? -LX_ERESTARTNOHAND : -LX_EINTR; goto done; }
        if (w < 0) { ret = w; goto done; }
        if (w == 0) deadline = 0;               /* timed out: one last scan */
    }
done:
    free(rd);
    lkd_fdobj_put(eo);
    return ret;
}

static int64_t timeout_to_deadline_ms(int timeout)
{
    if (timeout < 0) return -1;
    if (timeout == 0) return 0;
    return net_deadline_from_ms(timeout);
}

LKD_SYSCALL(epoll_wait)
{
    return do_wait(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), timeout_to_deadline_ms(LKD_ARGI(3)), 0);
}

LKD_SYSCALL(epoll_pwait)
{
    int m = net_install_sigmask(t, LKD_ARGP(4), LKD_ARGU(5));
    if (m < 0) return m;
    int64_t r = do_wait(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), timeout_to_deadline_ms(LKD_ARGI(3)), m > 0);
    net_finish_temp_mask(t, r);
    return r;
}

LKD_SYSCALL(epoll_pwait2)
{
    int64_t deadline = -1;
    uint64_t tsp = LKD_ARGP(3);
    if (tsp) {
        struct lx_timespec ts;
        if (lkd_user_read(tsp, &ts, sizeof ts) < 0) return -LX_EFAULT;
        if (!net_ts_valid(&ts)) return -LX_EINVAL;
        deadline = (ts.tv_sec == 0 && ts.tv_nsec == 0) ? 0 : net_deadline_from_ts(&ts);
    }
    int m = net_install_sigmask(t, LKD_ARGP(4), LKD_ARGU(5));
    if (m < 0) return m;
    int64_t r = do_wait(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGI(2), deadline, m > 0);
    net_finish_temp_mask(t, r);
    return r;
}

/* epoll_ctl_old and epoll_wait_old (slots 214 and 215) were never implemented by Linux: ENOSYS. */
LKD_SYSCALL(epoll_ctl_old) { return -LX_ENOSYS; }
LKD_SYSCALL(epoll_wait_old) { return -LX_ENOSYS; }

void net_register_epoll(void)
{
    LKD_REGISTER(epoll_create);
    LKD_REGISTER(epoll_create1);
    LKD_REGISTER(epoll_ctl);
    LKD_REGISTER(epoll_wait);
    LKD_REGISTER(epoll_pwait);
    LKD_REGISTER(epoll_pwait2);
    LKD_REGISTER(epoll_ctl_old);
    LKD_REGISTER(epoll_wait_old);
}
