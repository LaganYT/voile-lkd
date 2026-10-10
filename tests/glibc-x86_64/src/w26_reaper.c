/* w26_reaper: orphans and their new parent, as Linux gives them (kernel/exit.c v6.12: forget_original_parent,
 * find_new_reaper, reparent_leader).
 * 1. Wine's double fork (ntdll/unix/process.c, spawn_process): the grandchild of the witness is an orphan once the
 *    middle process exits. With no subreaper it goes to the family's init, the engine's first process: this witness.
 *    getppid() and /proc/<pid>/status show it at once, even before the orphan makes a syscall; the new parent's
 *    /proc/<ppid>/{comm,exe,cmdline,status} are readable (what EAC reads); the witness gets SIGCHLD and reaps it.
 * 2. PR_SET_CHILD_SUBREAPER in a child S (not inherited by its children): the orphans of S's grandchildren go to S,
 *    which waits for one of them. PR_SET_PDEATHSIG in the other: the signal comes from its parent when it dies
 *    (SI_USER, that parent's pid), and again when S dies; it then belongs to the witness, which reaps it.
 * Prints W26 lines, then W26 PASS or W26 FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("W26 FAIL " __VA_ARGS__); printf("\n"); fflush(stdout); fails++; } } while (0)

struct rec { int tag, v[7]; };
enum { R_ORPHAN = 1, R_MID2, R_G1, R_S, R_G2 };

static void put(int fd, int tag, int a, int b, int c, int d, int e, int f, int g)
{
    struct rec r = { tag, { a, b, c, d, e, f, g } };
    (void)!write(fd, &r, sizeof r);
}

static int get(int fd, struct rec *r)
{
    return read(fd, r, sizeof *r) == (ssize_t)sizeof *r;
}

static ssize_t read_file(const char *path, char *buf, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    ssize_t r = read(fd, buf, n);
    int e = errno;
    close(fd);
    return r < 0 ? -e : r;
}

static int ppid_in_status(int pid)
{
    char path[64], buf[2048];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    ssize_t n = read_file(path, buf, sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "\nPPid:\t");
    return p ? atoi(p + 7) : -1;
}

/* EAC's reads of its parent (eac-probes-digest, table 2.2): comm, exe, cmdline and status (31 bytes each). Returns
 * the number of entries that were readable, comm in *comm. */
static int parent_entries(int ppid, char *comm, size_t n)
{
    char path[64], buf[512];
    int ok = 0;
    snprintf(path, sizeof path, "/proc/%d/comm", ppid);
    ssize_t r = read_file(path, buf, 31);
    if (r > 0) { ok++; buf[r] = 0; buf[strcspn(buf, "\n")] = 0; snprintf(comm, n, "%s", buf); }
    snprintf(path, sizeof path, "/proc/%d/exe", ppid);
    if (readlink(path, buf, sizeof buf) > 0) ok++;
    snprintf(path, sizeof path, "/proc/%d/cmdline", ppid);
    if (read_file(path, buf, 31) > 0) ok++;
    snprintf(path, sizeof path, "/proc/%d/status", ppid);
    if (read_file(path, buf, 31) > 0) ok++;
    return ok;
}

/* getppid() once it is no longer `old`, within 4 s. */
static int ppid_after(int old)
{
    int p = getppid();
    for (int i = 0; i < 400 && p == old; i++) {
        usleep(10000);
        p = getppid();
    }
    return p;
}

static int take(int sig, siginfo_t *si)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    struct timespec ts = { 3, 0 };
    memset(si, 0, sizeof *si);
    return sigtimedwait(&s, si, &ts);
}

static void block(int sig)
{
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, sig);
    sigprocmask(SIG_BLOCK, &s, NULL);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int me = getpid();
    char buf[256];
    printf("W26 witness pid %d, parent %d\n", me, getppid());
    ssize_t n1 = read_file("/proc/1/status", buf, sizeof buf);
    printf("W26 /proc/1/status: %s\n", n1 > 0 ? "readable" : strerror((int)-n1));
    block(SIGCHLD);
    siginfo_t si;
    int st;

    /* ---- 1. double fork, orphan to the family's init ---- */
    int rp[2], mp[2], go[2];
    if (pipe(rp) || pipe(mp) || pipe(go)) return 2;
    pid_t mid = fork();
    if (mid == 0) {
        pid_t g = fork();
        if (g == 0) {
            int first = getppid();
            (void)!write(mp[1], &first, sizeof first);
            char c;
            (void)!read(go[0], &c, 1);                  /* the witness has read /proc/<us>/status */
            int pp = getppid(), self = ppid_in_status(getpid());
            char comm[32] = "";
            int readable = parent_entries(pp, comm, sizeof comm);
            put(rp[1], R_ORPHAN, pp, self, readable, comm[0], comm[1], 0, 0);
            _exit(9);
        }
        int first = -1;
        (void)!read(mp[0], &first, sizeof first);
        put(rp[1], R_MID2, g, first, 0, 0, 0, 0, 0);
        _exit(0);
    }
    struct rec r;
    CHECK(get(rp[0], &r) && r.tag == R_MID2, "no report from the middle process");
    pid_t g = r.v[0];
    CHECK(r.v[1] == mid, "orphan's first parent %d, middle process %d", r.v[1], mid);
    CHECK(waitpid(mid, &st, 0) == mid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "wait for the middle process");
    CHECK(take(SIGCHLD, &si) == SIGCHLD && si.si_pid == mid, "SIGCHLD of the middle process (pid %d)", si.si_pid);
    int seen = ppid_in_status(g);
    printf("W26 double-fork: orphan %d, first parent %d (exited), /proc/%d/status PPid %d\n", g, mid, g, seen);
    CHECK(seen == me, "/proc/%d/status PPid %d, want %d", g, seen, me);
    (void)!write(go[1], "g", 1);
    CHECK(get(rp[0], &r) && r.tag == R_ORPHAN, "no report from the orphan");
    printf("W26 orphan: getppid %d, own status PPid %d, parent entries readable %d/4, parent comm %c%c...\n", r.v[0],
           r.v[1], r.v[2], r.v[3] ? r.v[3] : '?', r.v[4] ? r.v[4] : '?');
    CHECK(r.v[0] == me, "orphan's getppid %d, want %d", r.v[0], me);
    CHECK(r.v[1] == me, "orphan's own PPid %d, want %d", r.v[1], me);
    CHECK(r.v[2] == 4, "%d of the parent's comm, exe, cmdline, status readable", r.v[2]);
    int got = take(SIGCHLD, &si);
    printf("W26 init: SIGCHLD %d from %d code %d status %d\n", got, si.si_pid, si.si_code, si.si_status);
    CHECK(got == SIGCHLD && si.si_pid == g && si.si_code == CLD_EXITED && si.si_status == 9, "SIGCHLD of the orphan");
    pid_t w = waitpid(-1, &st, 0);
    CHECK(w == g && WIFEXITED(st) && WEXITSTATUS(st) == 9, "wait for the orphan: %d status 0x%x", w, st);
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "no child left");

    /* ---- 2. subreaper and parent-death signal ---- */
    int armed[2], die[2];
    if (pipe(armed) || pipe(die)) return 2;
    pid_t s = fork();
    if (s == 0) {
        int sr = -1;
        prctl(PR_SET_CHILD_SUBREAPER, 1);
        prctl(PR_GET_CHILD_SUBREAPER, &sr);
        int spid = getpid();
        pid_t m = fork();
        if (m == 0) {
            int inherited = -1;
            prctl(PR_GET_CHILD_SUBREAPER, &inherited);
            int mpid = getpid();
            pid_t g1 = fork();
            if (g1 == 0) {
                int pp = ppid_after(mpid);
                put(rp[1], R_G1, pp, 0, 0, 0, 0, 0, 0);
                _exit(5);
            }
            pid_t g2 = fork();
            if (g2 == 0) {
                block(SIGUSR1);
                int pd = -1;
                prctl(PR_SET_PDEATHSIG, SIGUSR1);
                prctl(PR_GET_PDEATHSIG, &pd);
                (void)!write(armed[1], &pd, sizeof pd);
                int pp1 = ppid_after(mpid);
                siginfo_t a, b;
                int s1 = take(SIGUSR1, &a);
                (void)!write(die[1], "d", 1);            /* S may die now */
                int pp2 = ppid_after(spid);
                int s2 = take(SIGUSR1, &b);
                put(rp[1], R_G2, pd, pp1, s1, a.si_pid == mpid, a.si_code, pp2, s2 == SIGUSR1 ? b.si_pid : -1);
                _exit(4);
            }
            int pd = -1;
            (void)!read(armed[0], &pd, sizeof pd);
            (void)inherited;
            _exit(inherited == 0 ? 0 : 1);               /* both grandchildren are orphans now */
        }
        int mst = -1, g1st = -1, alive = -1;
        pid_t wm = waitpid(m, &mst, 0);
        pid_t w1 = waitpid(-1, &g1st, 0);                 /* the orphan g1 (g2 lives until we die) */
        alive = waitpid(-1, NULL, WNOHANG);               /* g2: our child, still running */
        put(rp[1], R_S, sr, wm == m && WIFEXITED(mst) ? WEXITSTATUS(mst) : -1, w1, WIFEXITED(g1st) ? WEXITSTATUS(g1st) : -1,
            alive, 0, 0);
        char c;
        (void)!read(die[0], &c, 1);
        _exit(0);
    }
    close(rp[1]);
    int have_g1 = 0, have_s = 0, have_g2 = 0;
    struct rec g1r = { 0 }, sr_ = { 0 }, g2r = { 0 };
    for (int i = 0; i < 3 && get(rp[0], &r); i++) {
        if (r.tag == R_G1) { g1r = r; have_g1 = 1; }
        if (r.tag == R_S) { sr_ = r; have_s = 1; }
        if (r.tag == R_G2) { g2r = r; have_g2 = 1; }
    }
    CHECK(have_g1 && have_s && have_g2, "reports: g1 %d, S %d, g2 %d", have_g1, have_s, have_g2);
    printf("W26 subreaper: S %d PR_GET %d, middle exit %d (0: flag not inherited), orphan g1 parent %d, S reaped %d "
           "with %d, S still has a child: %d\n", s, sr_.v[0], sr_.v[1], g1r.v[0], sr_.v[2], sr_.v[3], sr_.v[4]);
    CHECK(sr_.v[0] == 1, "PR_GET_CHILD_SUBREAPER %d", sr_.v[0]);
    CHECK(sr_.v[1] == 0, "the middle process saw the subreaper flag inherited");
    CHECK(g1r.v[0] == s, "orphan g1's parent %d, subreaper %d", g1r.v[0], s);
    CHECK(sr_.v[3] == 5 && sr_.v[2] > 0, "the subreaper reaped %d with %d", sr_.v[2], sr_.v[3]);
    CHECK(sr_.v[4] == 0, "waitpid(WNOHANG) in S: %d, want 0 (g2 alive)", sr_.v[4]);
    printf("W26 pdeathsig: PR_GET %d; parent died: new parent %d, signal %d from the dead parent %s, code %d; S died: "
           "new parent %d, signal from %d\n", g2r.v[0], g2r.v[1], g2r.v[2], g2r.v[3] ? "yes" : "no", g2r.v[4], g2r.v[5],
           g2r.v[6]);
    CHECK(g2r.v[0] == SIGUSR1, "PR_GET_PDEATHSIG %d", g2r.v[0]);
    CHECK(g2r.v[1] == s, "g2's parent after its parent died: %d, want S %d", g2r.v[1], s);
    CHECK(g2r.v[2] == SIGUSR1 && g2r.v[3] && g2r.v[4] == SI_USER, "first parent-death signal %d from parent %d code %d",
          g2r.v[2], g2r.v[3], g2r.v[4]);
    CHECK(g2r.v[5] == me, "g2's parent after S died: %d, want %d", g2r.v[5], me);
    CHECK(g2r.v[6] == s, "second parent-death signal from %d, want S %d", g2r.v[6], s);
    CHECK(waitpid(s, &st, 0) == s && WIFEXITED(st) && WEXITSTATUS(st) == 0, "wait for S");
    int found_g2 = 0;
    for (int i = 0; i < 2; i++) {
        w = waitpid(-1, &st, 0);
        if (w > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 4) found_g2 = 1;
    }
    CHECK(found_g2, "the witness reaped g2");
    CHECK(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD, "no child left at the end");
    if (fails) printf("W26 FAIL %d\n", fails);
    else printf("W26 PASS\n");
    return fails ? 1 : 0;
}
