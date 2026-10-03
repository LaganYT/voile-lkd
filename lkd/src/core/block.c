/* block.c: interruptible blocking helpers and the host clock. Owner: core.
 *
 * Every task owns a nonblocking wake pipe. A blocking wait polls the fd it waits for together with the
 * read end of that pipe, so a signal queued before or during the wait ends it without relying on a host
 * signal arriving at the right instant. See lkd_internal.h.
 */
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "lkd_internal.h"
#include "sig.h"
#include "task.h"

int64_t lkd_host_mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

void lkd_task_wake(lkd_task *t)
{
    if (!t || t->wake_wr < 0) return;
    char b = 1;
    ssize_t r;
    do { r = write(t->wake_wr, &b, 1); } while (r < 0 && errno == EINTR);
    /* EAGAIN: the pipe is full of wake bytes already, the waiter will see one. */
    lkd_sig_wake_hook(t);
}

static void drain_wake(lkd_task *t)
{
    char buf[64];
    while (read(t->wake_rd, buf, sizeof buf) > 0) { }
}

static int timeout_ms(int64_t deadline_ns)
{
    if (deadline_ns < 0) return -1;
    int64_t left = deadline_ns - lkd_host_mono_ns();
    if (left <= 0) return 0;
    int64_t ms = (left + 999999) / 1000000;
    return ms > 0x7fffffff ? 0x7fffffff : (int)ms;
}

static int block_poll(lkd_task *t, struct pollfd *fds, unsigned nfds, int64_t deadline_ns);

/* LKD_TF_IN_BLOCK while the task sleeps (request task-1 point 4): the registry and /proc show 'S' for a
 * task blocked in read, poll, accept... Nested waits keep the outer flag. */
int lkd_block_poll(lkd_task *t, struct pollfd *fds, unsigned nfds, int64_t deadline_ns)
{
    uint32_t prev = atomic_fetch_or(&t->flags, LKD_TF_IN_BLOCK);
    int r = block_poll(t, fds, nfds, deadline_ns);
    if (!(prev & LKD_TF_IN_BLOCK)) atomic_fetch_and(&t->flags, ~LKD_TF_IN_BLOCK);
    return r;
}

static int block_poll(lkd_task *t, struct pollfd *fds, unsigned nfds, int64_t deadline_ns)
{
    if (nfds > 1024) return -LX_EINVAL;
    struct pollfd local[1025];
    for (;;) {
        if (lkd_sig_interrupted(t)) return -LX_ERESTARTSYS;
        if (nfds) memcpy(local, fds, nfds * sizeof *fds);
        local[nfds].fd = t->wake_rd;
        local[nfds].events = POLLIN;
        local[nfds].revents = 0;
        int ms = timeout_ms(deadline_ns);
        int r = poll(local, nfds + 1, ms);
        if (r < 0) {
            if (errno == EINTR) continue; /* host signal (kick): re-check the interruption predicate */
            return LKD_HOST_ERR();
        }
        if (local[nfds].revents & POLLIN) {
            drain_wake(t);
            if (lkd_sig_interrupted(t)) return -LX_ERESTARTSYS;
            r -= 1;
        }
        if (r > 0) {
            int ready = 0;
            for (unsigned i = 0; i < nfds; i++) {
                fds[i].revents = local[i].revents;
                if (local[i].revents) ready++;
            }
            if (ready) return ready;
        }
        if (r == 0 && deadline_ns >= 0 && lkd_host_mono_ns() >= deadline_ns) return 0;
    }
}

int lkd_block_wait_fd(lkd_task *t, int host_fd, short events, int64_t deadline_ns)
{
    struct pollfd p = { .fd = host_fd, .events = events, .revents = 0 };
    int r = lkd_block_poll(t, &p, 1, deadline_ns);
    return r > 0 ? p.revents : r;
}

int lkd_block_sleep(lkd_task *t, int64_t deadline_ns)
{
    int r = lkd_block_poll(t, NULL, 0, deadline_ns);
    return r < 0 ? r : 0;
}

static int wait_wake(lkd_task *t, int64_t deadline_ns);

int lkd_block_wait_wake(lkd_task *t, int64_t deadline_ns)
{
    uint32_t prev = atomic_fetch_or(&t->flags, LKD_TF_IN_BLOCK);
    int r = wait_wake(t, deadline_ns);
    if (!(prev & LKD_TF_IN_BLOCK)) atomic_fetch_and(&t->flags, ~LKD_TF_IN_BLOCK);
    return r;
}

static int wait_wake(lkd_task *t, int64_t deadline_ns)
{
    for (;;) {
        if (lkd_sig_interrupted(t)) return -LX_ERESTARTSYS;
        /* poll the wake pipe alone: lkd_block_wait_fd(t, t->wake_rd) would list it twice, and macOS
         * reports only one of the two entries (MESURE, ptrace-2) */
        struct pollfd p = { .fd = t->wake_rd, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, timeout_ms(deadline_ns));
        if (r < 0) {
            if (errno == EINTR) continue;
            return LKD_HOST_ERR();
        }
        if (r > 0 && (p.revents & POLLIN)) {
            drain_wake(t);
            return lkd_sig_interrupted(t) ? -LX_ERESTARTSYS : 1;
        }
        if (deadline_ns >= 0 && lkd_host_mono_ns() >= deadline_ns) return 0;
    }
}
