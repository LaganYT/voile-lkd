/* net_init.c: module init, the module lock, fork hooks and the registry of kqueue-backed objects. */
#include <unistd.h>

#include "net_priv.h"

static pthread_mutex_t g_net_lock = PTHREAD_MUTEX_INITIALIZER;   /* LKD_RANK_EVENTS */
static struct net_kqobj *g_kqobjs;

void net_lock(void) { pthread_mutex_lock(&g_net_lock); }
void net_unlock(void) { pthread_mutex_unlock(&g_net_lock); }

void net_kqobj_add(struct net_kqobj *k)
{
    net_lock();
    k->prev = NULL;
    k->next = g_kqobjs;
    if (g_kqobjs) g_kqobjs->prev = k;
    g_kqobjs = k;
    net_unlock();
}

void net_kqobj_del(struct net_kqobj *k)
{
    net_lock();
    if (k->prev) k->prev->next = k->next;
    else if (g_kqobjs == k) g_kqobjs = k->next;
    if (k->next) k->next->prev = k->prev;
    k->next = k->prev = NULL;
    net_unlock();
}

/* fd.h hook: the last reference of an object is going away; drop it from every epoll set. */
void lkd_net_object_released(lkd_fdobj *o)
{
    net_epoll_forget_object(o);
}

int net_anon_fstat(lkd_fdobj *o, struct lx_stat *out)
{
    /* anon_inode_getfile: one shared inode, i_mode = S_IRUSR | S_IWUSR without a file type */
    memset(out, 0, sizeof *out);
    out->st_dev = 0xe;
    out->st_ino = o->ino;
    out->st_nlink = 1;
    out->st_mode = 0600;
    out->st_blksize = 4096;
    return 0;
}

void lkd_net_init(void)
{
    net_register_socket();
    net_register_msg();
    net_register_sockopt();
    net_register_poll();
    net_register_epoll();
    net_register_eventfd();
    net_register_timerfd();
    net_register_inotify();
}

/* fork: kqueue descriptors are not inherited (MESURE, CONVENTIONS 6.2). The registry lock is taken
 * across fork so that the list is consistent, then every kqueue-backed object is rebuilt in the child,
 * timers and inotify (order 0) before epoll sets (order 1) that may watch them. */
void lkd_net_fork_prepare(lkd_task *t, uint64_t flags)
{
    (void)t; (void)flags;
    net_lock();
}

void lkd_net_fork_parent(lkd_task *t, pid_t child)
{
    (void)t; (void)child;
    net_unlock();
}

void lkd_net_fork_child(lkd_task *t)
{
    (void)t;
    lkd_fork_reinit_mutex(&g_net_lock, 0);
    for (int order = 0; order < 2; order++)
        for (struct net_kqobj *k = g_kqobjs; k; k = k->next)
            if (k->order == order && k->rebuild) k->rebuild(k->o);
}
