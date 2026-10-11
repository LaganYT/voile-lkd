/* w34_vlocksig: ELDEN RING's world loading froze for good with every thread waiting on LKD's VMA lock (mm->vlock),
 * one munmap waiting to write it and no thread holding it. This reproduces the load: threads that copy small
 * results to guest memory as fast as they can (clock_gettime and getrusage, a VMA lookup each through
 * lkd_user_write), threads that map and unmap private and shared memory (the writer side), and a thread that sends
 * SIGUSR1 to the copying threads all along (Wine's thread suspension and context reads). The main thread prints
 * the counters every second; it cannot report a freeze itself (its write needs the same lock), so a freeze shows as
 * counters that stop and a run killed by its timeout instead of "W34 PASS".
 * Usage: w34_vlocksig [seconds] [nosig|sigidle|sigmap] (default 30 s, signals to the copying threads; nosig: none;
 * sigidle: only to a thread that sleeps; sigmap: only to the mapping threads). */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define READERS 16
#define MAPPERS 4

static atomic_long reads, maps, sigs, sent;
static atomic_int stop;
static pid_t reader_tid[READERS], mapper_tid[MAPPERS], idle_tid;
static int target; /* 0 copying threads, 1 idle thread, 2 mapping threads */
static atomic_int readers_ready;

static void on_usr1(int s)
{
    (void)s;
    atomic_fetch_add_explicit(&sigs, 1, memory_order_relaxed);
}

static void *reader(void *arg)
{
    long i = (long)arg;
    reader_tid[i] = (pid_t)syscall(SYS_gettid);
    atomic_fetch_add(&readers_ready, 1);
    struct timespec ts;
    struct rusage ru;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
        if ((i & 1) == 0) syscall(SYS_getrusage, RUSAGE_THREAD, &ru);
        atomic_fetch_add_explicit(&reads, 1, memory_order_relaxed);
    }
    return NULL;
}

static void *mapper(void *arg)
{
    long i = (long)arg;
    mapper_tid[i] = (pid_t)syscall(SYS_gettid);
    int fd = memfd_create("w34", 0);
    if (fd < 0 || ftruncate(fd, 1 << 20) != 0) {
        perror("memfd");
        exit(2);
    }
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        size_t len = (size_t)(64 << 10) << (atomic_load_explicit(&maps, memory_order_relaxed) % 3);
        void *p = (i & 1) ? mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                          : mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            perror("mmap");
            exit(2);
        }
        ((volatile char *)p)[0] = 1;
        ((volatile char *)p)[len - 1] = 2;
        munmap(p, len);
        atomic_fetch_add_explicit(&maps, 1, memory_order_relaxed);
    }
    close(fd);
    return NULL;
}

static void *idle(void *arg)
{
    (void)arg;
    idle_tid = (pid_t)syscall(SYS_gettid);
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) usleep(1000);
    return NULL;
}

static void *signaler(void *arg)
{
    (void)arg;
    pid_t pid = getpid();
    for (unsigned n = 0; !atomic_load_explicit(&stop, memory_order_relaxed); n++) {
        pid_t to = target == 0 ? reader_tid[n % READERS] : target == 1 ? idle_tid : mapper_tid[n % MAPPERS];
        if (to) syscall(SYS_tgkill, pid, to, SIGUSR1);
        atomic_fetch_add_explicit(&sent, 1, memory_order_relaxed);
        if ((n & 7) == 7) usleep(100);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);

    pthread_t r[READERS], m[MAPPERS], s;
    for (long i = 0; i < READERS; i++) pthread_create(&r[i], NULL, reader, (void *)i);
    while (atomic_load(&readers_ready) < READERS) usleep(1000);
    for (long i = 0; i < MAPPERS; i++) pthread_create(&m[i], NULL, mapper, (void *)i);
    int nosig = argc > 2 && strcmp(argv[2], "nosig") == 0;
    target = argc > 2 && strcmp(argv[2], "sigidle") == 0 ? 1 : argc > 2 && strcmp(argv[2], "sigmap") == 0 ? 2 : 0;
    pthread_t it;
    pthread_create(&it, NULL, idle, NULL);
    while (!idle_tid) usleep(1000);
    if (!nosig) pthread_create(&s, NULL, signaler, NULL);

    long last_reads = 0, last_maps = 0;
    int still = 0;
    for (int t = 1; t <= seconds; t++) {
        sleep(1);
        long rd = atomic_load(&reads), mp = atomic_load(&maps);
        printf("t=%d reads=%ld maps=%ld signals=%ld/%ld\n", t, rd, mp, atomic_load(&sigs), atomic_load(&sent));
        still = (rd == last_reads || mp == last_maps) ? still + 1 : 0;
        if (still >= 3) {
            printf("W34 FAIL: no progress for 3 s\n");
            _exit(1);
        }
        last_reads = rd;
        last_maps = mp;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < READERS; i++) pthread_join(r[i], NULL);
    for (int i = 0; i < MAPPERS; i++) pthread_join(m[i], NULL);
    if (!nosig) pthread_join(s, NULL);
    pthread_join(it, NULL);
    printf("W34 PASS: %ld copies, %ld map/unmap, %ld signals\n", atomic_load(&reads), atomic_load(&maps), atomic_load(&sigs));
    return 0;
}
