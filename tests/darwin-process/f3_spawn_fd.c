/*
 * f3_spawn_fd.c - F: posix_spawn of the same binary, fd inheritance, POSIX_SPAWN_CLOEXEC_DEFAULT interplay,
 * sharing memory with a spawned child through an inherited fd, process attributes across spawn, cost, start-suspended,
 * and FD_CLOFORK across fork.
 *
 * The program re-runs itself with a role argument. A child role writes a text report to an inherited pipe
 * (so that it still works when POSIX_SPAWN_CLOEXEC_DEFAULT closed stdout); the parent parses it.
 */
#include "fg_util.h"
#include <mach-o/dyld.h>

#define REPORT_MAX 65536
static int g_count_from;   /* parent: lowest fd the child should scan for gaps (0 = no scan) */

/* ---- child roles -------------------------------------------------------------- */
static void rep(int rfd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rep(int rfd, const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(rfd, b, n < (int)sizeof b ? n : (int)sizeof b - 1); (void)w; }
}

static const char *sig_state(int sig)
{
    struct sigaction sa;
    sigaction(sig, NULL, &sa);
    if (sa.sa_handler == SIG_IGN) return "ignored";
    if (sa.sa_handler == SIG_DFL) return "default";
    return "handler";
}

static int role_report(int argc, char **argv)
{
    int rfd = -1;
    const char *fds = "";
    int count_below = 0, count_from = 0;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--report=", 9)) rfd = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--fds=", 6)) fds = argv[i] + 6;
        else if (!strncmp(argv[i], "--count-open-below=", 19)) count_below = atoi(argv[i] + 19);
        else if (!strncmp(argv[i], "--count-from=", 13)) count_from = atoi(argv[i] + 13);
    }
    if (rfd < 0) return 90;
    char list[4096];
    snprintf(list, sizeof list, "%s", fds);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        int fd = atoi(tok);
        int fl = fcntl(fd, F_GETFD);
        if (fl < 0) { rep(rfd, "fd=%d closed errno=%d\n", fd, errno); continue; }
        struct stat st;
        fstat(fd, &st);
        rep(rfd, "fd=%d open fdflags=%d (cloexec=%d clofork=%d) mode=0%o\n", fd, fl, !!(fl & FD_CLOEXEC), !!(fl & FD_CLOFORK), st.st_mode & S_IFMT);
    }
    if (count_below > 0) {
        int n = 0;
        for (int fd = 0; fd < count_below; fd++) if (fcntl(fd, F_GETFD) >= 0) n++;
        rep(rfd, "open_below=%d count=%d\n", count_below, n);
        if (count_from > 0) {
            int first_closed = -1, last_open = -1, m = 0;
            for (int fd = count_from; fd < count_below; fd++) {
                if (fcntl(fd, F_GETFD) >= 0) { m++; last_open = fd; }
                else if (first_closed < 0) first_closed = fd;
            }
            rep(rfd, "range_from=%d open_in_range=%d first_closed=%d last_open=%d\n", count_from, m, first_closed, last_open);
        }
    }
    sigset_t m;
    sigprocmask(SIG_BLOCK, NULL, &m);
    rep(rfd, "sig USR1=%s USR2=%s CHLD=%s mask: USR1=%d USR2=%d TERM=%d\n", sig_state(SIGUSR1), sig_state(SIGUSR2), sig_state(SIGCHLD),
        sigismember(&m, SIGUSR1), sigismember(&m, SIGUSR2), sigismember(&m, SIGTERM));
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    mode_t um = umask(0);
    char cwd[1024];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "?");
    const char *ev = getenv("FG3_VAR");
    rep(rfd, "proc nofile_soft=%llu um=%03o cwd=%s env=%s pid=%d ppid=%d pgid=%d sid=%d argv0=%s\n", (unsigned long long)rl.rlim_cur, um, cwd, ev ? ev : "(unset)",
        getpid(), getppid(), getpgrp(), getsid(0), argv[0]);
    return 0;
}

struct shm_layout {
    uint64_t pattern[256];
    _Atomic uint32_t child_ready, parent_go;
    _Atomic uint64_t counter;
    uint64_t child_reply;
    uint64_t child_addr;
};

static int role_shm(int argc, char **argv)
{
    int rfd = -1, sfd = -1;
    size_t size = 0;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--report=", 9)) rfd = atoi(argv[i] + 9);
        else if (!strncmp(argv[i], "--shmfd=", 8)) sfd = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--size=", 7)) size = strtoull(argv[i] + 7, NULL, 0);
    }
    if (rfd < 0 || sfd < 0 || !size) return 90;
    struct shm_layout *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    if (m == MAP_FAILED) { rep(rfd, "shm mmap failed errno=%d\n", errno); return 91; }
    int bad = 0;
    for (int i = 0; i < 256; i++) if (m->pattern[i] != (0xabcd000000000000ull | (uint64_t)i)) bad++;
    rep(rfd, "shm child: mapped at %p, parent's pattern mismatches=%d\n", (void *)m, bad);
    m->child_reply = 0xfeedfacecafebeefull;
    m->child_addr = (uint64_t)(uintptr_t)m;
    atomic_store(&m->child_ready, 1);
    for (int i = 0; i < 20000 && !atomic_load(&m->parent_go); i++) usleep(250);
    if (!atomic_load(&m->parent_go)) { rep(rfd, "shm child: parent_go never set\n"); return 92; }
    for (int i = 0; i < 100000; i++) atomic_fetch_add(&m->counter, 1);
    return bad ? 1 : 0;
}

/* ---- parent helpers ------------------------------------------------------------- */
struct run { char rep[REPORT_MAX]; int status; int hung; int spawn_err; double spawn_us, total_us; };

/*
 * Spawns this binary with `role` and extra args, with a report pipe that is inherited at the same fd number.
 * fa and attr may be NULL. cloexec_default tells us to explicitly inherit the report fd.
 */
static void run_role(struct run *r, const char *role, const char *fdlist, int count_below, posix_spawn_file_actions_t *fa_in, posix_spawnattr_t *attr,
                     char *const extra[], char *const envp[])
{
    memset(r, 0, sizeof *r);
    int p[2];
    if (pipe(p)) { r->spawn_err = errno; return; }
    posix_spawn_file_actions_t fa_local;
    posix_spawn_file_actions_t *fa = fa_in;
    if (!fa) { posix_spawn_file_actions_init(&fa_local); fa = &fa_local; }
    posix_spawn_file_actions_addinherit_np(fa, p[1]);
    char a1[64], a2[4200], a3[64], a4[64];
    snprintf(a4, sizeof a4, "--count-from=%d", g_count_from);
    snprintf(a1, sizeof a1, "--report=%d", p[1]);
    snprintf(a2, sizeof a2, "--fds=%s", fdlist ? fdlist : "");
    snprintf(a3, sizeof a3, "--count-open-below=%d", count_below);
    char *argv[16];
    int n = 0;
    char rolearg[64];
    snprintf(rolearg, sizeof rolearg, "--role=%s", role);
    argv[n++] = (char *)"f3child";
    argv[n++] = rolearg;
    argv[n++] = a1;
    argv[n++] = a2;
    argv[n++] = a3;
    argv[n++] = a4;
    for (int i = 0; extra && extra[i] && n < 14; i++) argv[n++] = extra[i];
    argv[n] = NULL;
    uint64_t t0 = dp_ns();
    pid_t pid;
    int e = posix_spawn(&pid, dp_self_path(), fa, attr, argv, envp ? envp : environ);
    r->spawn_us = (double)(dp_ns() - t0) / 1e3;
    close(p[1]);
    if (!fa_in) posix_spawn_file_actions_destroy(&fa_local);
    if (e) { r->spawn_err = e; close(p[0]); return; }
    dp_track(pid);
    size_t got = 0;
    fg_set_nonblock(p[0]);
    uint64_t deadline = dp_ns() + 15000000000ull;
    for (;;) {
        ssize_t k = read(p[0], r->rep + got, REPORT_MAX - 1 - got);
        if (k > 0) { got += k; continue; }
        if (k == 0) break;
        if (errno == EAGAIN) { if (dp_ns() > deadline) { r->hung = 1; break; } usleep(300); continue; }
        break;
    }
    r->rep[got] = 0;
    close(p[0]);
    int st = 0;
    pid_t w = dp_wait_timeout(pid, &st, r->hung ? 100 : 10000);
    r->total_us = (double)(dp_ns() - t0) / 1e3;
    if (w == 0) r->hung = 1;
    r->status = st;
}

/* 1 open, 0 closed, -1 not reported; fdflags in *flags when open */
static int rep_fd(const char *rp, int fd, int *flags)
{
    char key[32];
    snprintf(key, sizeof key, "fd=%d ", fd);
    const char *q = rp;
    while ((q = strstr(q, key))) {
        if (q == rp || q[-1] == '\n') {
            if (!strncmp(q + strlen(key), "open", 4)) {
                int f = -1;
                const char *fp = strstr(q, "fdflags=");
                if (fp) f = atoi(fp + 8);
                if (flags) *flags = f;
                return 1;
            }
            return 0;
        }
        q += strlen(key);
    }
    return -1;
}

static void print_report(const char *id, const char *tag, const struct run *r)
{
    const char *s = r->rep;
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        INFO(id, "%s | %.*s", tag, (int)n, s);
        if (!e) break;
        s = e + 1;
    }
}

static const char *st3(int v) { return v == 1 ? "open" : v == 0 ? "closed" : "not-reported"; }

/* ---- tests ------------------------------------------------------------------------ */
static void t1_default_spawn(void)
{
    int pr[2];
    pipe(pr);
    int b = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int c = open("/dev/null", O_RDONLY | O_CLOFORK);
    int d = open("/dev/null", O_RDONLY | O_CLOEXEC | O_CLOFORK);
    int e = open("/dev/null", O_RDONLY);
    char fl[128];
    snprintf(fl, sizeof fl, "0,1,2,%d,%d,%d,%d,%d", pr[0], b, c, d, e);
    struct run r;
    run_role(&r, "report", fl, 0, NULL, NULL, NULL, NULL);
    CHECK(!r.spawn_err && !r.hung, "F3.t1", "default posix_spawn of this binary (spawn err=%s, hung=%d)", ERRS(r.spawn_err), r.hung);
    int s0 = rep_fd(r.rep, 0, NULL), s1 = rep_fd(r.rep, 1, NULL), s2 = rep_fd(r.rep, 2, NULL);
    int sp = rep_fd(r.rep, pr[0], NULL), sb = rep_fd(r.rep, b, NULL), sc = rep_fd(r.rep, c, NULL), sd = rep_fd(r.rep, d, NULL), se = rep_fd(r.rep, e, NULL);
    CHECK(s0 == 1 && s1 == 1 && s2 == 1, "F3.t1", "default mode: stdio 0,1,2 inherited (%s %s %s)", st3(s0), st3(s1), st3(s2));
    CHECK(sp == 1 && se == 1, "F3.t1", "default mode: plain fds are inherited (pipe end %s, plain file %s)", st3(sp), st3(se));
    CHECK(sb == 0, "F3.t1", "default mode: FD_CLOEXEC fd is closed in the child (%s)", st3(sb));
    CHECK(sd == 0, "F3.t1", "default mode: FD_CLOEXEC|FD_CLOFORK fd is closed in the child (%s)", st3(sd));
    INFO("F3.t1", "default mode: an fd with only FD_CLOFORK (fork-time close flag) is %s in a posix_spawn child", st3(sc));
    print_report("F3.t1", "child", &r);
    close(pr[0]); close(pr[1]); close(b); close(c); close(d); close(e);
}

static void t2_cloexec_default(void)
{
    int pr[2];
    pipe(pr);
    int b = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int e = open("/dev/null", O_RDONLY);
    int e2 = open("/dev/null", O_RDONLY);
    int f = open("/dev/null", O_RDONLY);
    int g = open("/dev/null", O_RDONLY);
    int h = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int h2 = open("/dev/null", O_RDONLY | O_CLOEXEC);
    char fl[256];
    snprintf(fl, sizeof fl, "0,1,2,%d,%d,%d,%d,%d,%d,%d,%d,20", pr[0], b, e, e2, f, g, h, h2);

    /* T2a: CLOEXEC_DEFAULT and nothing else */
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    struct run r;
    run_role(&r, "report", fl, 0, NULL, &at, NULL, NULL);
    CHECK(!r.spawn_err && !r.hung, "F3.t2a", "POSIX_SPAWN_CLOEXEC_DEFAULT spawn (err=%s)", ERRS(r.spawn_err));
    int all_closed = 1;
    int ids[] = { 0, 1, 2, pr[0], b, e, e2, f, g, h, h2 };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) if (rep_fd(r.rep, ids[i], NULL) != 0) all_closed = 0;
    CHECK(all_closed, "F3.t2a", "CLOEXEC_DEFAULT with no file action: every fd except the report pipe is closed, stdio 0,1,2 included");
    print_report("F3.t2a", "child", &r);
    posix_spawnattr_destroy(&at);

    /* T2b: CLOEXEC_DEFAULT + addinherit_np / adddup2 / nothing */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addinherit_np(&fa, 0);
    posix_spawn_file_actions_addinherit_np(&fa, 1);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
    posix_spawn_file_actions_addinherit_np(&fa, e);        /* plain fd */
    posix_spawn_file_actions_addinherit_np(&fa, b);        /* fd with FD_CLOEXEC set */
    posix_spawn_file_actions_adddup2(&fa, f, 20);          /* dup2 to a new number */
    posix_spawn_file_actions_adddup2(&fa, h, h);           /* dup2 onto itself, fd has FD_CLOEXEC */
    posix_spawn_file_actions_addinherit_np(&fa, h2);
    posix_spawn_file_actions_addclose(&fa, h2);            /* inherit then close */
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    run_role(&r, "report", fl, 0, &fa, &at, NULL, NULL);
    CHECK(!r.spawn_err && !r.hung, "F3.t2b", "CLOEXEC_DEFAULT with file actions (err=%s)", ERRS(r.spawn_err));
    int fe, fb, fh, f20;
    int se2 = rep_fd(r.rep, e, &fe), sb2 = rep_fd(r.rep, b, &fb), sh2 = rep_fd(r.rep, h, &fh), s20 = rep_fd(r.rep, 20, &f20);
    CHECK(rep_fd(r.rep, 0, NULL) == 1 && rep_fd(r.rep, 1, NULL) == 1 && rep_fd(r.rep, 2, NULL) == 1, "F3.t2b", "addinherit_np(0,1,2) keeps stdio");
    CHECK(se2 == 1, "F3.t2b", "addinherit_np(plain fd) keeps it (%s, fdflags=%d)", st3(se2), fe);
    CHECK(sb2 == 1, "F3.t2b", "addinherit_np on an fd that has FD_CLOEXEC keeps it open in the child (%s); FD_CLOEXEC in the child = %d", st3(sb2), (fb >= 0) ? !!(fb & FD_CLOEXEC) : -1);
    CHECK(s20 == 1, "F3.t2b", "adddup2(f, 20) creates fd 20 in the child (%s, fdflags=%d)", st3(s20), f20);
    CHECK(rep_fd(r.rep, f, NULL) == 0, "F3.t2b", "the source of the dup2 (f=%d) is closed in the child under CLOEXEC_DEFAULT", f);
    CHECK(sh2 == 1, "F3.t2b", "adddup2(h, h) on an FD_CLOEXEC fd keeps it open (%s); FD_CLOEXEC in the child = %d (POSIX says the flag is cleared)", st3(sh2), (fh >= 0) ? !!(fh & FD_CLOEXEC) : -1);
    CHECK(rep_fd(r.rep, g, NULL) == 0, "F3.t2b", "an fd named by no action is closed (g=%d)", g);
    CHECK(rep_fd(r.rep, h2, NULL) == 0, "F3.t2b", "addinherit_np followed by addclose leaves the fd closed (h2=%d)", h2);
    print_report("F3.t2b", "child", &r);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(pr[0]); close(pr[1]); close(b); close(e); close(e2); close(f); close(g); close(h); close(h2);
}

static void t2c_default_mode_dup2_self(void)
{
    int pr[2];
    pipe(pr);
    int h = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int h2 = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int h3 = open("/dev/null", O_RDONLY | O_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, h, h);
    posix_spawn_file_actions_addinherit_np(&fa, h2);
    char fl[128];
    snprintf(fl, sizeof fl, "%d,%d,%d", h, h2, h3);
    struct run r;
    run_role(&r, "report", fl, 0, &fa, NULL, NULL, NULL);
    int fh, fh2;
    int sh = rep_fd(r.rep, h, &fh), sh2 = rep_fd(r.rep, h2, &fh2), sh3 = rep_fd(r.rep, h3, NULL);
    INFO("F3.t2c", "default mode (no CLOEXEC_DEFAULT), parent fds all FD_CLOEXEC: adddup2(h,h) -> %s fdflags=%d ; addinherit_np(h2) -> %s fdflags=%d ; no action (h3) -> %s",
         st3(sh), fh, st3(sh2), fh2, st3(sh3));
    CHECK(sh == 1 && !(fh & FD_CLOEXEC), "F3.t2c", "in default mode adddup2(fd, fd) is the POSIX way to keep an FD_CLOEXEC fd across spawn (flag cleared)");
    posix_spawn_file_actions_destroy(&fa);
    close(pr[0]); close(pr[1]); close(h); close(h2); close(h3);
}

static void t3b_action_cap(void)
{
    /* the 20000-action run above kept only about 10040 inherited fds: find where it stops */
    int base = 200;
    int top = base + 12500;
    int zero = open("/dev/null", O_RDONLY);
    for (int fd = base; fd < top; fd++) {
        int r = fcntl(zero, F_DUPFD, fd);
        if (r != fd) { INFO("F3.t3b", "cannot create fd %d (%d, %s)", fd, r, ERRS(errno)); return; }
    }
    int counts[] = { 1000, 5000, 10000, 10039, 10040, 10041, 10100, 10240, 12000, 12300 };
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        int want = counts[k];
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        for (int i = 0; i < want; i++) posix_spawn_file_actions_addinherit_np(&fa, base + i);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
        struct run r;
        g_count_from = base;
        run_role(&r, "report", "", top + 10, &fa, &at, NULL, NULL);
        g_count_from = 0;
        const char *q = strstr(r.rep, "range_from=");
        INFO("F3.t3b", "CLOEXEC_DEFAULT with %5d addinherit_np actions (fds %d..%d): spawn=%s | child: %.*s", want, base, base + want - 1,
             r.spawn_err ? ERRS(r.spawn_err) : "ok", q ? (int)strcspn(q, "\n") : 4, q ? q : "none");
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
    }
    for (int fd = base; fd < top; fd++) close(fd);
    close(zero);
}

static void t3c_high_fd_workaround(void)
{
    /* fds numbered >= 10240 cannot be inherited under CLOEXEC_DEFAULT; what else works? */
    int zero = open("/dev/null", O_RDONLY);
    int hi[4];
    for (int i = 0; i < 4; i++) {
        hi[i] = fcntl(zero, F_DUPFD, 10300 + i);
        if (hi[i] != 10300 + i) { INFO("F3.t3c", "cannot create fd %d (%d, %s)", 10300 + i, hi[i], ERRS(errno)); close(zero); return; }
    }
    fcntl(hi[3], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addinherit_np(&fa, hi[0]);
    posix_spawn_file_actions_adddup2(&fa, hi[1], 300);
    posix_spawn_file_actions_adddup2(&fa, hi[2], 301);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    struct run r;
    run_role(&r, "report", "300,301,10300,10301,10302,10303", 0, &fa, &at, NULL, NULL);
    INFO("F3.t3c", "CLOEXEC_DEFAULT, parent fds 10300..10303: addinherit_np(10300) -> %s ; adddup2(10301 -> 300) -> fd 300 %s ; adddup2(10302 -> 301) -> fd 301 %s",
         st3(rep_fd(r.rep, 10300, NULL)), st3(rep_fd(r.rep, 300, NULL)), st3(rep_fd(r.rep, 301, NULL)));
    CHECK(rep_fd(r.rep, 10300, NULL) == 0 && rep_fd(r.rep, 300, NULL) == 0, "F3.t3c",
          "under CLOEXEC_DEFAULT neither addinherit_np nor adddup2 can transfer a descriptor numbered above 10239 (it is simply absent in the child)");
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);

    /* default mode: no attribute, high fds without FD_CLOEXEC are inherited, with FD_CLOEXEC are not, adddup2 high->low */
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, hi[1], 300);
    run_role(&r, "report", "300,10300,10301,10302,10303", 0, &fa, NULL, NULL, NULL);
    int d0 = rep_fd(r.rep, 10300, NULL), d300 = rep_fd(r.rep, 300, NULL), d3 = rep_fd(r.rep, 10303, NULL);
    INFO("F3.t3c", "default mode: fd 10300 without FD_CLOEXEC -> %s ; adddup2(10301 -> 300) -> fd 300 %s ; fd 10303 with FD_CLOEXEC -> %s", st3(d0), st3(d300), st3(d3));
    CHECK(d0 == 1 && d3 == 0, "F3.t3c", "default mode: a high fd is inherited unless it carries FD_CLOEXEC, whatever its number (workaround: flag every internal fd FD_CLOEXEC and do not use CLOEXEC_DEFAULT)");
    posix_spawn_file_actions_destroy(&fa);
    for (int i = 0; i < 4; i++) close(hi[i]);
    close(zero);
}

static void t3_many_actions(void)
{
    /* dup /dev/null to fds 200.. and inherit them all */
    int base = 200;
    int counts[] = { 0, 10, 100, 1000, 4000, 20000 };
    int zero = open("/dev/null", O_RDONLY);
    int opened = 0;
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        int want = counts[k];
        while (opened < want) {
            int fd = fcntl(zero, F_DUPFD, base + opened);
            if (fd != base + opened) { INFO("F3.t3", "cannot create fd %d (got %d, %s)", base + opened, fd, ERRS(errno)); break; }
            opened++;
        }
        if (opened < want) break;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        for (int i = 0; i < want; i++) posix_spawn_file_actions_addinherit_np(&fa, base + i);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
        struct run r;
        char lst[64];
        snprintf(lst, sizeof lst, "%d,%d", base, base + (want ? want - 1 : 0));
        int below = base + want + 10;
        run_role(&r, "report", lst, below, &fa, &at, NULL, NULL);
        const char *cnt = strstr(r.rep, "count=");
        int c = cnt ? atoi(cnt + 6) : -1;
        INFO("F3.t3", "CLOEXEC_DEFAULT + %5d addinherit_np actions: spawn=%s, call %.0f us, total %.0f us, child sees %d open fds below %d (report pipe + %d expected)",
             want, r.spawn_err ? ERRS(r.spawn_err) : "ok", r.spawn_us, r.total_us, c, below, want);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        /* the same number of open fds without CLOEXEC_DEFAULT (everything inherited by default) */
        run_role(&r, "report", lst, below, NULL, NULL, NULL, NULL);
        cnt = strstr(r.rep, "count=");
        c = cnt ? atoi(cnt + 6) : -1;
        INFO("F3.t3", "default mode with %5d extra open fds: call %.0f us, total %.0f us, child sees %d open fds below %d", want, r.spawn_us, r.total_us, c, below);
    }
    for (int i = 0; i < opened; i++) close(base + i);
    close(zero);
}

static void t4_shared_memory(const char *kind)
{
    int fd = -1;
    char *tmp = NULL;
    if (!strcmp(kind, "shm_open")) {
        char name[32];
        snprintf(name, sizeof name, "/fg3-%d", (int)getpid());
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) { INFO("F3.t4", "%s: shm_open failed %s", kind, ERRS(errno)); return; }
        shm_unlink(name);
    } else {
        tmp = fg_tmpname("f3shm");
        fd = mkstemp(tmp);
        if (fd < 0) { INFO("F3.t4", "%s: mkstemp failed %s", kind, ERRS(errno)); return; }
        unlink(tmp);
    }
    size_t size = 4 * FG_PAGE;
    if (ftruncate(fd, size)) { INFO("F3.t4", "%s: ftruncate failed %s", kind, ERRS(errno)); close(fd); return; }
    struct shm_layout *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { INFO("F3.t4", "%s: mmap failed %s", kind, ERRS(errno)); close(fd); return; }
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 256; i++) m->pattern[i] = 0xabcd000000000000ull | (uint64_t)i;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addinherit_np(&fa, fd);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    char a1[32], a2[48];
    snprintf(a1, sizeof a1, "--shmfd=%d", fd);
    snprintf(a2, sizeof a2, "--size=%zu", size);
    char *extra[] = { a1, a2, NULL };

    /* run in a helper thread-less way: spawn, then drive the protocol while the child lives */
    int p[2];
    pipe(p);
    posix_spawn_file_actions_addinherit_np(&fa, p[1]);
    char a3[32];
    snprintf(a3, sizeof a3, "--report=%d", p[1]);
    char *argv[] = { "f3child", "--role=shm", a3, a1, a2, NULL };
    (void)extra;
    pid_t pid;
    int e = posix_spawn(&pid, dp_self_path(), &fa, &at, argv, environ);
    close(p[1]);
    if (e) { INFO("F3.t4", "%s: spawn failed %s", kind, ERRS(e)); return; }
    dp_track(pid);
    for (int i = 0; i < 20000 && !atomic_load(&m->child_ready); i++) usleep(250);
    CHECK(atomic_load(&m->child_ready) == 1, "F3.t4", "%s: spawned child mapped the inherited fd and signalled ready", kind);
    CHECK(m->child_reply == 0xfeedfacecafebeefull, "F3.t4", "%s: the parent sees the spawned child's write through MAP_SHARED (0x%llx)", kind, (unsigned long long)m->child_reply);
    atomic_store(&m->parent_go, 1);
    for (int i = 0; i < 100000; i++) atomic_fetch_add(&m->counter, 1);
    int st = 0;
    dp_wait_timeout(pid, &st, 10000);
    char buf[1024] = { 0 };
    ssize_t n = read(p[0], buf, sizeof buf - 1);
    (void)n;
    close(p[0]);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "F3.t4", "%s: spawned child exited 0 (the parent's pattern was intact in its view)", kind);
    CHECK(atomic_load(&m->counter) == 200000, "F3.t4", "%s: atomics from two processes mapped at different addresses (parent %p, child 0x%llx) sum to %llu", kind, (void *)m,
          (unsigned long long)m->child_addr, (unsigned long long)atomic_load(&m->counter));
    for (char *s = buf, *q; s && *s; s = q ? q + 1 : NULL) {
        q = strchr(s, '\n');
        if (q) *q = 0;
        INFO("F3.t4", "%s: child report: %s", kind, s);
        if (!q) break;
    }
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    munmap(m, size);
    close(fd);
    free(tmp);
}

static void usr2_noop(int s) {}

static void t5_process_attrs(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = usr2_noop;
    sigaction(SIGUSR2, &sa, NULL);
    sigset_t blk, old;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigaddset(&blk, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &blk, &old);
    mode_t oldum = umask(027);
    char cwdold[1024];
    getcwd(cwdold, sizeof cwdold);
    chdir("/private/tmp");
    struct rlimit rl, rlold;
    getrlimit(RLIMIT_NOFILE, &rlold);
    rl = rlold;
    rl.rlim_cur = 5000;
    setrlimit(RLIMIT_NOFILE, &rl);
    char *envp[] = { "FG3_VAR=hello", "PATH=/usr/bin:/bin", NULL };

    struct run r;
    run_role(&r, "report", "", 0, NULL, NULL, NULL, envp);
    const char *s = strstr(r.rep, "sig USR1=");
    INFO("F3.t5", "default spawn, parent had SIGUSR1 ignored, SIGUSR2 handled, SIGUSR1+SIGTERM blocked, umask 027, cwd /private/tmp, RLIMIT_NOFILE soft 5000, explicit env:");
    print_report("F3.t5", "child", &r);
    CHECK(s && strstr(s, "USR1=ignored") && strstr(s, "USR2=default"), "F3.t5", "ignored signals stay ignored across spawn, handlers reset to default");
    CHECK(s && strstr(s, "mask: USR1=1 USR2=0 TERM=1"), "F3.t5", "the signal mask is inherited");
    CHECK(strstr(r.rep, "um=027") && strstr(r.rep, "cwd=/private/tmp") && strstr(r.rep, "env=hello") && strstr(r.rep, "nofile_soft=5000"), "F3.t5", "umask, cwd, explicit env and RLIMIT_NOFILE soft limit follow the spawn");

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t empty, all;
    sigemptyset(&empty);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &empty);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    run_role(&r, "report", "", 0, NULL, &at, NULL, envp);
    s = strstr(r.rep, "sig USR1=");
    CHECK(s && strstr(s, "USR1=default") && strstr(s, "mask: USR1=0 USR2=0 TERM=0"), "F3.t5", "SETSIGMASK(empty)+SETSIGDEF(all) resets mask and dispositions");
    posix_spawnattr_destroy(&at);

    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    run_role(&r, "report", "", 0, NULL, &at, NULL, envp);
    const char *pp = strstr(r.rep, "pgid=");
    int pg = pp ? atoi(pp + 5) : -1, cp = -1;
    const char *pi = strstr(r.rep, " pid=");
    if (pi) cp = atoi(pi + 5);
    CHECK(pg > 0 && pg == cp, "F3.t5", "POSIX_SPAWN_SETPGROUP with pgroup 0 makes the child its own group leader (pgid=%d pid=%d)", pg, cp);
    posix_spawnattr_destroy(&at);

    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
    run_role(&r, "report", "", 0, NULL, &at, NULL, envp);
    const char *ps = strstr(r.rep, "sid=");
    int sid = ps ? atoi(ps + 4) : -1;
    pi = strstr(r.rep, " pid=");
    cp = pi ? atoi(pi + 5) : -1;
    CHECK(sid > 0 && sid == cp, "F3.t5", "POSIX_SPAWN_SETSID makes the child a session leader (sid=%d pid=%d)", sid, cp);
    posix_spawnattr_destroy(&at);

    setrlimit(RLIMIT_NOFILE, &rlold);
    chdir(cwdold);
    umask(oldum);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, NULL);
}

static void t6_errors(void)
{
    char *dir = fg_tmpname("f3err");
    mkdir(dir, 0700);
    char path[1200];
    char *argv[] = { "x", NULL };
    pid_t pid;
    int e;
    struct { const char *what; const char *content; mode_t mode; } cases[] = {
        { "text file with exec bit and no #!", "echo hi\n", 0700 },
        { "ELF header file with exec bit", "\x7f" "ELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x03\0\x3e\0", 0700 },
        { "script whose #! interpreter does not exist", "#!/nonexistent/interp\n", 0700 },
        { "regular file without exec bit", "#!/bin/sh\n", 0600 },
        { "empty file with exec bit", "", 0700 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(path, sizeof path, "%s/case%zu", dir, i);
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, cases[i].mode);
        if (fd >= 0) { ssize_t w = write(fd, cases[i].content, strlen(cases[i].content) + (i == 1 ? 12 : 0)); (void)w; close(fd); chmod(path, cases[i].mode); }
        e = posix_spawn(&pid, path, NULL, NULL, argv, environ);
        if (e == 0) { int st; dp_track(pid); dp_wait_timeout(pid, &st, 5000); INFO("F3.t6", "posix_spawn of a %s: returned 0 (child status 0x%x)", cases[i].what, st); }
        else INFO("F3.t6", "posix_spawn of a %s: %s", cases[i].what, ERRS(e));
        unlink(path);
    }
    e = posix_spawn(&pid, "/nonexistent/prog", NULL, NULL, argv, environ);
    INFO("F3.t6", "posix_spawn of a missing path: %s", ERRS(e));
    e = posix_spawn(&pid, dir, NULL, NULL, argv, environ);
    INFO("F3.t6", "posix_spawn of a directory: %s", ERRS(e));
    char *argv2[] = { "x", "-c", "true", NULL };
    e = posix_spawn(&pid, "/bin/sh", NULL, NULL, argv2, environ);
    if (e == 0) { int st; dp_track(pid); dp_wait_timeout(pid, &st, 5000); }
    INFO("F3.t6", "(control) posix_spawn of /bin/sh -c true with argv[0]=x: %s", ERRS(e));
    rmdir(dir);
    free(dir);
}

static void t7_cost(void)
{
    int variants = 3;
    const char *names[] = { "default attributes", "CLOEXEC_DEFAULT, stdio inherited", "CLOEXEC_DEFAULT + 100 inherited fds" };
    int zero = open("/dev/null", O_RDONLY);
    for (int v = 0; v < variants; v++) {
        uint64_t calls[40], totals[40];
        int n = 30;
        int extra_fds[100];
        if (v == 2) for (int i = 0; i < 100; i++) extra_fds[i] = dup(zero);
        for (int i = 0; i < n; i++) {
            posix_spawn_file_actions_t fa;
            posix_spawn_file_actions_init(&fa);
            posix_spawnattr_t at;
            posix_spawnattr_init(&at);
            if (v >= 1) {
                posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
                for (int k = 0; k < 3; k++) posix_spawn_file_actions_addinherit_np(&fa, k);
            }
            if (v == 2) for (int k = 0; k < 100; k++) posix_spawn_file_actions_addinherit_np(&fa, extra_fds[k]);
            char *argv[] = { "f3child", "--role=exit0", NULL };
            uint64_t t0 = dp_ns();
            pid_t pid;
            int e = posix_spawn(&pid, dp_self_path(), &fa, &at, argv, environ);
            uint64_t t1 = dp_ns();
            if (e) { INFO("F3.t7", "spawn failed %s", ERRS(e)); break; }
            dp_track(pid);
            int st;
            waitpid(pid, &st, 0);
            calls[i] = t1 - t0;
            totals[i] = dp_ns() - t0;
            posix_spawn_file_actions_destroy(&fa);
            posix_spawnattr_destroy(&at);
        }
        dp_stats sc = dp_stat_u64(calls, n), st2 = dp_stat_u64(totals, n);
        INFO("F3.t7", "spawn + reap of this binary (role exit0), %s: call p50=%.0f us, total p50=%.0f us p90=%.0f us max=%.0f us (n=%d)", names[v], sc.p50 / 1e3, st2.p50 / 1e3, st2.p90 / 1e3, st2.max / 1e3, n);
        if (v == 2) for (int i = 0; i < 100; i++) close(extra_fds[i]);
    }
    close(zero);
}

static void t8_start_suspended(void)
{
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_START_SUSPENDED);
    char *argv[] = { "f3child", "--role=exit0", NULL };
    pid_t pid;
    int e = posix_spawn(&pid, dp_self_path(), NULL, &at, argv, environ);
    if (e) { INFO("F3.t8", "START_SUSPENDED spawn failed %s", ERRS(e)); return; }
    dp_track(pid);
    int st = 0;
    pid_t w = waitpid(pid, &st, WUNTRACED);
    char b[96];
    INFO("F3.t8", "POSIX_SPAWN_START_SUSPENDED: waitpid(WUNTRACED) returned %d, status %s", w, dp_status_str(st, b, sizeof b));
    CHECK(w == pid && WIFSTOPPED(st), "F3.t8", "the child starts stopped, before running any of its code");
    kill(pid, SIGCONT);
    dp_wait_timeout(pid, &st, 5000);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "F3.t8", "after SIGCONT the child runs and exits 0 (%s)", dp_status_str(st, b, sizeof b));
}

static int clofork_child(void *arg)
{
    int *fds = arg;
    int c = fcntl(fds[0], F_GETFD), d = fcntl(fds[1], F_GETFD), p = fcntl(fds[2], F_GETFD);
    dp_emit("INFO", "F3.t9", "in the fork child: fd with FD_CLOFORK -> %s, fd with FD_CLOFORK|FD_CLOEXEC -> %s, plain fd -> %s",
            c < 0 ? "closed" : "open", d < 0 ? "closed" : "open", p < 0 ? "closed" : "open");
    return (c < 0 && d < 0 && p >= 0) ? 0 : 1;
}

static void t9_clofork_fork(void)
{
    int c = open("/dev/null", O_RDONLY | O_CLOFORK);
    int d = open("/dev/null", O_RDONLY | O_CLOFORK | O_CLOEXEC);
    int p = open("/dev/null", O_RDONLY);
    int fds[3] = { c, d, p };
    char b[96];
    int st = 0;
    int rc = fg_run_in_child(clofork_child, fds, 5000, &st);
    CHECK(rc == FG_OK, "F3.t9", "fork() closes O_CLOFORK descriptors in the child and keeps the others (%s)", fg_outcome_str(rc, st, b, sizeof b));
    int fl = fcntl(c, F_GETFD);
    int dup_plain = dup(c);
    int dup_cf = fcntl(c, F_DUPFD_CLOFORK, 50);
    INFO("F3.t9", "FD_CLOFORK flag on the fd: %d ; after dup() the copy has fdflags=%d ; F_DUPFD_CLOFORK copy has fdflags=%d", fl, fcntl(dup_plain, F_GETFD), fcntl(dup_cf, F_GETFD));
    close(c); close(d); close(p); close(dup_plain); close(dup_cf);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--role=report")) return role_report(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--role=shm")) return role_shm(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--role=exit0")) return 0;
    dp_watchdog(200);
    fg_load_info("F3.env");
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    INFO("F3.env", "RLIMIT_NOFILE soft=%llu hard=%llu", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
    t1_default_spawn();
    t2_cloexec_default();
    t2c_default_mode_dup2_self();
    t3_many_actions();
    t3b_action_cap();
    t3c_high_fd_workaround();
    t4_shared_memory("shm_open");
    t4_shared_memory("mkstemp");
    t5_process_attrs();
    t6_errors();
    t7_cost();
    t8_start_suspended();
    t9_clofork_fork();
    dp_summary("f3_spawn_fd");
    return 0;
}
