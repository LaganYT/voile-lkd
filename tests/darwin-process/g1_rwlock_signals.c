/*
 * g1_rwlock_signals.c - G: a pthread_rwlock whose waiting threads take signals (ELDEN RING's freeze, 2026-10-11).
 *
 * 16 reader threads take a lock for read in a loop, 4 writer threads for write, while the main thread sends SIGUSR2
 * to one of them every 500 us (an empty handler, the way fexdarwin kicks a guest thread). Each case runs in its own
 * child for 6 s: progress every second, or "frozen" when neither readers nor writers moved for 3 s (the child then
 * exits without joining its blocked threads). Cases:
 *   rwlock          pthread_rwlock_t, no signals
 *   rwlock+sig      pthread_rwlock_t, signals, handler without SA_RESTART
 *   rwlock+restart  pthread_rwlock_t, signals, SA_RESTART
 *   mutex+sig       pthread_mutex_t (readers and writers both lock it), signals
 *   word+sig        a one-word lock on os_sync_wait_on_address (LKD's mm_rwlock), signals
 */
#include "fg_util.h"
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>

#define READERS 16
#define WRITERS 4
#define SECONDS 6

enum kind { RW, MUTEX, WORD };

static enum kind kind;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t word;
static atomic_long reads, writes;
static pthread_t th[READERS + WRITERS];

#define W_READERS 0x0000ffffu
#define W_WAITER 0x00010000u
#define W_WAITERS 0x7fff0000u
#define W_WRITER 0x80000000u

static void word_rdlock(void)
{
    uint32_t s = atomic_load(&word);
    for (;;) {
        if (!(s & (W_WRITER | W_WAITERS))) {
            if (atomic_compare_exchange_weak(&word, &s, s + 1)) return;
            continue;
        }
        os_sync_wait_on_address(&word, s, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        s = atomic_load(&word);
    }
}

static void word_rdunlock(void)
{
    uint32_t s = atomic_fetch_sub(&word, 1) - 1;
    if (!(s & W_READERS) && (s & W_WAITERS)) os_sync_wake_by_address_all(&word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

static void word_wrlock(void)
{
    uint32_t s = 0;
    if (atomic_compare_exchange_strong(&word, &s, W_WRITER)) return;
    s = atomic_fetch_add(&word, W_WAITER) + W_WAITER;
    for (;;) {
        if (!(s & (W_WRITER | W_READERS))) {
            if (atomic_compare_exchange_weak(&word, &s, (s - W_WAITER) | W_WRITER)) return;
            continue;
        }
        os_sync_wait_on_address(&word, s, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        s = atomic_load(&word);
    }
}

static void word_wrunlock(void)
{
    atomic_fetch_and(&word, ~W_WRITER);
    os_sync_wake_by_address_all(&word, 4, OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

static void rd(int lock)
{
    if (kind == RW) lock ? pthread_rwlock_rdlock(&rw) : pthread_rwlock_unlock(&rw);
    else if (kind == MUTEX) lock ? pthread_mutex_lock(&mu) : pthread_mutex_unlock(&mu);
    else lock ? word_rdlock() : word_rdunlock();
}

static void wr(int lock)
{
    if (kind == RW) lock ? pthread_rwlock_wrlock(&rw) : pthread_rwlock_unlock(&rw);
    else if (kind == MUTEX) lock ? pthread_mutex_lock(&mu) : pthread_mutex_unlock(&mu);
    else lock ? word_wrlock() : word_wrunlock();
}

static void on_sig(int s) { (void)s; }

static void *reader(void *a)
{
    (void)a;
    volatile int x = 0;
    for (;;) {
        rd(1);
        for (int i = 0; i < 20; i++) x++;
        rd(0);
        atomic_fetch_add_explicit(&reads, 1, memory_order_relaxed);
    }
    return NULL;
}

static void *writer(void *a)
{
    (void)a;
    volatile int x = 0;
    for (;;) {
        wr(1);
        for (int i = 0; i < 200; i++) x++;
        wr(0);
        atomic_fetch_add_explicit(&writes, 1, memory_order_relaxed);
        usleep(20);
    }
    return NULL;
}

/* In the child: 0 when both sides kept moving for SECONDS, 1 when they stopped for 3 s. */
static int run_case(enum kind k, int signals, int restart)
{
    kind = k;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = restart ? SA_RESTART : 0;
    sigaction(SIGUSR2, &sa, NULL);
    for (int i = 0; i < READERS; i++) pthread_create(&th[i], NULL, reader, NULL);
    for (int i = 0; i < WRITERS; i++) pthread_create(&th[READERS + i], NULL, writer, NULL);
    long lr = 0, lw = 0;
    int still = 0;
    unsigned n = 0;
    for (int t = 1; t <= SECONDS; t++) {
        for (int i = 0; i < 2000; i++) {
            if (signals) pthread_kill(th[n++ % (READERS + WRITERS)], SIGUSR2);
            usleep(500);
        }
        long r = atomic_load(&reads), w = atomic_load(&writes);
        still = (r == lr && w == lw) ? still + 1 : 0;
        lr = r;
        lw = w;
        if (still >= 3) {
            printf("    frozen at %d s: %ld reads, %ld writes\n", t, r, w);
            fflush(stdout);
            return 1;
        }
    }
    printf("    %ld reads, %ld writes in %d s\n", lr, lw, SECONDS);
    fflush(stdout);
    return 0;
}

static int in_child(enum kind k, int signals, int restart)
{
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) _exit(run_case(k, signals, restart));
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 2;
}

int main(void)
{
    dp_watchdog(120);
    fg_load_info("G1.env");
    int r;
    r = in_child(RW, 0, 0);
    CHECK(r == 0, "G1.rwlock", "pthread_rwlock without signals keeps moving (child %d)", r);
    r = in_child(RW, 1, 0);
    CHECK(r == 1, "G1.rwlock+sig", "pthread_rwlock with signals to its waiters freezes (child %d: 1 frozen, 0 moved)", r);
    r = in_child(RW, 1, 1);
    CHECK(r == 1, "G1.rwlock+restart", "same with SA_RESTART: freezes (child %d)", r);
    r = in_child(MUTEX, 1, 0);
    CHECK(r == 0, "G1.mutex+sig", "pthread_mutex with the same signals keeps moving (child %d)", r);
    r = in_child(WORD, 1, 0);
    CHECK(r == 0, "G1.word+sig", "a one-word lock on os_sync_wait_on_address with the same signals keeps moving (child %d)", r);
    return dp_summary("g1_rwlock_signals");
}
