/* w31_threadvm: thread creation when the engine has no host memory left for one more thread.
 *
 * Creates up to N threads (argument, default 280: LKD allows 300 per process) that all wait on a condition variable.
 * Each one costs the engine about 280 MiB of host address space. pthread_create must succeed or fail with EAGAIN, as
 * on Linux when the kernel has no memory for a new task; the process must stay alive. Then the witness releases and
 * joins every thread, and creates and joins 50 more: the memory of ended threads comes back.
 *
 * Run it once as is, and once with FEXDARWIN_GUEST_BASE=0x7d80000000 (the guest window base before 2026-10-10),
 * which leaves the host about 30 GiB: there the engine runs out after 100 threads or so. Before the fix, the thread
 * that found no room died on its first block with a host SIGSEGV at 0x10005297 (FEXCore built its lookup cache
 * pointers from a failed mmap).
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int go;

static void *waiter(void *arg)
{
    pthread_mutex_lock(&mu);
    while (!go) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return arg;
}

static void *quick(void *arg) { return arg; }

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 280;
    pthread_t *t = calloc((size_t)n, sizeof *t);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64 * 1024);
    int made = 0, eagain = 0, other = 0, first = 0;
    for (int i = 0; i < n; i++) {
        int e = pthread_create(&t[made], &at, waiter, (void *)(long)i);
        if (e == 0) { made++; continue; }
        if (e == EAGAIN) eagain++; else other++;
        if (!first) first = e;
        if (eagain + other >= 20) break;
    }
    printf("info: created %d of %d, EAGAIN %d, other errors %d%s%s\n", made, n, eagain, other, first ? ", first: " : "",
           first ? strerror(first) : "");
    fflush(stdout);

    pthread_mutex_lock(&mu);
    go = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    int joined = 0;
    for (int i = 0; i < made; i++) {
        void *r;
        if (pthread_join(t[i], &r) == 0) joined++;
    }

    int again = 0;
    for (int i = 0; i < 50; i++) {
        pthread_t x;
        if (pthread_create(&x, &at, quick, NULL) == 0 && pthread_join(x, NULL) == 0) again++;
    }
    int ok = made > 0 && other == 0 && joined == made && again == 50;
    printf("w31_threadvm: %s (created %d, joined %d, then %d/50 created and joined)\n", ok ? "PASS" : "FAIL", made, joined,
           again);
    return ok ? 0 : 1;
}
