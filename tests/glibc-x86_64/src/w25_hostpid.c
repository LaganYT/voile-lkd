/* w25_hostpid: /proc/self/lkd_host_pid and guest/steam-hostpid.so in a real guest. Run by ../hostpid.sh, which checks
 * every host pid the witness reports against macOS (ps) before it lets the witness go on.
 *
 *   w25_hostpid DIR                    main thread, a thread, fork then exec (with the library preloaded), posix_spawn
 *   w25_hostpid DIR exec HOSTPID       after the exec: same host process as before it; the library's getpid() from a
 *                                      library named steamclient.so (DIR/steamclient.so) and from elsewhere, in a
 *                                      thread and after a fork
 *   w25_hostpid DIR spawn HOSTPID      a posix_spawn child: a host process of its own
 *
 * Each step prints "W25 <step> guest=<pid> host=<host pid>" and waits for /ack/<host pid>-<step> (W25_ACK=1 only).
 * The process exits 0 when every check passes, otherwise prints "W25 FAIL ..." and exits 1. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static const char *g_dir;
static int g_fail;

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) (void)!write(1, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

#define CHECK(c, ...) do { if (!(c)) { out("W25 FAIL %s:%d ", __FILE__, __LINE__); out(__VA_ARGS__); out("\n"); g_fail = 1; } } while (0)

static long guest_pid(void) { return syscall(SYS_getpid); }

static long read_host(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* Report a step and wait until the runner has checked the host pid against macOS. */
static void step(const char *name, long host)
{
    out("W25 %s guest=%ld host=%ld\n", name, guest_pid(), host);
    if (!getenv("W25_ACK")) return;
    char ack[96];
    snprintf(ack, sizeof ack, "/ack/%ld-%s", host, name);
    struct stat st;
    for (int i = 0; i < 2000; i++) {
        if (stat(ack, &st) == 0) return;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(0, "no ack for %s", ack);
}

static void *thread_main(void *arg)
{
    long *r = arg;
    r[0] = read_host("/proc/thread-self/lkd_host_pid");
    r[1] = read_host("/proc/self/lkd_host_pid");
    r[2] = syscall(SYS_gettid);
    return NULL;
}

static int wait_ok(pid_t p)
{
    int st = 0;
    if (waitpid(p, &st, 0) != p) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int run_main(char *self)
{
    long h0 = read_host("/proc/self/lkd_host_pid");
    CHECK(h0 > 0, "no /proc/self/lkd_host_pid (%ld)", h0);
    CHECK(h0 != guest_pid(), "host pid %ld equals the guest pid", h0);
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/lkd_host_pid", guest_pid());
    CHECK(read_host(path) == h0, "%s differs from /proc/self", path);
    step("main", h0);

    long r[3] = { 0, 0, 0 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, thread_main, r) == 0, "pthread_create");
    pthread_join(th, NULL);
    CHECK(r[2] != guest_pid(), "the thread has the main tid");
    CHECK(r[0] == h0 && r[1] == h0, "thread: %ld %ld, process %ld", r[0], r[1], h0);
    step("thread", r[0]);

    long parent_guest = guest_pid();
    pid_t c = fork();
    if (c == 0) {
        long h1 = read_host("/proc/self/lkd_host_pid");
        CHECK(h1 > 0 && h1 != h0, "fork child: %ld, parent %ld", h1, h0);
        snprintf(path, sizeof path, "/proc/%ld/lkd_host_pid", parent_guest);
        CHECK(read_host(path) == h0, "fork child sees its parent at %ld", read_host(path));
        step("fork", h1);
        if (g_fail) _exit(1);
        char hs[24], pre[256];
        snprintf(hs, sizeof hs, "%ld", h1);
        snprintf(pre, sizeof pre, "LD_PRELOAD=%s/steam-hostpid.so", g_dir);
        char *argv[] = { self, (char *)g_dir, "exec", hs, NULL };
        char *envp[] = { pre, getenv("W25_ACK") ? "W25_ACK=1" : "W25_NOACK=1", NULL };
        execve(self, argv, envp);
        out("W25 FAIL execve %s\n", self);
        _exit(1);
    }
    CHECK(c > 0 && wait_ok(c), "fork and exec child failed");

    char hs[24];
    snprintf(hs, sizeof hs, "%ld", h0);
    char *argv[] = { self, (char *)g_dir, "spawn", hs, NULL };
    pid_t s;
    CHECK(posix_spawn(&s, self, NULL, NULL, argv, environ) == 0, "posix_spawn");
    CHECK(wait_ok(s), "spawn child failed");
    return g_fail;
}

static long (*g_steam_getpid)(void);

static void *shim_thread(void *arg)
{
    long *r = arg;
    r[0] = g_steam_getpid();
    r[1] = getpid();
    return NULL;
}

static int run_exec(long before)
{
    long h2 = read_host("/proc/self/lkd_host_pid");
    CHECK(h2 == before, "exec: %ld, before the exec %ld", h2, before);
    step("exec", h2);

    char lib[256];
    snprintf(lib, sizeof lib, "%s/steamclient.so", g_dir);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    CHECK(h != NULL, "dlopen %s: %s", lib, dlerror());
    if (!h) return 1;
    g_steam_getpid = (long (*)(void))dlsym(h, "fake_steam_getpid");
    CHECK(g_steam_getpid != NULL, "fake_steam_getpid missing");
    if (!g_steam_getpid) return 1;
    long v = g_steam_getpid();
    CHECK(v == h2, "steamclient.so getpid %ld, host %ld", v, h2);
    CHECK(getpid() == guest_pid(), "getpid() outside steamclient.so: %ld, guest %ld", (long)getpid(), guest_pid());
    CHECK(getpid() != h2, "getpid() outside steamclient.so is the host pid");
    step("shim", v);

    long r[2] = { 0, 0 };
    pthread_t th;
    CHECK(pthread_create(&th, NULL, shim_thread, r) == 0, "pthread_create");
    pthread_join(th, NULL);
    CHECK(r[0] == h2 && r[1] == guest_pid(), "shim thread: %ld %ld", r[0], r[1]);
    step("shim-thread", r[0]);

    pid_t c = fork();
    if (c == 0) {
        long own = read_host("/proc/self/lkd_host_pid");
        long w = g_steam_getpid();
        CHECK(own > 0 && own != h2 && w == own, "shim after fork: %ld, own %ld, parent %ld", w, own, h2);
        CHECK(getpid() == guest_pid(), "getpid() after fork");
        step("shim-fork", w);
        _exit(g_fail);
    }
    CHECK(c > 0 && wait_ok(c), "shim fork child failed");
    CHECK(g_steam_getpid() == h2, "parent after fork: %ld", g_steam_getpid());
    return g_fail;
}

static int run_spawn(long parent)
{
    long h3 = read_host("/proc/self/lkd_host_pid");
    CHECK(h3 > 0 && h3 != parent, "spawn: %ld, parent %ld", h3, parent);
    step("spawn", h3);
    return g_fail;
}

int main(int argc, char **argv)
{
    if (argc < 2) { out("usage: w25_hostpid DIR [exec|spawn HOSTPID]\n"); return 2; }
    g_dir = argv[1];
    int rc;
    if (argc >= 4 && !strcmp(argv[2], "exec")) rc = run_exec(atol(argv[3]));
    else if (argc >= 4 && !strcmp(argv[2], "spawn")) rc = run_spawn(atol(argv[3]));
    else rc = run_main(argv[0]);
    if (!rc && argc < 3) out("W25 PASS\n");
    return rc ? 1 : 0;
}
