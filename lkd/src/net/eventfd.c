/* eventfd.c: eventfd and eventfd2. Models fs/eventfd.c of Linux 6.12 (eventfd_read, eventfd_write,
 * eventfd_poll, eventfd_show_fdinfo, do_eventfd).
 *
 * The counter lives in a 16 KiB POSIX shared memory object mapped MAP_SHARED, so a fork child and any
 * process that receives the eventfd through SCM_RIGHTS share it, as Linux shares the file. Updates are
 * lock-free (compare and swap), which survives the death of any process (CONVENTIONS 3.4).
 *
 * Readiness for host pollers (poll, epoll over kqueue, other processes) is a nonblocking pipe: every
 * write puts one byte in it (edge-triggered watchers see each write, like the wake-up eventfd_write
 * does), and whoever finds the counter at zero drains the pipe and then checks the counter again,
 * putting a byte back if a writer raced. Hence: counter != 0 implies the pipe holds a byte; a byte with
 * a zero counter is only a spurious wake-up that the next check removes.
 *
 * A writer that would overflow the counter waits by sleeping 1 ms steps (interruptible): Linux wakes it
 * on the next read; the gap only costs latency in that rare case.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "net_priv.h"

#define EFD_REGION 16384
#define EFD_MAGIC "LKDEVFD1"
#define EFD_MAX 0xfffffffffffffffeull          /* ULLONG_MAX - 1 */

struct efd_region {
    char magic[8];
    uint32_t version;
    uint32_t semaphore;                         /* EFD_SEMAPHORE belongs to the file, shared */
    _Atomic uint64_t count;
    _Atomic uint32_t id;
};

struct efd {
    struct efd_region *r;
    int shm_fd;
    int wr;                                     /* write end of the readiness pipe (rd is notify_fd) */
};

static _Atomic uint32_t g_efd_id;

static void drain(int fd)
{
    char buf[256];
    while (read(fd, buf, sizeof buf) > 0) { }
}

static void kick(struct efd *e)
{
    char c = 1;
    (void)!write(e->wr, &c, 1);                 /* nonblocking: a full pipe is readable already */
}

/* Drain, then re-check: restores the invariant "count != 0 => pipe not empty". */
static void settle(lkd_fdobj *o, struct efd *e)
{
    drain(o->notify_fd);
    if (atomic_load(&e->r->count)) kick(e);
}

static uint32_t efd_poll(lkd_fdobj *o, uint32_t want)
{
    (void)want;
    struct efd *e = o->priv;
    uint64_t c = atomic_load(&e->r->count);
    if (!c) {
        settle(o, e);
        c = atomic_load(&e->r->count);
    }
    uint32_t ev = 0;
    if (c > 0) ev |= LX_POLLIN | LX_POLLRDNORM;
    if (c == UINT64_MAX) ev |= LX_POLLERR;
    if (EFD_MAX > c) ev |= LX_POLLOUT | LX_POLLWRNORM;
    return ev;
}

static int nonblocking(lkd_fdobj *o) { return (atomic_load(&o->status_flags) & LX_O_NONBLOCK) != 0; }

static int64_t efd_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    struct efd *e = o->priv;
    if (count < 8) return -LX_EINVAL;
    for (;;) {
        uint64_t c = atomic_load(&e->r->count);
        if (c) {
            uint64_t take = e->r->semaphore ? 1 : c;
            if (!atomic_compare_exchange_weak(&e->r->count, &c, c - take)) continue;
            if (c - take == 0) settle(o, e);
            return lkd_user_write(gva, &take, 8) < 0 ? -LX_EFAULT : 8;
        }
        if (nonblocking(o)) return -LX_EAGAIN;
        settle(o, e);
        if (atomic_load(&e->r->count)) continue;
        int w = lkd_block_wait_fd(t, o->notify_fd, POLLIN, -1);
        if (w < 0) return w;                    /* -ERESTARTSYS: read is restartable */
    }
}

static int64_t efd_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *off)
{
    if (off) return -LX_ESPIPE;
    struct efd *e = o->priv;
    if (count < 8) return -LX_EINVAL;
    uint64_t v;
    if (lkd_user_read(gva, &v, 8) < 0) return -LX_EFAULT;
    if (v == UINT64_MAX) return -LX_EINVAL;
    for (;;) {
        uint64_t c = atomic_load(&e->r->count);
        if (UINT64_MAX - c > v) {
            if (!atomic_compare_exchange_weak(&e->r->count, &c, c + v)) continue;
            kick(e);
            return 8;
        }
        if (nonblocking(o)) return -LX_EAGAIN;
        int s = lkd_block_sleep(t, lkd_host_mono_ns() + 1000000);
        if (s < 0) return s;
    }
}

static int efd_describe(lkd_fdobj *o, char *buf, size_t cap)
{
    (void)o;
    int n = snprintf(buf, cap, "anon_inode:[eventfd]");
    return n < (int)cap ? n : -LX_ENAMETOOLONG;
}

static int efd_fdinfo(lkd_fdobj *o, char *buf, size_t cap)
{
    struct efd *e = o->priv;
    int n = snprintf(buf, cap, "eventfd-count: %16llx\neventfd-id: %u\neventfd-semaphore: %u\n",
                     (unsigned long long)atomic_load(&e->r->count), atomic_load(&e->r->id), e->r->semaphore ? 1u : 0u);
    return n < (int)cap ? n : (int)cap - 1;
}

static int efd_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out) { (void)t; return net_anon_fstat(o, out); }

static int64_t efd_fcntl(lkd_task *t, lkd_fdobj *o, uint32_t cmd, uint64_t arg)
{
    (void)t; (void)o; (void)arg;
    return cmd == LX_F_SETFL ? 0 : -LX_EINVAL;
}

static void efd_release(lkd_fdobj *o)
{
    struct efd *e = o->priv;
    if (!e) return;
    munmap(e->r, EFD_REGION);
    close(e->shm_fd);
    close(e->wr);
    free(e);
    o->priv = NULL;
}

const lkd_fd_ops net_eventfd_ops = {
    .name = "eventfd",
    .read = efd_read,
    .write = efd_write,
    .fcntl = efd_fcntl,
    .poll = efd_poll,
    .fstat = efd_fstat,
    .describe = efd_describe,
    .fdinfo = efd_fdinfo,
    .release = efd_release,
};

static int wrap(int shm_fd, struct efd_region *r, int rd, int wr, uint32_t lx_flags, lkd_fdobj **out)
{
    struct efd *e = calloc(1, sizeof *e);
    if (!e) return -LX_ENOMEM;
    e->r = r;
    e->shm_fd = shm_fd;
    e->wr = wr;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_EVENTFD, &net_eventfd_ops, -1, LX_O_RDWR | lx_flags, e);
    if (!o) { free(e); return -LX_ENOMEM; }
    o->notify_fd = rd;
    o->dev = 0xe;
    *out = o;
    return 0;
}

static int efd_create(uint64_t initval, int semaphore, uint32_t lx_flags, lkd_fdobj **out)
{
    char name[32];
    static _Atomic uint32_t seq;
    int shm = -1;
    for (int i = 0; i < 16 && shm < 0; i++) {
        snprintf(name, sizeof name, "/lkdefd.%d.%u", (int)getpid(), atomic_fetch_add(&seq, 1));
        shm = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (shm < 0 && errno != EEXIST) return (int)LKD_HOST_ERR();
    }
    if (shm < 0) return -LX_ENFILE;
    shm_unlink(name);
    fcntl(shm, F_SETFD, FD_CLOEXEC);
    if (ftruncate(shm, EFD_REGION) < 0) { int e = (int)LKD_HOST_ERR(); close(shm); return e; }
    struct efd_region *r = mmap(NULL, EFD_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
    if (r == MAP_FAILED) { int e = (int)LKD_HOST_ERR(); close(shm); return e; }
    memcpy(r->magic, EFD_MAGIC, 8);
    r->version = 1;
    r->semaphore = semaphore ? 1 : 0;
    atomic_store(&r->count, initval);
    atomic_store(&r->id, atomic_fetch_add(&g_efd_id, 1));
    int rd, wr;
    int e = lkd_net_make_notify_pipe(&rd, &wr);
    if (e < 0) { munmap(r, EFD_REGION); close(shm); return e; }
    e = wrap(shm, r, rd, wr, lx_flags, out);
    if (e < 0) { munmap(r, EFD_REGION); close(shm); close(rd); close(wr); return e; }
    if (initval) kick((*out)->priv);
    return 0;
}

/* ---- SCM_RIGHTS -------------------------------------------------------------------------------- */

int net_eventfd_export(lkd_fdobj *o, int *hfds)
{
    struct efd *e = o->priv;
    int src[3] = { e->shm_fd, o->notify_fd, e->wr };
    for (int i = 0; i < 3; i++) {
        hfds[i] = fcntl(src[i], F_DUPFD_CLOEXEC, 0);
        if (hfds[i] < 0) {
            int err = (int)LKD_HOST_ERR();
            for (int j = 0; j < i; j++) close(hfds[j]);
            return err;
        }
    }
    return 3;
}

/* A received descriptor is an LKD eventfd region when it is a POSIX shm object (MESURE: fstat shows no
 * file type bits) of exactly 16 KiB that starts with the magic. */
int net_eventfd_is_region(int fd)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || (st.st_mode & S_IFMT) != 0 || st.st_size != EFD_REGION) return 0;
    struct efd_region *r = mmap(NULL, EFD_REGION, PROT_READ, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) return 0;
    int ok = !memcmp(r->magic, EFD_MAGIC, 8) && r->version == 1;
    munmap(r, EFD_REGION);
    return ok;
}

int net_eventfd_from_host(int shm_fd, int rd, int wr, lkd_fdobj **out)
{
    struct efd_region *r = mmap(NULL, EFD_REGION, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (r == MAP_FAILED) return (int)LKD_HOST_ERR();
    int fl = fcntl(rd, F_GETFL);
    if (fl < 0 || !(fl & O_NONBLOCK)) fcntl(rd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
    fcntl(shm_fd, F_SETFD, FD_CLOEXEC);
    fcntl(rd, F_SETFD, FD_CLOEXEC);
    fcntl(wr, F_SETFD, FD_CLOEXEC);
    int e = wrap(shm_fd, r, rd, wr, 0, out);
    if (e < 0) munmap(r, EFD_REGION);
    return e;
}

/* ---- syscalls ---------------------------------------------------------------------------------- */

static int64_t do_eventfd(lkd_task *t, uint32_t initval, int flags)
{
    if (flags & ~(LX_EFD_CLOEXEC | LX_EFD_NONBLOCK | LX_EFD_SEMAPHORE)) return -LX_EINVAL;
    lkd_fdobj *o;
    int e = efd_create(initval, flags & LX_EFD_SEMAPHORE, flags & LX_EFD_NONBLOCK, &o);
    if (e < 0) return e;
    return net_install(t, o, (flags & LX_EFD_CLOEXEC) != 0);
}

LKD_SYSCALL(eventfd) { return do_eventfd(t, LKD_ARGUI(0), 0); }
LKD_SYSCALL(eventfd2) { return do_eventfd(t, LKD_ARGUI(0), LKD_ARGI(1)); }

void net_register_eventfd(void)
{
    LKD_REGISTER(eventfd);
    LKD_REGISTER(eventfd2);
}
