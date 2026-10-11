/* w36_syncpp: what a wait that sleeps and is woken costs under the engine, per round trip between two threads (two
 * wakes), for the paths a Windows game takes through Wine: an ntsync event pair (Wine 11's WaitForSingleObject and
 * SetEvent), a private futex pair (glibc's mutexes and condition variables), a pthread mutex and condition variable;
 * and the price of the calls ELDEN RING's job threads repeat while idle: sched_yield, getrusage(RUSAGE_THREAD),
 * clock_gettime. A measurement, not a check: it always ends with "W36 DONE".
 * Usage: w36_syncpp [round trips] (default 20000). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/futex.h>
#include <linux/ntsync.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long n_trips;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ---- ntsync events ---- */
static int dev, ev_ping, ev_pong;

static void nt_wait(int ev)
{
    __u32 objs[1] = { (__u32)ev };
    struct ntsync_wait_args a = { .timeout = UINT64_MAX, .objs = (uintptr_t)objs, .count = 1 };
    if (ioctl(dev, NTSYNC_IOC_WAIT_ANY, &a) < 0) { perror("WAIT_ANY"); exit(2); }
}

static void nt_set(int ev)
{
    __u32 prev;
    if (ioctl(ev, NTSYNC_IOC_EVENT_SET, &prev) < 0) { perror("EVENT_SET"); exit(2); }
}

static void *nt_echo(void *x)
{
    (void)x;
    for (long i = 0; i < n_trips; i++) { nt_wait(ev_ping); nt_set(ev_pong); }
    return NULL;
}

/* ---- private futexes ---- */
static _Atomic uint32_t fx_ping, fx_pong;

static void fx_wait(_Atomic uint32_t *w, uint32_t want)
{
    uint32_t v;
    while ((v = atomic_load(w)) != want) syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}

static void fx_post(_Atomic uint32_t *w, uint32_t v)
{
    atomic_store(w, v);
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void *fx_echo(void *x)
{
    (void)x;
    for (uint32_t i = 1; i <= (uint32_t)n_trips; i++) { fx_wait(&fx_ping, i); fx_post(&fx_pong, i); }
    return NULL;
}

/* ---- pthread mutex + condition variable ---- */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long turn; /* odd: the echo thread's turn */

static void *cv_echo(void *x)
{
    (void)x;
    pthread_mutex_lock(&mu);
    for (long i = 0; i < n_trips; i++) {
        while (!(turn & 1)) pthread_cond_wait(&cv, &mu);
        turn++;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

int main(int argc, char **argv)
{
    n_trips = argc > 1 ? atol(argv[1]) : 20000;
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_t t;
    double t0;

    dev = open("/dev/ntsync", O_RDWR | O_CLOEXEC);
    if (dev >= 0) {
        struct ntsync_event_args e = { .manual = 0, .signaled = 0 };
        ev_ping = ioctl(dev, NTSYNC_IOC_CREATE_EVENT, &e);
        ev_pong = ioctl(dev, NTSYNC_IOC_CREATE_EVENT, &e);
        pthread_create(&t, NULL, nt_echo, NULL);
        t0 = now_us();
        for (long i = 0; i < n_trips; i++) { nt_set(ev_ping); nt_wait(ev_pong); }
        printf("W36 ntsync event round trip      %8.2f us\n", (now_us() - t0) / n_trips);
        pthread_join(t, NULL);
    } else {
        printf("W36 no /dev/ntsync\n");
    }

    pthread_create(&t, NULL, fx_echo, NULL);
    t0 = now_us();
    for (uint32_t i = 1; i <= (uint32_t)n_trips; i++) { fx_post(&fx_ping, i); fx_wait(&fx_pong, i); }
    printf("W36 private futex round trip     %8.2f us\n", (now_us() - t0) / n_trips);
    pthread_join(t, NULL);

    pthread_create(&t, NULL, cv_echo, NULL);
    t0 = now_us();
    pthread_mutex_lock(&mu);
    for (long i = 0; i < n_trips; i++) {
        turn++;
        pthread_cond_broadcast(&cv);
        while (turn & 1) pthread_cond_wait(&cv, &mu);
    }
    pthread_mutex_unlock(&mu);
    printf("W36 mutex+condvar round trip     %8.2f us\n", (now_us() - t0) / n_trips);
    pthread_join(t, NULL);

    long n = n_trips * 10;
    t0 = now_us();
    for (long i = 0; i < n; i++) sched_yield();
    printf("W36 sched_yield                  %8.2f us\n", (now_us() - t0) / n);
    struct rusage ru;
    t0 = now_us();
    for (long i = 0; i < n; i++) getrusage(RUSAGE_THREAD, &ru);
    printf("W36 getrusage(RUSAGE_THREAD)     %8.2f us\n", (now_us() - t0) / n);
    struct timespec ts;
    t0 = now_us();
    for (long i = 0; i < n; i++) syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    printf("W36 clock_gettime (syscall)      %8.2f us\n", (now_us() - t0) / n);
    printf("W36 DONE\n");
    return 0;
}
