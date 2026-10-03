/* test_task_bigwin.c: vfork + execve + wait4 with a production-size guest window (2^46 bytes at a fixed
 * base, lkd_guest_base != 0), the shape that hung under the real engine on 2026-10-02
 * (/bin/sh -c '/bin/true; echo $?': the vfork child's in-place exec never finished, so the shell waited
 * forever). Linux: the parent of vfork resumes at the child's exec (complete_vfork_done) and wait4
 * returns the child's status (kernel/exit.c). The scenario runs in a forked host child bounded by a
 * deadline; RLIMIT_CPU (inherited by every host fork) bounds any grandchild that would spin. Needs the
 * mm and fs modules (skipped otherwise). At most 3 host processes exist at once. */
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ltest.h"
#include "fake_engine.h"
#include "mm.h"
#include "task.h"
#include "vfs.h"

static char D[512];
static lkd_task *E;

static int64_t sc(lkd_task *t, unsigned nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    lkd_sysreq rq = { nr, { a0, a1, a2, a3, a4, a5 } };
    return lkd_dispatch(t, &rq);
}

struct vf { int64_t ret; volatile int done; };
static void *vfork_thread(void *arg)
{
    struct vf *v = arg;
    lkd_set_current(E);
    v->ret = sc(E, LKD_NR_vfork, 0, 0, 0, 0, 0, 0);
    v->done = 1;
    return NULL;
}

/* Exit codes of the scenario child: 0 success, 10+ the step that failed. */
static void scenario(void *arg)
{
    (void)arg;
    struct rlimit rl = { 30, 30 };
    setrlimit(RLIMIT_CPU, &rl);
    static const char *argv0[] = { "/bin/sh", NULL };
    lkd_main_spec spec = { .size = sizeof spec, .exe_path = "/bin/sh", .argv = argv0, .cwd = "/" };
    E = lkd_task_create_main(&spec);
    if (!E || !E->mm) _exit(10);
    lkd_set_current(E);
    fake_cpu_of(E);
    /* guest memory for the execve arguments */
    int64_t g = sc(E, LKD_NR_mmap, 0, 16384, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, (uint64_t)-1, 0);
    if (g < 0) _exit(11);
    char path[600];
    snprintf(path, sizeof path, "%s/true", D);
    uint64_t gpath = (uint64_t)g, garg0 = gpath + 1024, gargv = gpath + 2048, genvp = gpath + 2080;
    uint64_t argv[2] = { garg0, 0 }, envp[1] = { 0 };
    if (lkd_user_write(gpath, path, strlen(path) + 1) || lkd_user_write(garg0, "/bin/true", 10) ||
        lkd_user_write(gargv, argv, sizeof argv) || lkd_user_write(genvp, envp, sizeof envp))
        _exit(12);
    struct vf v = { 0, 0 };
    pthread_t h;
    pthread_create(&h, NULL, vfork_thread, &v);
    for (int i = 0; i < 1000 && !fake_last_spawned; i++) usleep(2000);
    lkd_task *c = fake_last_spawned;
    if (!c) _exit(13);
    lkd_set_current(c);
    fake_cpu_of(c);
    int64_t r = sc(c, LKD_NR_execve, gpath, gargv, genvp, 0, 0, 0);
    if (r == LKD_RET_CONTEXT_REPLACED) {
        /* the vfork child, now in its own host process: /bin/true */
        sc(c, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0);
        int st;
        if (lkd_task_release(c, &st)) _exit(st);
        _exit(98);
    }
    if (r != LKD_RET_EXIT_THREAD) _exit(14);
    lkd_task_release(c, NULL);
    lkd_set_current(E);
    pthread_join(h, NULL);
    if (!v.done || v.ret <= 0) _exit(15);
    int64_t gst = gpath + 4096;
    int64_t w = sc(E, LKD_NR_wait4, (uint64_t)v.ret, (uint64_t)gst, 0, 0, 0, 0);
    if (w != v.ret) _exit(16);
    int32_t status = -1;
    lkd_user_read((uint64_t)gst, &status, sizeof status);
    if (status != 0) _exit(17);
    /* the shell's own exit: exit_group tears the address space down */
    sc(E, LKD_NR_exit_group, 0, 0, 0, 0, 0, 0);
    int st = -1;
    if (lkd_task_release(E, &st) != 1) _exit(18);
    _exit(st == 0 ? 0 : 19);
}

/* The whole sequence completes within 5 s (the exec and the exit reset the address space; a reset that
 * walks the empty 2^46 window one host page at a time takes far longer). */
LT_TEST(vfork_exec_wait_with_production_window)
{
    int64_t t0 = lkd_host_mono_ns();
    int rc = lt_run_child(scenario, NULL, 30);
    int64_t ms = (lkd_host_mono_ns() - t0) / 1000000;
    LT_MSG(rc == 0, "scenario child: rc %d after %lld ms (-1 = timeout; 10+ = failed step)", rc, (long long)ms);
    printf("scenario took %lld ms\n", (long long)ms);
    LT_CHECK(ms < 5000); /* MESURE with a per-VMA reset: about 1 s including two forks of a 2^46 reservation */
}

int main(void)
{
    lkd_config cfg = { .size = sizeof cfg, .guest_window_base = 0x80000000000ull, .guest_window_bytes = 1ull << 46 };
    LT_EQ(lkd_init_config(&cfg), 0);
    LT_EQ(fake_engine_init(), 0);
    const char *tmp = getenv("TMPDIR");
    snprintf(D, sizeof D, "%s/lkd-task-bigwin-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(D)) { perror("mkdtemp"); return 1; }
    unsigned char h[64];
    memset(h, 0, sizeof h);
    h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 2; h[5] = 1; h[6] = 1; h[16] = 2; h[18] = 62;
    char p[600];
    snprintf(p, sizeof p, "%s/true", D);
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    if (fd >= 0) { (void)!write(fd, h, sizeof h); close(fd); }
    struct lx_stat st;
    lkd_task *probe = NULL;
    int have = lkd_guest_base != 0; /* the mm module reserved the configured window */
    if (have) {
        /* the vfs must exist too; probe it without creating an mm in this process */
        probe = lkd_task_alloc(NULL, 0);
        probe->fsctx = lkd_fsctx_new();
        have = lkd_vfs_stat(probe, LX_AT_FDCWD, "/", 0, &st) == 0;
    }
    if (!have) printf("window %llu bytes, guest base 0x%llx\n", (unsigned long long)lkd_mm_window_size(), (unsigned long long)lkd_guest_base);
    if (have) LT_RUN(vfork_exec_wait_with_production_window);
    else { LT_CHECK(1); printf("SKIP vfork_exec_wait_with_production_window: needs the mm and fs modules\n"); }
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", D);
    (void)!system(cmd);
    return lt_finish();
}
