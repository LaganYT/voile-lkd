/* inotify.c: inotify_init, inotify_init1, inotify_add_watch, inotify_rm_watch over EVFILT_VNODE.
 * Models fs/notify/inotify/inotify_user.c of Linux 6.12 (event layout, name padding, coalescing,
 * limits, errors). The honest subset, what the host can tell:
 *
 *   IN_MODIFY       NOTE_WRITE/NOTE_EXTEND of the watched file; for a watched directory, of each regular
 *                   file it contains (one extra descriptor per file, at most INOTIFY_MAX_CHILDREN)
 *   IN_ATTRIB       NOTE_ATTRIB (same rule for the children of a directory)
 *   IN_CREATE       a name appeared in a watched directory (directory rescan after NOTE_WRITE)
 *   IN_DELETE       a name vanished
 *   IN_MOVED_FROM/TO  a vanished and an appeared name with the same inode share a cookie (a rename that
 *                   leaves the directory shows as DELETE, and one that enters it as CREATE)
 *   IN_DELETE_SELF, IN_MOVE_SELF, IN_IGNORED  NOTE_DELETE, NOTE_RENAME, removal of the watch
 *   IN_ISDIR        on events about a directory entry
 * Never delivered (the host has no notification): IN_ACCESS, IN_OPEN, IN_CLOSE_WRITE, IN_CLOSE_NOWRITE.
 * Not recursive (as Linux). Events are collected lazily when the guest reads, polls or waits; the kqueue
 * keeps its knotes pending until then. The directory rescan can merge several changes of one burst, so
 * the order of a CREATE and a MODIFY inside the same burst is not guaranteed. kqueue is not inherited by
 * fork: the child rebuilds the registration, the queue is copied (Linux shares the instance).
 */
#include <dirent.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net_priv.h"

#define INOTIFY_MAX_QUEUED 16384
#define INOTIFY_MAX_WATCHES 8192
#define INOTIFY_MAX_INSTANCES 128
#define INOTIFY_MAX_CHILDREN 1024
#define EV_SIZE 16
#define LX_IN_ALL_EVENTS (LX_IN_ACCESS | LX_IN_MODIFY | LX_IN_ATTRIB | LX_IN_CLOSE_WRITE | LX_IN_CLOSE_NOWRITE | \
                          LX_IN_OPEN | LX_IN_MOVED_FROM | LX_IN_MOVED_TO | LX_IN_CREATE | LX_IN_DELETE | \
                          LX_IN_DELETE_SELF | LX_IN_MOVE_SELF)
#define LX_IN_FLAGS (LX_IN_ONLYDIR | LX_IN_DONT_FOLLOW | LX_IN_EXCL_UNLINK | LX_IN_MASK_CREATE | LX_IN_MASK_ADD | LX_IN_ISDIR | LX_IN_ONESHOT)

struct dent { char *name; uint64_t ino; uint8_t is_dir; int cfd; };   /* cfd: watch on a regular file child, -1 */

struct watch {
    struct watch *next;
    int wd;
    uint32_t mask;
    int fd;                      /* O_EVTONLY on the watched object */
    int is_dir;
    uint64_t dev, ino;
    char *path;                  /* host path at registration, used to rescan */
    struct dent *ents;           /* directory snapshot */
    size_t nents;
};

struct ev { struct ev *next; uint32_t len; uint8_t data[]; };

struct ino {
    struct watch *watches;
    int next_wd;
    int nwatches;
    struct ev *head, *tail;
    size_t nevents, bytes;
    int overflow_queued;
    uint32_t cookie;
    struct net_kqobj reg;
};

static _Atomic int g_instances;
static _Atomic int g_total_watches;

static struct ino *INO(lkd_fdobj *o) { return o && o->ops == &net_inotify_ops ? o->priv : NULL; }

/* ---- queue (o->lock held) --------------------------------------------------------------------- */

static size_t padded(size_t namelen) { return namelen ? ((namelen + 1 + EV_SIZE - 1) / EV_SIZE) * EV_SIZE : 0; }

static void queue_event(struct ino *n, int wd, uint32_t mask, uint32_t cookie, const char *name)
{
    size_t nl = name ? strlen(name) : 0, pl = padded(nl);
    /* fsnotify_add_event: an event equal to the last queued one is merged (inotify_merge) */
    if (n->tail) {
        struct lx_inotify_event *l = (struct lx_inotify_event *)n->tail->data;
        if (l->wd == wd && l->mask == mask && l->cookie == cookie && l->len == pl &&
            (pl == 0 || !strcmp(l->name, name))) return;
    }
    if (n->nevents >= INOTIFY_MAX_QUEUED) {
        if (n->overflow_queued) return;
        n->overflow_queued = 1;
        wd = -1; mask = LX_IN_Q_OVERFLOW; cookie = 0; nl = 0; pl = 0; name = NULL;
    }
    struct ev *e = calloc(1, sizeof *e + sizeof(struct lx_inotify_event) + pl);
    if (!e) return;
    struct lx_inotify_event *ie = (struct lx_inotify_event *)e->data;
    ie->wd = wd;
    ie->mask = mask;
    ie->cookie = cookie;
    ie->len = (uint32_t)pl;
    if (nl) memcpy(ie->name, name, nl);
    e->len = (uint32_t)(sizeof *ie + pl);
    if (n->tail) n->tail->next = e; else n->head = e;
    n->tail = e;
    n->nevents++;
    n->bytes += e->len;
}

static struct watch *find_wd(struct ino *n, int wd)
{
    for (struct watch *w = n->watches; w; w = w->next) if (w->wd == wd) return w;
    return NULL;
}

/* ---- directory snapshots ---------------------------------------------------------------------- */

static void free_ents(struct watch *w, int kq)
{
    for (size_t i = 0; i < w->nents; i++) {
        if (w->ents[i].cfd >= 0) {
            struct kevent kev;
            EV_SET(&kev, w->ents[i].cfd, EVFILT_VNODE, EV_DELETE, 0, 0, 0);
            kevent(kq, &kev, 1, NULL, 0, NULL);
            close(w->ents[i].cfd);
        }
        free(w->ents[i].name);
    }
    free(w->ents);
    w->ents = NULL;
    w->nents = 0;
}

static uint64_t udata_child(int wd, size_t idx) { return ((uint64_t)(uint32_t)wd << 32) | (uint32_t)(idx + 1); }

static void watch_child(struct ino *n, lkd_fdobj *o, struct watch *w, size_t idx)
{
    struct dent *d = &w->ents[idx];
    d->cfd = -1;
    if (d->is_dir || !(w->mask & (LX_IN_MODIFY | LX_IN_ATTRIB))) return;
    if (idx >= INOTIFY_MAX_CHILDREN) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", w->path, d->name);
    int fd = open(p, O_EVTONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return;
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, (void *)(uintptr_t)udata_child(w->wd, idx));
    if (kevent(o->host_fd, &kev, 1, NULL, 0, NULL) < 0) { close(fd); return; }
    d->cfd = fd;
    (void)n;
}

static int snapshot(struct ino *n, lkd_fdobj *o, struct watch *w, struct dent **out, size_t *cnt)
{
    DIR *dp = opendir(w->path);
    if (!dp) return -1;
    size_t cap = 16, c = 0;
    struct dent *a = malloc(cap * sizeof *a);
    struct dirent *de;
    while (a && (de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (c == cap) {
            struct dent *na = realloc(a, cap * 2 * sizeof *a);
            if (!na) break;
            a = na;
            cap *= 2;
        }
        a[c].name = strdup(de->d_name);
        a[c].ino = de->d_ino;
        a[c].is_dir = de->d_type == DT_DIR;
        a[c].cfd = -1;
        c++;
    }
    closedir(dp);
    (void)n; (void)o;
    *out = a;
    *cnt = c;
    return a ? 0 : -1;
}

/* Compare the old snapshot with the directory now and queue CREATE / DELETE / MOVED_*; then adopt the
 * new snapshot, keeping the child descriptors of the names that stayed. */
static void rescan_dir(struct ino *n, lkd_fdobj *o, struct watch *w)
{
    struct dent *nw;
    size_t nn;
    if (snapshot(n, o, w, &nw, &nn) < 0) return;
    int *gone = calloc(w->nents + 1, sizeof *gone), *born = calloc(nn + 1, sizeof *born);
    if (!gone || !born) { free(gone); free(born); for (size_t i = 0; i < nn; i++) free(nw[i].name); free(nw); return; }
    for (size_t i = 0; i < w->nents; i++) {
        gone[i] = 1;
        for (size_t j = 0; j < nn; j++)
            if (!strcmp(w->ents[i].name, nw[j].name) && w->ents[i].ino == nw[j].ino) { gone[i] = 0; break; }
    }
    for (size_t j = 0; j < nn; j++) {
        born[j] = 1;
        for (size_t i = 0; i < w->nents; i++)
            if (!strcmp(w->ents[i].name, nw[j].name) && w->ents[i].ino == nw[j].ino) { born[j] = 0; break; }
    }
    /* renames: a vanished and an appeared name with the same inode */
    for (size_t i = 0; i < w->nents; i++) {
        if (!gone[i]) continue;
        size_t mate = (size_t)-1;
        for (size_t j = 0; j < nn; j++) if (born[j] && nw[j].ino == w->ents[i].ino) { mate = j; break; }
        uint32_t isd = w->ents[i].is_dir ? LX_IN_ISDIR : 0;
        if (mate != (size_t)-1) {
            uint32_t ck = ++n->cookie;
            if (w->mask & LX_IN_MOVED_FROM) queue_event(n, w->wd, LX_IN_MOVED_FROM | isd, ck, w->ents[i].name);
            if (w->mask & LX_IN_MOVED_TO) queue_event(n, w->wd, LX_IN_MOVED_TO | isd, ck, nw[mate].name);
            born[mate] = 0;
        } else if (w->mask & LX_IN_DELETE) {
            queue_event(n, w->wd, LX_IN_DELETE | isd, 0, w->ents[i].name);
        }
    }
    for (size_t j = 0; j < nn; j++)
        if (born[j] && (w->mask & LX_IN_CREATE)) queue_event(n, w->wd, LX_IN_CREATE | (nw[j].is_dir ? LX_IN_ISDIR : 0), 0, nw[j].name);
    /* adopt: carry child descriptors over for unchanged names, drop the others */
    for (size_t i = 0; i < w->nents; i++) {
        int kept = 0;
        for (size_t j = 0; j < nn; j++)
            if (!strcmp(w->ents[i].name, nw[j].name) && w->ents[i].ino == nw[j].ino) {
                nw[j].cfd = w->ents[i].cfd;
                w->ents[i].cfd = -1;
                kept = 1;
                break;
            }
        if (!kept && w->ents[i].cfd >= 0) {
            struct kevent kev;
            EV_SET(&kev, w->ents[i].cfd, EVFILT_VNODE, EV_DELETE, 0, 0, 0);
            kevent(o->host_fd, &kev, 1, NULL, 0, NULL);
            close(w->ents[i].cfd);
        }
        free(w->ents[i].name);
    }
    free(w->ents);
    w->ents = nw;
    w->nents = nn;
    /* udata of kept children embeds the index: register again at the new index */
    for (size_t j = 0; j < nn; j++) {
        if (nw[j].cfd >= 0) {
            struct kevent kev;
            EV_SET(&kev, nw[j].cfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, (void *)(uintptr_t)udata_child(w->wd, j));
            kevent(o->host_fd, &kev, 1, NULL, 0, NULL);
        } else {
            watch_child(n, o, w, j);
        }
    }
    free(gone);
    free(born);
}

static void remove_watch(struct ino *n, lkd_fdobj *o, struct watch *w, int announce)
{
    struct watch **pp = &n->watches;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    if (announce) queue_event(n, w->wd, LX_IN_IGNORED, 0, NULL);
    struct kevent kev;
    EV_SET(&kev, w->fd, EVFILT_VNODE, EV_DELETE, 0, 0, 0);
    kevent(o->host_fd, &kev, 1, NULL, 0, NULL);
    free_ents(w, o->host_fd);
    close(w->fd);
    free(w->path);
    free(w);
    n->nwatches--;
    atomic_fetch_sub(&g_total_watches, 1);
}

/* Fetch the pending vnode events and turn them into inotify events (o->lock held). */
static void process(lkd_fdobj *o, struct ino *n)
{
    struct kevent kev[32];
    struct timespec z = { 0, 0 };
    int k;
    while ((k = kevent(o->host_fd, NULL, 0, kev, 32, &z)) > 0) {
        for (int i = 0; i < k; i++) {
            uint64_t ud = (uint64_t)(uintptr_t)kev[i].udata;
            int wd = (int)(ud >> 32);
            uint32_t ci = (uint32_t)ud;
            struct watch *w = find_wd(n, wd);
            if (!w) continue;
            uint32_t f = kev[i].fflags;
            if (ci) {
                /* a child file of a watched directory */
                size_t idx = ci - 1;
                if (idx >= w->nents) continue;
                const char *nm = w->ents[idx].name;
                if ((f & (NOTE_WRITE | NOTE_EXTEND)) && (w->mask & LX_IN_MODIFY)) queue_event(n, wd, LX_IN_MODIFY, 0, nm);
                if ((f & NOTE_ATTRIB) && (w->mask & LX_IN_ATTRIB)) queue_event(n, wd, LX_IN_ATTRIB, 0, nm);
                continue;
            }
            int gone = 0;
            if (w->is_dir) {
                if (f & NOTE_WRITE) rescan_dir(n, o, w);
                if ((f & NOTE_ATTRIB) && (w->mask & LX_IN_ATTRIB)) queue_event(n, wd, LX_IN_ATTRIB | LX_IN_ISDIR, 0, NULL);
            } else {
                if ((f & (NOTE_WRITE | NOTE_EXTEND)) && (w->mask & LX_IN_MODIFY)) queue_event(n, wd, LX_IN_MODIFY, 0, NULL);
                if ((f & NOTE_ATTRIB) && (w->mask & LX_IN_ATTRIB)) queue_event(n, wd, LX_IN_ATTRIB, 0, NULL);
            }
            uint32_t isd = w->is_dir ? LX_IN_ISDIR : 0;
            if ((f & NOTE_RENAME) && (w->mask & LX_IN_MOVE_SELF)) queue_event(n, wd, LX_IN_MOVE_SELF | isd, 0, NULL);
            if (f & NOTE_DELETE) {
                if (w->mask & LX_IN_DELETE_SELF) queue_event(n, wd, LX_IN_DELETE_SELF | isd, 0, NULL);
                remove_watch(n, o, w, 1);
                gone = 1;
            } else if (f & NOTE_REVOKE) {
                queue_event(n, wd, LX_IN_UNMOUNT | isd, 0, NULL);
                remove_watch(n, o, w, 1);
                gone = 1;
            }
            if (!gone && (w->mask & LX_IN_ONESHOT) && n->tail) {
                struct lx_inotify_event *l = (struct lx_inotify_event *)n->tail->data;
                if (l->wd == wd) remove_watch(n, o, w, 1);
            }
        }
    }
}

/* ---- object ----------------------------------------------------------------------------------- */

static uint32_t ino_poll(lkd_fdobj *o, uint32_t want)
{
    (void)want;
    struct ino *n = o->priv;
    pthread_mutex_lock(&o->lock);
    process(o, n);
    int ready = n->head != NULL;
    pthread_mutex_unlock(&o->lock);
    return ready ? (LX_POLLIN | LX_POLLRDNORM) : 0;
}

static int nonblocking(lkd_fdobj *o) { return (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0; }

static int64_t ino_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    struct ino *n = o->priv;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        process(o, n);
        if (n->head) {
            /* inotify_read: the first event must fit (EINVAL), then as many as fit */
            if (n->head->len > count) { pthread_mutex_unlock(&o->lock); return -LX_EINVAL; }
            uint64_t used = 0;
            uint8_t *buf = malloc(count < 65536 ? count : 65536);
            if (!buf) { pthread_mutex_unlock(&o->lock); return -LX_ENOMEM; }
            uint64_t cap = count < 65536 ? count : 65536;
            while (n->head && used + n->head->len <= cap) {
                struct ev *e = n->head;
                memcpy(buf + used, e->data, e->len);
                used += e->len;
                n->head = e->next;
                if (!n->head) n->tail = NULL;
                n->nevents--;
                n->bytes -= e->len;
                if (n->nevents == 0) n->overflow_queued = 0;
                free(e);
            }
            pthread_mutex_unlock(&o->lock);
            int64_t r = lkd_user_write(gva, buf, used) < 0 ? -LX_EFAULT : (int64_t)used;
            free(buf);
            return r;
        }
        pthread_mutex_unlock(&o->lock);
        if (nonblocking(o)) return -LX_EAGAIN;
        int w = lkd_block_wait_fd(t, o->host_fd, POLLIN, -1);
        if (w < 0) return w;
    }
}

static int64_t ino_ioctl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t;
    struct ino *n = o->priv;
    if (cmd == LX_FIONREAD) {
        pthread_mutex_lock(&o->lock);
        process(o, n);
        int32_t v = (int32_t)n->bytes;
        pthread_mutex_unlock(&o->lock);
        return lkd_user_write(arg, &v, sizeof v) < 0 ? -LX_EFAULT : 0;
    }
    return -LX_ENOTTY;
}

static int64_t ino_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)arg;
    return cmd == LX_F_SETFL ? 0 : -LX_EINVAL;
}

static int ino_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    (void)o;
    int n = snprintf(buf, cap, "anon_inode:inotify");
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static int ino_fdinfo(lkd_fdobj *o, char *buf, size_t cap)
{
    struct ino *n = o->priv;
    size_t used = 0;
    pthread_mutex_lock(&o->lock);
    for (struct watch *w = n->watches; w && used + 100 < cap; w = w->next) {
        int k = snprintf(buf + used, cap - used, "inotify wd:%x ino:%llx sdev:%llx mask:%x ignored_mask:0 fhandle-bytes:8 fhandle-type:1 f_handle:0\n",
                         w->wd, (unsigned long long)w->ino, (unsigned long long)w->dev, w->mask);
        if (k < 0) break;
        used += (size_t)k;
    }
    pthread_mutex_unlock(&o->lock);
    return (int)used;
}

static int ino_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out) { (void)t; return net_anon_fstat(o, out); }

static void ino_release(lkd_fdobj *o)
{
    struct ino *n = o->priv;
    if (!n) return;
    net_kqobj_del(&n->reg);
    while (n->watches) remove_watch(n, o, n->watches, 0);
    while (n->head) { struct ev *e = n->head; n->head = e->next; free(e); }
    free(n);
    o->priv = NULL;
    atomic_fetch_sub(&g_instances, 1);
}

const lkd_fd_ops net_inotify_ops = {
    .name = "inotify",
    .read = ino_read,
    .ioctl = ino_ioctl,
    .fcntl = ino_fcntl,
    .poll = ino_poll,
    .fstat = ino_fstat,
    .describe = ino_describe,
    .fdinfo = ino_fdinfo,
    .release = ino_release,
};

/* Fork child: new kqueue; every watch (and child file) registered again on it. */
static void ino_rebuild(lkd_fdobj *o)
{
    struct ino *n = o->priv;
    int kq = kqueue();
    if (kq < 0) return;
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    o->host_fd = kq;
    for (struct watch *w = n->watches; w; w = w->next) {
        struct kevent kev;
        EV_SET(&kev, w->fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
               0, (void *)(uintptr_t)((uint64_t)(uint32_t)w->wd << 32));
        kevent(kq, &kev, 1, NULL, 0, NULL);
        for (size_t j = 0; j < w->nents; j++) {
            if (w->ents[j].cfd < 0) continue;
            EV_SET(&kev, w->ents[j].cfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, (void *)(uintptr_t)udata_child(w->wd, j));
            kevent(kq, &kev, 1, NULL, 0, NULL);
        }
    }
}

/* ---- syscalls --------------------------------------------------------------------------------- */

static int64_t do_init(lkd_task *t, int flags)
{
    if (flags & ~(LX_IN_CLOEXEC | LX_IN_NONBLOCK)) return -LX_EINVAL;
    if (atomic_fetch_add(&g_instances, 1) >= INOTIFY_MAX_INSTANCES) { atomic_fetch_sub(&g_instances, 1); return -LX_EMFILE; }
    struct ino *n = calloc(1, sizeof *n);
    int kq = kqueue();
    lkd_fdobj *o = NULL;
    if (n && kq >= 0) {
        fcntl(kq, F_SETFD, FD_CLOEXEC);
        o = lkd_fdobj_new(LKD_FD_INOTIFY, &net_inotify_ops, kq, LX_O_RDONLY | (flags & LX_IN_NONBLOCK), n);
    }
    if (!o) {
        int e = kq < 0 && n ? (int)LKD_HOST_ERR() : -LX_ENOMEM;
        if (kq >= 0) close(kq);
        free(n);
        atomic_fetch_sub(&g_instances, 1);
        return e;
    }
    n->next_wd = 1;
    n->reg.o = o;
    n->reg.order = 0;
    n->reg.rebuild = ino_rebuild;
    net_kqobj_add(&n->reg);
    return net_install(t, o, (flags & LX_IN_CLOEXEC) != 0);
}

LKD_SYSCALL(inotify_init) { return do_init(t, 0); }
LKD_SYSCALL(inotify_init1) { return do_init(t, LKD_ARGI(0)); }

LKD_SYSCALL(inotify_add_watch)
{
    int fd = LKD_ARGI(0);
    uint32_t mask = LKD_ARGUI(2);
    char gp[LX_PATH_MAX];
    int64_t pl = lkd_user_strn(LKD_ARGP(1), gp, sizeof gp);
    if (pl < 0) return pl;
    /* inotify_arg_to_mask / inotify_update_watch (v6.12): */
    if (!(mask & LX_IN_ALL_EVENTS)) return -LX_EINVAL;
    if ((mask & LX_IN_MASK_ADD) && (mask & LX_IN_MASK_CREATE)) return -LX_EINVAL;
    if (mask & ~(uint32_t)(LX_IN_ALL_EVENTS | LX_IN_FLAGS)) return -LX_EINVAL;
    lkd_fdobj *o = lkd_fd_get(t, fd, NULL);
    if (!o) return -LX_EBADF;
    struct ino *n = INO(o);
    if (!n) { lkd_fdobj_put(o); return -LX_EINVAL; }
    char host[LX_PATH_MAX];
    int r = lkd_vfs_to_host(t, LX_AT_FDCWD, gp, (mask & LX_IN_DONT_FOLLOW) ? LX_AT_SYMLINK_NOFOLLOW : 0, host, sizeof host);
    if (r == -LX_EXDEV) {
        lkd_log_once("net: inotify on a virtual file system (/proc, /sys, /dev) is not available (-EOPNOTSUPP)");
        lkd_fdobj_put(o);
        return -LX_EOPNOTSUPP;
    }
    if (r < 0) { lkd_fdobj_put(o); return r; }
    int64_t ret;
    int hfd = open(host, O_EVTONLY | O_CLOEXEC | ((mask & LX_IN_DONT_FOLLOW) ? O_NOFOLLOW : 0));
    if (hfd < 0) { lkd_fdobj_put(o); return LKD_HOST_ERR(); }
    struct stat st;
    if (fstat(hfd, &st) < 0) { int64_t e = LKD_HOST_ERR(); close(hfd); lkd_fdobj_put(o); return e; }
    if ((mask & LX_IN_ONLYDIR) && !S_ISDIR(st.st_mode)) { close(hfd); lkd_fdobj_put(o); return -LX_ENOTDIR; }
    pthread_mutex_lock(&o->lock);
    struct watch *w;
    for (w = n->watches; w; w = w->next) if (w->dev == (uint64_t)st.st_dev && w->ino == st.st_ino) break;
    if (w) {
        /* the same object again: update the mask, return the same wd */
        close(hfd);
        if (mask & LX_IN_MASK_CREATE) { ret = -LX_EEXIST; goto out; }
        uint32_t m = mask & (LX_IN_ALL_EVENTS | LX_IN_ONESHOT);
        w->mask = (mask & LX_IN_MASK_ADD) ? (w->mask | m) : m;
        ret = w->wd;
        goto out;
    }
    if (atomic_load(&g_total_watches) >= INOTIFY_MAX_WATCHES) { close(hfd); ret = -LX_ENOSPC; goto out; }
    w = calloc(1, sizeof *w);
    if (!w) { close(hfd); ret = -LX_ENOMEM; goto out; }
    w->wd = n->next_wd++;
    w->mask = mask & (LX_IN_ALL_EVENTS | LX_IN_ONESHOT);
    w->fd = hfd;
    w->is_dir = S_ISDIR(st.st_mode);
    w->dev = (uint64_t)st.st_dev;
    w->ino = st.st_ino;
    w->path = strdup(host);
    struct kevent kev;
    EV_SET(&kev, hfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE,
           0, (void *)(uintptr_t)((uint64_t)(uint32_t)w->wd << 32));
    if (!w->path || kevent(o->host_fd, &kev, 1, NULL, 0, NULL) < 0) {
        ret = w->path ? LKD_HOST_ERR() : -LX_ENOMEM;
        close(hfd);
        free(w->path);
        free(w);
        goto out;
    }
    w->next = n->watches;
    n->watches = w;
    n->nwatches++;
    atomic_fetch_add(&g_total_watches, 1);
    if (w->is_dir) {
        if (snapshot(n, o, w, &w->ents, &w->nents) == 0)
            for (size_t j = 0; j < w->nents; j++) watch_child(n, o, w, j);
    }
    ret = w->wd;
out:
    pthread_mutex_unlock(&o->lock);
    lkd_fdobj_put(o);
    return ret;
}

LKD_SYSCALL(inotify_rm_watch)
{
    lkd_fdobj *o = lkd_fd_get(t, LKD_ARGI(0), NULL);
    if (!o) return -LX_EBADF;
    struct ino *n = INO(o);
    if (!n) { lkd_fdobj_put(o); return -LX_EINVAL; }
    pthread_mutex_lock(&o->lock);
    struct watch *w = find_wd(n, LKD_ARGI(1));
    int64_t r = 0;
    if (!w) r = -LX_EINVAL;
    else remove_watch(n, o, w, 1);                 /* IN_IGNORED is queued */
    pthread_mutex_unlock(&o->lock);
    lkd_fdobj_put(o);
    return r;
}

void net_register_inotify(void)
{
    LKD_REGISTER(inotify_init);
    LKD_REGISTER(inotify_init1);
    LKD_REGISTER(inotify_add_watch);
    LKD_REGISTER(inotify_rm_watch);
}

void lkd_net_inotify_limits(struct lkd_inotify_limits *out)
{
    out->max_user_instances = INOTIFY_MAX_INSTANCES;
    out->max_user_watches = INOTIFY_MAX_WATCHES;
    out->max_queued_events = INOTIFY_MAX_QUEUED;
}
