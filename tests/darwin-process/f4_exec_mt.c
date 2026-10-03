/*
 * f4_exec_mt.c - F: execve() from a multi-threaded process.
 *
 * Roles (the binary re-runs itself):
 *   driver  (no argument)   spawns a "victim" per variant, collects latency numbers, runs the execve error table
 *   victim                  builds a multi-threaded process with known state, then execve()s this binary as "after"
 *   after                   checks what survived the exec and writes the result
 * Variants: exec from a secondary thread or from the main thread, small or huge address space, execve or
 * posix_spawn(POSIX_SPAWN_SETEXEC).
 */
#include "fg_util.h"
#include <mach/mach_vm.h>
#include <os/lock.h>
#include <os/os_sync_wait_on_address.h>
#include <sys/proc.h>

static int g_rfd = -1;
static const char *g_variant = "";
static int g_quiet;

static void usr2_noop(int s) {}

/* ---- helpers ------------------------------------------------------------------- */
static int get_start(pid_t pid, struct timeval *tv)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    struct kinfo_proc kp;
    size_t len = sizeof kp;
    memset(&kp, 0, sizeof kp);
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0) return -1;
    *tv = kp.kp_proc.p_starttime;
    return 0;
}

static int nthreads(void)
{
    thread_act_array_t th;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS) return -1;
    return (int)n;
}

static const char *env_or(const char *k, const char *d)
{
    const char *v = getenv(k);
    return v ? v : d;
}

/* ---- victim --------------------------------------------------------------------- */
static _Atomic int victim_stop;
static int vpipe[2];
static int kq_victim;
static _Atomic uint32_t ulock_word;

static void *vt_spin(void *a) { volatile uint64_t x = 0; while (!atomic_load(&victim_stop)) x++; return NULL; }
static void *vt_sleep(void *a) { while (!atomic_load(&victim_stop)) usleep(1000); return NULL; }
static void *vt_read(void *a) { char c; (void)!read(vpipe[0], &c, 1); return NULL; }
static void *vt_ulock(void *a) { os_sync_wait_on_address((void *)&ulock_word, 0, 4, OS_SYNC_WAIT_ON_ADDRESS_NONE); return NULL; }
static void *vt_kevent(void *a) { struct kevent ev; kevent(kq_victim, NULL, 0, &ev, 1, NULL); return NULL; }

struct exec_args { char **argv; char **envp; const char *use_spawn; };

static void *do_exec(void *p)
{
    struct exec_args *ea = p;
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGTERM);          /* mask of the exec'ing thread: will it become the main thread's mask? */
    pthread_sigmask(SIG_BLOCK, &blk, NULL);
    char t0[64];
    snprintf(t0, sizeof t0, "FG4_T0=%llu", (unsigned long long)mach_absolute_time());
    /* replace the placeholder env entry at index 0 */
    ea->envp[0] = t0;
    if (ea->use_spawn) {
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETEXEC);
        pid_t pid;
        int e = posix_spawn(&pid, dp_self_path(), NULL, &at, ea->argv, ea->envp);
        dp_emit("FAIL", "F4.victim", "posix_spawn(SETEXEC) returned %s instead of replacing the process", ERRS(e));
        _exit(3);
    }
    execve(dp_self_path(), ea->argv, ea->envp);
    dp_emit("FAIL", "F4.victim", "execve returned %s", ERRS(errno));
    _exit(3);
    return NULL;
}

static int role_victim(int argc, char **argv)
{
    int nonmain = 0, big = 0, huge = 0, spawn = 0;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--report=", 9)) g_rfd = atoi(argv[i] + 9);
        else if (!strcmp(argv[i], "--nonmain")) nonmain = 1;
        else if (!strcmp(argv[i], "--big")) big = 1;
        else if (!strcmp(argv[i], "--huge")) huge = 1;
        else if (!strcmp(argv[i], "--spawn")) spawn = 1;
        else if (!strcmp(argv[i], "--quiet")) g_quiet = 1;
        else if (!strncmp(argv[i], "--variant=", 10)) g_variant = argv[i] + 10;
    }
    dp_role = "victim";
    dp_watchdog(60);                       /* leaves an armed alarm: does it survive exec? */

    /* state that the exec will or will not preserve */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = usr2_noop;
    sigaction(SIGUSR2, &sa, NULL);
    int fd_plain = open("/dev/null", O_RDONLY);
    int fd_cloexec = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int fd_clofork = open("/dev/null", O_RDONLY | O_CLOFORK);
    umask(027);
    setpriority(PRIO_PROCESS, 0, 5);
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = 4321;
    setrlimit(RLIMIT_NOFILE, &rl);
    mach_port_t port = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    mach_port_set_context(mach_task_self(), port, 0x5a17c0de5a17c0deull);   /* identifies this very port after exec even if the name is recycled */
    unsigned char *shm = mmap(NULL, FG_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    *(volatile uint64_t *)shm = 0x4f4f50534d414749ull;   /* marker, looked for after exec */
    if (big) {
        void *r = mmap(NULL, 64ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        void *m = mmap(NULL, 1ull << 30, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m != MAP_FAILED) for (size_t o = 0; o < (1ull << 30); o += FG_PAGE) ((volatile unsigned char *)m)[o] = 1;
        (void)r;
    }
    if (huge) (void)mmap(NULL, 1ull << 46, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);

    pipe(vpipe);
    kq_victim = kqueue();
    pthread_t th[6];
    int n = 0;
    pthread_create(&th[n++], NULL, vt_spin, NULL);
    pthread_create(&th[n++], NULL, vt_sleep, NULL);
    pthread_create(&th[n++], NULL, vt_read, NULL);
    pthread_create(&th[n++], NULL, vt_ulock, NULL);
    pthread_create(&th[n++], NULL, vt_kevent, NULL);
    usleep(50000);

    struct timeval st;
    get_start(getpid(), &st);
    int victim_threads = nthreads();
    char e_pid[48], e_ss[48], e_su[48], e_thr[48], e_fds[96], e_port[48], e_addr[64], e_rfd[48], e_var[96], e_q[32], e_nonmain[32], e_ppid[32];
    snprintf(e_pid, sizeof e_pid, "FG4_PID=%d", getpid());
    snprintf(e_ss, sizeof e_ss, "FG4_START_SEC=%ld", (long)st.tv_sec);
    snprintf(e_su, sizeof e_su, "FG4_START_USEC=%ld", (long)st.tv_usec);
    snprintf(e_thr, sizeof e_thr, "FG4_NTHR=%d", victim_threads);
    snprintf(e_fds, sizeof e_fds, "FG4_FDS=%d,%d,%d", fd_plain, fd_cloexec, fd_clofork);
    snprintf(e_port, sizeof e_port, "FG4_PORT=%u", port);
    snprintf(e_addr, sizeof e_addr, "FG4_ADDR=%llu", (unsigned long long)(uintptr_t)shm);
    snprintf(e_rfd, sizeof e_rfd, "FG4_RFD=%d", g_rfd);
    snprintf(e_var, sizeof e_var, "FG4_VARIANT=%s", g_variant);
    snprintf(e_q, sizeof e_q, "FG4_QUIET=%d", g_quiet);
    snprintf(e_nonmain, sizeof e_nonmain, "FG4_NONMAIN=%d", nonmain);
    snprintf(e_ppid, sizeof e_ppid, "FG4_PPID=%d", getppid());
    char *envp[] = { "FG4_T0=0", e_pid, e_ss, e_su, e_thr, e_fds, e_port, e_addr, e_rfd, e_var, e_q, e_nonmain, e_ppid, "FG4_PGID=0", NULL };
    char pgid[32];
    snprintf(pgid, sizeof pgid, "FG4_PGID=%d", getpgrp());
    envp[13] = pgid;
    char rfd_arg[32];
    snprintf(rfd_arg, sizeof rfd_arg, "--report=%d", g_rfd);
    char *av[] = { "f4after", "--role=after", rfd_arg, NULL };
    struct exec_args ea = { av, envp, spawn ? "spawn" : NULL };
    if (!g_quiet)
        dp_emit("INFO", "F4.victim", "variant=%s pid=%d threads before exec=%d (main + 5 helpers + %s), exec from the %s thread, alarm armed, SIGUSR1 ignored, SIGUSR2 handled, SIGTERM blocked in the exec'ing thread, umask 027, nice 5, RLIMIT_NOFILE soft 4321",
                g_variant, getpid(), victim_threads, nonmain ? "exec thread" : "no extra thread", nonmain ? "secondary" : "main");
    if (nonmain) {
        pthread_t t;
        pthread_create(&t, NULL, do_exec, &ea);
        pthread_join(t, NULL);
    } else {
        do_exec(&ea);
    }
    return 4;
}

/* ---- the image after exec ---------------------------------------------------------- */
static mach_timebase_info_data_t tb;

static int role_after(int argc, char **argv)
{
    uint64_t now = mach_absolute_time();
    dp_role = "after";
    mach_timebase_info(&tb);
    g_rfd = atoi(env_or("FG4_RFD", "-1"));
    g_quiet = atoi(env_or("FG4_QUIET", "0"));
    const char *var = env_or("FG4_VARIANT", "?");
    uint64_t t0 = strtoull(env_or("FG4_T0", "0"), NULL, 10);
    double lat_us = (double)((now - t0) * tb.numer / tb.denom) / 1e3;
    if (g_rfd >= 0) {
        char b[128];
        int n = snprintf(b, sizeof b, "lat_us=%.1f\n", lat_us);
        ssize_t w = write(g_rfd, b, n);
        (void)w;
    }
    if (g_quiet) return 0;
    char id[64];
    snprintf(id, sizeof id, "F4.after");
    int ok_all = 1;
#define CK(cond, ...) do { int c_ = (cond); if (!c_) ok_all = 0; if (c_) PASS(id, __VA_ARGS__); else FAIL(id, __VA_ARGS__); } while (0)
    int vpid = atoi(env_or("FG4_PID", "0"));
    CK(getpid() == vpid, "[%s] pid is unchanged by exec (%d, was %d)", var, getpid(), vpid);
    CK(getppid() == atoi(env_or("FG4_PPID", "0")) && getpgrp() == atoi(env_or("FG4_PGID", "0")), "[%s] parent pid and process group are unchanged", var);
    struct timeval st;
    get_start(getpid(), &st);
    CK(st.tv_sec == atol(env_or("FG4_START_SEC", "0")) && st.tv_usec == atol(env_or("FG4_START_USEC", "0")), "[%s] process start time is unchanged (%ld.%06d)", var, (long)st.tv_sec, (int)st.tv_usec);
    int nt = nthreads();
    CK(nt == 1, "[%s] only 1 thread after exec (was %s before, including the exec'ing thread): %d", var, env_or("FG4_NTHR", "?"), nt);
    CK(pthread_main_np() == 1, "[%s] the surviving thread is the new main thread (pthread_main_np=%d, exec was called from a %s thread)", var, pthread_main_np(),
       atoi(env_or("FG4_NONMAIN", "0")) ? "secondary" : "main");
    INFO(id, "[%s] latency from just before execve to the first line of main(): %.0f us", var, lat_us);

    int a = -1, b = -1, c = -1;
    sscanf(env_or("FG4_FDS", "-1,-1,-1"), "%d,%d,%d", &a, &b, &c);
    CK(fcntl(a, F_GETFD) >= 0, "[%s] plain fd survives", var);
    CK(fcntl(b, F_GETFD) < 0, "[%s] FD_CLOEXEC fd is closed", var);
    CK(fcntl(c, F_GETFD) >= 0, "[%s] FD_CLOFORK-only fd survives exec (the flag only acts at fork/spawn)", var);
    struct sigaction sa;
    sigaction(SIGUSR1, NULL, &sa);
    CK(sa.sa_handler == SIG_IGN, "[%s] ignored signal stays ignored", var);
    sigaction(SIGUSR2, NULL, &sa);
    CK(sa.sa_handler == SIG_DFL, "[%s] handled signal is reset to default", var);
    sigset_t m;
    pthread_sigmask(SIG_BLOCK, NULL, &m);
    int termblk = sigismember(&m, SIGTERM);
    INFO(id, "[%s] SIGTERM blocked in the new main thread: %d (it was blocked only in the exec'ing thread, %s)", var, termblk,
         atoi(env_or("FG4_NONMAIN", "0")) ? "a secondary one" : "the main thread");
    unsigned left = alarm(0);
    INFO(id, "[%s] pending alarm after exec: %u s remaining (a 60 s alarm was armed before)", var, left);
    CK(umask(0) == 027, "[%s] umask is kept", var);
    CK(getpriority(PRIO_PROCESS, 0) == 5, "[%s] nice value is kept", var);
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    CK(rl.rlim_cur == 4321, "[%s] RLIMIT_NOFILE soft limit is kept (%llu)", var, (unsigned long long)rl.rlim_cur);
    mach_port_t port = (mach_port_t)atoi(env_or("FG4_PORT", "0"));
    mach_port_type_t pt = 0;
    kern_return_t kr = mach_port_type(mach_task_self(), port, &pt);
    mach_port_context_t ctx = 0;
    int same_port = 0;
    if (kr == KERN_SUCCESS && (pt & MACH_PORT_TYPE_RECEIVE) && mach_port_get_context(mach_task_self(), port, &ctx) == KERN_SUCCESS) same_port = (ctx == 0x5a17c0de5a17c0deull);
    CK(!same_port, "[%s] the Mach receive right created before exec is gone (name 0x%x: kr=%d type=0x%x; the name may be recycled by the new image, its context is %s)", var, port, kr,
       kr == KERN_SUCCESS ? pt : 0, same_port ? "the old marker" : "not the old marker");
    mach_vm_address_t addr = (mach_vm_address_t)strtoull(env_or("FG4_ADDR", "0"), NULL, 10);
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj;
    mach_vm_address_t probe = addr;
    kr = mach_vm_region(mach_task_self(), &probe, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    int in_region = (kr == KERN_SUCCESS && probe <= addr);
    int marker = 0;
    if (in_region && (info.protection & VM_PROT_READ)) marker = (*(volatile uint64_t *)(uintptr_t)addr == 0x4f4f50534d414749ull);
    CK(!marker, "[%s] the marker written in a MAP_SHARED|MAP_ANON page of the old image is gone (address 0x%llx %s)", var, (unsigned long long)addr, in_region ? "is mapped by something else" : "is not mapped");
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_msg_type_number_t ecnt = EXC_TYPES_COUNT;
    mach_port_t eports[EXC_TYPES_COUNT];
    exception_behavior_t beh[EXC_TYPES_COUNT];
    thread_state_flavor_t fl[EXC_TYPES_COUNT];
    kr = task_get_exception_ports(mach_task_self(), EXC_MASK_ALL, masks, &ecnt, eports, beh, fl);
    INFO(id, "[%s] task exception ports after exec: kr=%d entries=%u", var, kr, ecnt);
    INFO(id, "[%s] summary: %s", var, ok_all ? "all expectations met" : "some expectation failed");
    return ok_all ? 0 : 1;
}

/* ---- driver ------------------------------------------------------------------------ */
struct vrun { char rep[1024]; int status; int hung; };

static void run_victim(struct vrun *r, const char *variant, int nonmain, int big, int huge, int spawn, int quiet)
{
    memset(r, 0, sizeof *r);
    int p[2];
    pipe(p);
    char a1[32], a2[64];
    snprintf(a1, sizeof a1, "--report=%d", p[1]);
    snprintf(a2, sizeof a2, "--variant=%s", variant);
    char *argv[12];
    int n = 0;
    argv[n++] = "f4victim";
    argv[n++] = "--role=victim";
    argv[n++] = a1;
    argv[n++] = a2;
    if (nonmain) argv[n++] = "--nonmain";
    if (big) argv[n++] = "--big";
    if (huge) argv[n++] = "--huge";
    if (spawn) argv[n++] = "--spawn";
    if (quiet) argv[n++] = "--quiet";
    argv[n] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addinherit_np(&fa, p[1]);
    pid_t pid = fg_spawn_self(argv, &fa, NULL, NULL);
    close(p[1]);
    if (pid < 0) { close(p[0]); r->status = -1; return; }
    size_t got = 0;
    fg_set_nonblock(p[0]);
    int st = 0;
    pid_t w = dp_wait_timeout(pid, &st, 30000);
    if (w == 0) r->hung = 1;
    r->status = st;
    for (;;) {
        ssize_t k = read(p[0], r->rep + got, sizeof r->rep - 1 - got);
        if (k <= 0) break;
        got += k;
    }
    r->rep[got] = 0;
    close(p[0]);
    posix_spawn_file_actions_destroy(&fa);
}

static void latency_series(const char *label, int nonmain, int big, int huge, int spawn, int n)
{
    uint64_t v[32];
    int k = 0;
    for (int i = 0; i < n && i < 32; i++) {
        struct vrun r;
        run_victim(&r, label, nonmain, big, huge, spawn, 1);
        const char *q = strstr(r.rep, "lat_us=");
        if (q && !r.hung) v[k++] = (uint64_t)(atof(q + 7) * 1000.0);
    }
    if (!k) { INFO("F4.lat", "%s: no measurement", label); return; }
    dp_stats s = dp_stat_u64(v, k);
    INFO("F4.lat", "%-58s exec latency (to main of the new image): min=%.0f p50=%.0f max=%.0f us (n=%d)", label, s.min / 1e3, s.p50 / 1e3, s.max / 1e3, k);
}

static int exec_case_child(void *arg)
{
    void **a = arg;
    const char *path = a[0];
    char **argv = a[1];
    execve(path, argv, environ);
    return errno;      /* exit status = errno */
}

static void exec_errors(void)
{
    char *dir = fg_tmpname("f4err");
    mkdir(dir, 0700);
    char path[1200];
    struct {
        const char *what;
        const unsigned char *data;
        size_t len;
        mode_t mode;
    } cases[8];
    int nc = 0;
    static const unsigned char text[] = "echo hi\n";
    static const unsigned char elf[] = { 0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0x3e, 0, 1, 0, 0, 0 };
    static const unsigned char shebang[] = "#!/nonexistent/interp\n";
    /* 32-bit ARM Mach-O header, no load commands */
    static unsigned char macho_arm32[28];
    { uint32_t h[7] = { 0xfeedface, 12, 9, 2, 0, 0, 0 }; memcpy(macho_arm32, h, sizeof h); }
    /* arm64 Mach-O header claiming one load command of 4096 bytes in a file that ends right after the header */
    static unsigned char macho_bad[32];
    { uint32_t h[8] = { 0xfeedfacf, 0x0100000c, 0, 2, 1, 4096, 0, 0 }; memcpy(macho_bad, h, sizeof h); }
    /* arm64 dylib header with no load commands */
    static unsigned char macho_dylib[32];
    { uint32_t h[8] = { 0xfeedfacf, 0x0100000c, 0, 6, 0, 0, 0, 0 }; memcpy(macho_dylib, h, sizeof h); }
    cases[nc++] = (typeof(cases[0])){ "text file, exec bit, no #!", text, sizeof text - 1, 0700 };
    cases[nc++] = (typeof(cases[0])){ "ELF header (x86-64 shared object), exec bit", elf, sizeof elf, 0700 };
    cases[nc++] = (typeof(cases[0])){ "script whose interpreter is missing", shebang, sizeof shebang - 1, 0700 };
    cases[nc++] = (typeof(cases[0])){ "Mach-O 32-bit ARM header, no load commands", macho_arm32, sizeof macho_arm32, 0700 };
    cases[nc++] = (typeof(cases[0])){ "Mach-O arm64 with truncated load commands", macho_bad, sizeof macho_bad, 0700 };
    cases[nc++] = (typeof(cases[0])){ "Mach-O arm64 MH_DYLIB header", macho_dylib, sizeof macho_dylib, 0700 };
    cases[nc++] = (typeof(cases[0])){ "regular file without exec bit", text, sizeof text - 1, 0600 };
    cases[nc++] = (typeof(cases[0])){ "empty file, exec bit", text, 0, 0700 };
    for (int i = 0; i < nc; i++) {
        snprintf(path, sizeof path, "%s/case%d", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, cases[i].mode);
        if (fd >= 0) { ssize_t w = write(fd, cases[i].data, cases[i].len); (void)w; close(fd); chmod(path, cases[i].mode); }
        char *av[] = { "x", NULL };
        void *args[2] = { path, av };
        int st = 0;
        int rc = fg_run_in_child(exec_case_child, args, 5000, &st);
        INFO("F4.err", "execve of %s: %s", cases[i].what, rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : rc == FG_OK ? "exec succeeded (child exit 0)" : "child did not exit normally");
        unlink(path);
    }
    {
        char *av[] = { "x", NULL };
        void *a1[2] = { "/nonexistent/prog", av };
        int st = 0;
        int rc = fg_run_in_child(exec_case_child, a1, 5000, &st);
        INFO("F4.err", "execve of a missing path: %s", rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        void *a2[2] = { "/tmp", av };
        rc = fg_run_in_child(exec_case_child, a2, 5000, &st);
        INFO("F4.err", "execve of a directory: %s", rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        char longp[2100];
        memset(longp, 'a', sizeof longp - 1);
        longp[0] = '/';
        longp[sizeof longp - 1] = 0;
        void *a3[2] = { longp, av };
        rc = fg_run_in_child(exec_case_child, a3, 5000, &st);
        INFO("F4.err", "execve of a 2099-byte path: %s", rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        /* symlink loop */
        char l1[1200], l2[1200];
        snprintf(l1, sizeof l1, "%s/loop1", dir);
        snprintf(l2, sizeof l2, "%s/loop2", dir);
        symlink(l2, l1);
        symlink(l1, l2);
        void *a4[2] = { l1, av };
        rc = fg_run_in_child(exec_case_child, a4, 5000, &st);
        INFO("F4.err", "execve through a symlink loop: %s", rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        unlink(l1); unlink(l2);
        /* too many argument bytes */
        int mib[2] = { CTL_KERN, KERN_ARGMAX };
        int argmax = 0;
        size_t len = sizeof argmax;
        sysctl(mib, 2, &argmax, &len, NULL, 0);
        size_t big = (size_t)argmax + 4096;
        char *bigarg = malloc(big);
        memset(bigarg, 'x', big - 1);
        bigarg[big - 1] = 0;
        char *av2[] = { "x", bigarg, NULL };
        void *a5[2] = { "/usr/bin/true", av2 };
        rc = fg_run_in_child(exec_case_child, a5, 5000, &st);
        INFO("F4.err", "execve with %zu bytes of arguments (kern.argmax=%d): %s", big, argmax, rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        /* argv containing a single string just below argmax */
        size_t okl = (size_t)argmax - 65536;
        memset(bigarg, 'x', okl);
        bigarg[okl] = 0;
        rc = fg_run_in_child(exec_case_child, a5, 5000, &st);
        INFO("F4.err", "execve with %zu bytes of arguments plus the environment: %s", okl, rc == FG_OK ? "succeeds" : rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
        free(bigarg);
        /* NULL argv / envp */
        void *a6[2] = { "/usr/bin/true", NULL };
        rc = fg_run_in_child(exec_case_child, a6, 5000, &st);
        INFO("F4.err", "execve(\"/usr/bin/true\", NULL, environ): %s", rc == FG_OK ? "succeeds (Linux semantics: argv may be NULL)" : rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : "?");
    }
    /* text busy: executing a file that is open for writing */
    {
        snprintf(path, sizeof path, "%s/busy", dir);
        int src = open("/usr/bin/true", O_RDONLY);
        int dst = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0700);
        char buf[65536];
        ssize_t n;
        while ((n = read(src, buf, sizeof buf)) > 0) { ssize_t w = write(dst, buf, n); (void)w; }
        close(src);
        char *av[] = { "x", NULL };
        void *a[2] = { path, av };
        int st = 0;
        int rc = fg_run_in_child(exec_case_child, a, 5000, &st);
        INFO("F4.err", "execve of a copy of /usr/bin/true while it is still open for writing: %s", rc == FG_BADEXIT ? ERRS(WEXITSTATUS(st)) : rc == FG_OK ? "succeeded" : "?");
        close(dst);
        unlink(path);
    }
    rmdir(dir);
    free(dir);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--role=victim")) return role_victim(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--role=after")) return role_after(argc, argv);
    dp_watchdog(300);
    fg_load_info("F4.env");
    struct { const char *name; int nonmain, big, huge, spawn; } v[] = {
        { "exec-from-secondary-thread", 1, 0, 0, 0 },
        { "exec-from-main-thread", 0, 0, 0, 0 },
        { "posix_spawn-SETEXEC-from-secondary-thread", 1, 0, 0, 1 },
        { "exec-from-secondary-thread-big-address-space", 1, 1, 0, 0 },
    };
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        struct vrun r;
        run_victim(&r, v[i].name, v[i].nonmain, v[i].big, v[i].huge, v[i].spawn, 0);
        char b[96];
        INFO("F4.run", "variant %s: victim %s", v[i].name, r.hung ? "HUNG" : dp_status_str(r.status, b, sizeof b));
    }
    latency_series("exec, small process, secondary thread", 1, 0, 0, 0, 20);
    latency_series("exec, 64 GiB reservation + 1 GiB touched", 1, 1, 0, 0, 12);
    latency_series("exec, 64 TiB (2^46) PROT_NONE reservation", 1, 0, 1, 0, 8);
    latency_series("posix_spawn(SETEXEC), small process", 1, 0, 0, 1, 20);
    exec_errors();
    dp_summary("f4_exec_mt");
    return 0;
}
