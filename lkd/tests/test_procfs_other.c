/* test_procfs_other.c: /proc/<pid> of OTHER processes of the family. Two parts.
 *  1. A fake registry (lkd_procfs_set_backend): listing order, zombie records, the ptrace access verdict
 *     that decides maps, mem, environ and the exe/cwd/root/fd links, what is sent to the other process.
 *  2. Real processes: the test forks (task module fork, host fork), the parent reads the child through
 *     famsvc: status, stat, cmdline, maps, fd links, a reopened memfd received as a host descriptor,
 *     /proc/<pid>/mem read and write, TracerPid, a zombie before wait4.
 * Linux models (v6.12): fs/proc/base.c proc_pid_readdir / proc_pid_lookup, proc_fd_access_allowed,
 * proc_mem_open (mm_access -> -EACCES), proc_pid_cmdline_ops; fs/proc/array.c (zombie status). */
#include <sys/wait.h>

#include "test_procfs_util.h"
#include "../src/procfs/procfs_priv.h"
#include "../src/mm/lkd_mm_ext.h"

/* ---- Part 1: fake registry --------------------------------------------------------------------- */

struct fproc { lkd_proc_info pi; int nthreads; int32_t tids[3]; };
static struct fproc g_fake[3];
static int g_access_calls, g_access_last_pid;
static unsigned g_access_last_mode;
static int g_access_verdict;
static struct lkd_procfs_wire g_remote_last;
static int g_remote_calls;
static int32_t g_remote_host;

static int f_proc_lookup(int32_t pid, lkd_proc_info *out)
{
    for (int i = 0; i < 3; i++) if (g_fake[i].pi.pid == pid) { *out = g_fake[i].pi; return 0; }
    return lkd_proc_lookup(pid, out);   /* the real viewer (T) stays visible */
}
static int f_tid_lookup(int32_t tid, lkd_tid_info *out)
{
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < g_fake[i].nthreads; k++)
            if (g_fake[i].tids[k] == tid) {
                memset(out, 0, sizeof *out);
                out->tid = tid; out->tgid = g_fake[i].pi.pid; out->state = g_fake[i].pi.state;
                return 0;
            }
    return lkd_tid_lookup(tid, out);
}
static int f_proc_list(lkd_proc_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < 3 && n < max; i++) out[n++] = g_fake[i].pi;
    lkd_proc_info real[8];
    int rn = lkd_proc_list(real, 8);
    for (int i = 0; i < rn && n < max; i++) out[n++] = real[i];
    return n;
}
static int f_tid_list(int32_t tgid, int32_t *tids, int max)
{
    for (int i = 0; i < 3; i++)
        if (g_fake[i].pi.pid == tgid) {
            for (int k = 0; k < g_fake[i].nthreads && k < max; k++) tids[k] = g_fake[i].tids[k];
            return g_fake[i].nthreads;
        }
    return lkd_tid_list(tgid, tids, max);
}
static int f_access(lkd_task *caller, int32_t pid, unsigned mode)
{
    g_access_calls++;
    g_access_last_pid = pid;
    g_access_last_mode = mode;
    return g_access_verdict;
}
static int f_remote(int32_t host_pid, const struct lkd_procfs_wire *rq, char **text, size_t *len)
{
    g_remote_calls++;
    g_remote_last = *rq;
    g_remote_host = host_pid;
    if (rq->kind == PK_WIRE_FDLIST) {
        int fds[3] = { 0, 1, 7 };
        *text = malloc(sizeof fds);
        memcpy(*text, fds, sizeof fds);
        *len = sizeof fds;
        return 0;
    }
    const char *t = "remote-text\n";
    *text = strdup(t);
    *len = strlen(t);
    return 0;
}

static void install_fake(void)
{
    memset(g_fake, 0, sizeof g_fake);
    /* 7001: a live process of user 1000 living in "host process" 91001, two threads */
    g_fake[0].pi = (lkd_proc_info){ .pid = 7001, .ppid = 1, .pgid = 7001, .sid = 7001, .host_pid = 91001, .state = 'S',
                                    .nthreads = 2, .uid = 1000, .euid = 1000, .gid = 1000, .egid = 1000, .dumpable = 1 };
    snprintf(g_fake[0].pi.comm, sizeof g_fake[0].pi.comm, "fakewine");
    snprintf(g_fake[0].pi.exe, sizeof g_fake[0].pi.exe, "/opt/wine/bin/wine64");
    g_fake[0].nthreads = 2; g_fake[0].tids[0] = 7001; g_fake[0].tids[1] = 7005;
    /* 7002: a zombie that exited with status 3 */
    g_fake[1].pi = (lkd_proc_info){ .pid = 7002, .ppid = 7001, .pgid = 7001, .sid = 7001, .host_pid = 91002, .state = 'Z',
                                    .nthreads = 1, .uid = 1000, .euid = 1000, .gid = 1000, .egid = 1000, .dumpable = 1,
                                    .wait_status = 3 << 8, .exit_signal = 17 };
    snprintf(g_fake[1].pi.comm, sizeof g_fake[1].pi.comm, "deadkid");
    g_fake[1].nthreads = 1; g_fake[1].tids[0] = 7002;
    /* 7003: a non-dumpable process of another user */
    g_fake[2].pi = (lkd_proc_info){ .pid = 7003, .ppid = 1, .pgid = 7003, .sid = 7003, .host_pid = 91003, .state = 'S',
                                    .nthreads = 1, .uid = 2000, .euid = 2000, .gid = 2000, .egid = 2000, .dumpable = 0 };
    snprintf(g_fake[2].pi.comm, sizeof g_fake[2].pi.comm, "secret");
    g_fake[2].nthreads = 1; g_fake[2].tids[0] = 7003;
    struct lkd_procfs_backend be = { .proc_lookup = f_proc_lookup, .tid_lookup = f_tid_lookup, .proc_list = f_proc_list,
                                     .tid_list = f_tid_list, .access = f_access, .remote = f_remote };
    lkd_procfs_set_backend(&be);
    g_access_verdict = 0;
    g_access_calls = 0;
    g_remote_calls = 0;
}

static int64_t names_of(const char *path, char names[][32], int max);

LT_TEST(fake_listing_and_attributes)
{
    install_fake();
    static char names[64][32];
    int64_t n = names_of("/proc", names, 64);
    LT_CHECK(n > 10);
    int i7001 = -1, i7002 = -1, i7003 = -1, ifirstnum = -1, order = 1, last = 0;
    for (int i = 0; i < n && i < 64; i++) {
        int v = atoi(names[i]);
        if (v > 0 && names[i][0] >= '0' && names[i][0] <= '9') {
            if (ifirstnum < 0) ifirstnum = i;
            if (v == 7001) i7001 = i;
            if (v == 7002) i7002 = i;
            if (v == 7003) i7003 = i;
        }
    }
    (void)order; (void)last;
    LT_CHECK(i7001 >= 0 && i7002 >= 0 && i7003 >= 0);
    LT_CHECK(i7001 < i7002 && i7002 < i7003);             /* the registry order is kept */
    for (int i = 0; i < n && i < 64; i++)
        if (names[i][0] >= '0' && names[i][0] <= '9') LT_CHECK(i >= ifirstnum);   /* fixed entries first */
    struct lx_stat st;
    LT_EQ(gstat("/proc/7001", &st), 0);
    LT_EQ(st.st_uid, 1000);
    LT_EQ(st.st_mode, LX_S_IFDIR | 0555);
    LT_EQ(gstat("/proc/7003", &st), 0);
    LT_EQ(st.st_uid, 2000);                                /* directory: euid even when not dumpable */
    LT_EQ(gstat("/proc/7003/status", &st), 0);
    LT_EQ(st.st_uid, 0);                                   /* task_dump_owner: root when not dumpable */
    LT_ERRNO(gstat("/proc/7999", &st), LX_ENOENT);
    /* task/ lists the threads of the registry; a thread directory exists; a foreign tid does not */
    static char tn[16][32];
    LT_EQ(names_of("/proc/7001/task", tn, 16), 4);         /* ., .., 7001, 7005 */
    LT_EQ(gstat("/proc/7001/task/7005", &st), 0);
    LT_ERRNO(gstat("/proc/7001/task/7003", &st), LX_ENOENT);
    LT_EQ(gstat("/proc/7005", &st), 0);                    /* /proc/<tid> is reachable though not listed */
    lkd_procfs_set_backend(NULL);
}

LT_TEST(fake_access_decisions)
{
    install_fake();
    int64_t len;
    char buf[128];
    /* status, stat, cmdline, comm, limits are world readable: no ptrace check at open */
    g_access_calls = 0;
    char *s = slurp("/proc/7001/comm", &len);
    LT_CHECK(s != NULL);
    free(s);
    LT_EQ(g_remote_calls, 1);
    LT_EQ(g_remote_host, 91001);                           /* the request goes to the host process of the pid */
    LT_EQ(g_remote_last.kind, PK_COMM);
    LT_EQ(g_remote_last.pid, 7001);
    LT_EQ(g_remote_last.permitted, 1);
    /* maps/environ: PTRACE_MODE_READ at open; a refusal is -EACCES (mm_access) */
    g_access_verdict = 0;
    g_access_calls = 0;
    int64_t fd = gopen("/proc/7001/environ", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    LT_EQ(g_access_last_pid, 7001);
    LT_EQ(g_access_last_mode, LKD_PTRACE_MODE_READ);
    SC(T, close, (uint64_t)fd);
    g_access_verdict = -LX_EPERM;
    LT_ERRNO(gopen("/proc/7001/environ", LX_O_RDONLY), LX_EACCES);
    LT_ERRNO(gopen("/proc/7001/maps", LX_O_RDONLY), LX_EACCES);
    LT_ERRNO(gopen("/proc/7001/auxv", LX_O_RDONLY), LX_EACCES);
    /* mem: PTRACE_MODE_ATTACH */
    g_access_verdict = 0;
    fd = gopen("/proc/7001/mem", LX_O_RDONLY);
    LT_CHECK(fd >= 0);
    LT_EQ(g_access_last_mode, LKD_PTRACE_MODE_ATTACH);
    SC(T, close, (uint64_t)fd);
    g_access_verdict = -LX_EPERM;
    LT_ERRNO(gopen("/proc/7001/mem", LX_O_RDONLY), LX_EACCES);
    /* the wire carries the verdict used for the stat fields (permitted == 0) */
    g_remote_calls = 0;
    s = slurp("/proc/7001/stat", &len);
    free(s);
    LT_EQ(g_remote_last.kind, PK_STAT);
    LT_EQ(g_remote_last.permitted, 0);
    LT_EQ(g_remote_last.per_thread, 0);
    s = slurp("/proc/7001/task/7005/stat", &len);
    free(s);
    LT_EQ(g_remote_last.per_thread, 1);
    LT_EQ(g_remote_last.tid, 7005);
    /* links: refused when ptrace_may_access refuses (proc_fd_access_allowed), whatever the file mode */
    LT_ERRNO(rdlink("/proc/7001/exe", buf, sizeof buf), LX_EACCES);
    LT_ERRNO(rdlink("/proc/7001/cwd", buf, sizeof buf), LX_EACCES);
    LT_ERRNO(rdlink("/proc/7001/root", buf, sizeof buf), LX_EACCES);
    g_access_verdict = 0;
    /* the fd directory is 0500 owned by the target's user: mode check first */
    struct lx_stat st;
    LT_EQ(gstat("/proc/7001/fd", &st), 0);
    LT_EQ(st.st_mode, LX_S_IFDIR | 0500);
    LT_EQ(st.st_uid, 1000);
    lkd_procfs_set_backend(NULL);
}

LT_TEST(fake_zombie_and_fd_list)
{
    install_fake();
    int64_t len;
    /* a zombie (registry only, nothing sent to its host process) */
    char *s = slurp("/proc/7002/status", &len);
    LT_CHECK(s != NULL);
    LT_EQ(g_remote_calls, 0);
    if (s) {
        LT_STREQ(fline(s, "Name"), "\tdeadkid");
        LT_STREQ(fline(s, "State"), "\tZ (zombie)");
        LT_STREQ(fline(s, "PPid"), "\t7001");
        LT_STREQ(fline(s, "Tgid"), "\t7002");
        LT_CHECK(field(s, "VmSize") == NULL);              /* no mm */
    }
    free(s);
    s = slurp("/proc/7002/stat", &len);
    LT_CHECK(s && !strncmp(s, "7002 (deadkid) Z 7001 7001 7001 ", 31));
    free(s);
    s = slurp("/proc/7002/cmdline", &len);
    LT_EQ(len, 0);                                         /* mm gone: empty read */
    free(s);
    char buf[64];
    LT_ERRNO(rdlink("/proc/7002/exe", buf, sizeof buf), LX_ENOENT);
    /* descriptor listing of a live remote process comes from its host process */
    static char names[16][32];
    int64_t n = names_of("/proc/7001/fd", names, 16);
    LT_EQ(n, 5);                                           /* ., .., 0, 1, 7 */
    LT_STREQ(names[2], "0");
    LT_STREQ(names[4], "7");
    LT_EQ(g_remote_last.kind, PK_WIRE_FDLIST);
    lkd_procfs_set_backend(NULL);
}

/* ---- Part 2: real processes -------------------------------------------------------------------- */

static void put64(int fd, int64_t v) { (void)!write(fd, &v, sizeof v); }
static int64_t get64(int fd)
{
    int64_t v = -12345;
    if (read(fd, &v, sizeof v) != (ssize_t)sizeof v) return -12345;
    return v;
}

static int64_t names_of(const char *path, char names[][32], int max)
{
    int64_t fd = gopen(path, LX_O_RDONLY | LX_O_DIRECTORY);
    if (fd < 0) return fd;
    int n = 0;
    for (;;) {
        int64_t r = SC(T, getdents64, (uint64_t)fd, gbulk(), 32768);
        if (r <= 0) { SC(T, close, (uint64_t)fd); return r < 0 ? r : n; }
        uint8_t *p = (uint8_t *)(uintptr_t)gbulk();
        for (int64_t off = 0; off < r;) {
            uint16_t reclen;
            memcpy(&reclen, p + off + 16, 2);
            if (n < max) snprintf(names[n], 32, "%s", (const char *)(p + off + 19));
            n++;
            off += reclen;
        }
    }
}

static void child_exit_group(int code)
{
    SC(T, exit_group, (uint64_t)code);
    int st = 0;
    if (lkd_task_release(T, &st)) _exit(st);
    _exit(99);
}

LT_TEST(real_other_process)
{
    lkd_procfs_set_backend(NULL);
    LT_EQ(mm_remote_start(), 0);        /* identity-window tests do not start the mm service by themselves */
    int up[2], go[2];
    LT_EQ(pipe(up), 0);
    LT_EQ(pipe(go), 0);
    SC(T, prctl, LX_PR_SET_NAME, gstr("parentname"));
    free(T->proc->cmdline);
    T->proc->cmdline = malloc(16); memcpy(T->proc->cmdline, "kid\0--arg\0z\0", 12); T->proc->cmdline_len = 12;
    free(T->proc->exe_path);
    T->proc->exe_path = strdup("/opt/ge/wine-preloader");
    int32_t parent = T->proc->pid, parent_tid = T->tid;
    fflush(NULL);
    int64_t pid = SC(T, fork, 0);
    if (pid == 0) {
        alarm(40);
        SC(T, prctl, LX_PR_SET_NAME, gstr("childname"));
        /* a private mapping with a known content, a memfd with content, a cwd, a pipe, tracing */
        int64_t reg = SC(T, mmap, 0, 1 << 16, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, (uint64_t)-1, 0);
        memcpy((void *)(uintptr_t)reg, "child-secret", 12);
        int64_t mfd = SC(T, memfd_create, gstr("kid-memfd"), LX_MFD_CLOEXEC);
        memcpy(SB + 70000, "kid-data!", 9);
        SC(T, write, (uint64_t)mfd, P(SB + 70000), 9);
        SC(T, chdir, gstr("/dev"));
        SC(T, pipe2, P(SB + 71000), 0);
        int pf[2];
        memcpy(pf, SB + 71000, sizeof pf);
        int64_t tr = SC(T, ptrace, 0 /* PTRACE_TRACEME */, 0, 0, 0);
        put64(up[1], reg);
        put64(up[1], mfd);
        put64(up[1], pf[0]);
        put64(up[1], tr);
        get64(go[0]);
        put64(up[1], memcmp((void *)(uintptr_t)reg, "PARENT-WROTE", 12) == 0);   /* /proc/pid/mem write seen */
        get64(go[0]);
        child_exit_group(7);
    }
    LT_CHECK(pid > 0);
    int64_t reg = get64(up[0]), mfd = get64(up[0]), pipe_rd = get64(up[0]), traced = get64(up[0]);
    char path[96], buf[512];
    int64_t len;
    /* status of the child, rendered by the child's own process */
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    char *s = slurp(path, &len);
    LT_CHECK(s != NULL);
    if (s) {
        LT_STREQ(fline(s, "Name"), "\tchildname");
        char want[32];
        snprintf(want, sizeof want, "\t%d", parent);
        LT_STREQ(fline(s, "PPid"), want);
        snprintf(want, sizeof want, "\t%d", (int)pid);
        LT_STREQ(fline(s, "Pid"), want);
        LT_STREQ(fline(s, "Threads"), "\t1");
        LT_STREQ(fline(s, "Uid"), "\t1000\t1000\t1000\t1000");
        LT_CHECK(field(s, "VmSize") != NULL);
        LT_EQ(traced, 0);   /* PTRACE_TRACEME succeeded in the child (ptrace module) */
        if (traced == 0) {
            snprintf(want, sizeof want, "\t%d", parent_tid);
            LT_STREQ(fline(s, "TracerPid"), want);
        }
    }
    free(s);
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    s = slurp(path, &len);
    LT_CHECK(s && strstr(s, "(childname)") != NULL);
    free(s);
    /* the child's cmdline is inherited from the fork; exe and cwd are its own */
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    s = slurp(path, &len);
    LT_EQ(len, 12);
    if (s) LT_MEMEQ(s, "kid\0--arg\0z\0", 12);
    free(s);
    snprintf(path, sizeof path, "/proc/%d/exe", (int)pid);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_STREQ(buf, "/opt/ge/wine-preloader");
    snprintf(path, sizeof path, "/proc/%d/cwd", (int)pid);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_STREQ(buf, "/dev");
    snprintf(path, sizeof path, "/proc/%d/root", (int)pid);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_STREQ(buf, "/");
    snprintf(path, sizeof path, "/proc/%d/limits", (int)pid);
    s = slurp(path, &len);
    LT_CHECK(s && strstr(s, "Max open files") != NULL);
    free(s);
    /* maps through the mm service of the child */
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    s = slurp(path, &len);
    /* adjacent anonymous mappings merge like in Linux: look for a rw-p line that covers the region */
    int covered = 0;
    for (char *p = s; p && *p;) {
        unsigned long long lo, hi;
        char perm[8];
        if (sscanf(p, "%llx-%llx %7s", &lo, &hi, perm) == 3 && !strcmp(perm, "rw-p") && lo <= (unsigned long long)reg &&
            hi >= (unsigned long long)reg + (1 << 16))
            covered = 1;
        p = strchr(p, '\n');
        if (p) p++;
    }
    LT_CHECK(covered);
    free(s);
    /* fd listing and links of the child */
    snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)pid, (int)mfd);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_STREQ(buf, "/memfd:kid-memfd (deleted)");
    snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)pid, (int)pipe_rd);
    LT_CHECK(rdlink(path, buf, sizeof buf) > 0);
    LT_CHECK(!strncmp(buf, "pipe:[", 6));
    static char names[64][32];
    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    int64_t cnt = names_of(path, names, 64);
    LT_CHECK(cnt >= 5);                                    /* ., .., mfd, the two pipe ends ... */
    int sawm = 0;
    for (int i = 0; i < cnt && i < 64; i++) if (atoi(names[i]) == (int)mfd && names[i][0] != '.') sawm = 1;
    LT_CHECK(sawm);
    /* fdinfo rendered by the child */
    snprintf(path, sizeof path, "/proc/%d/fdinfo/%d", (int)pid, (int)mfd);
    s = slurp(path, &len);
    LT_CHECK(s && strstr(s, "pos:\t9") != NULL);
    free(s);
    /* open() of a remote fd link: a new description of the child's memfd, received over SCM_RIGHTS */
    snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)pid, (int)mfd);
    s = slurp(path, &len);
    LT_EQ(len, 9);
    if (s) LT_MEMEQ(s, "kid-data!", 9);
    free(s);
    /* /proc/<pid>/mem: read the child's secret, write over it */
    snprintf(path, sizeof path, "/proc/%d/mem", (int)pid);
    int64_t m = gopen(path, LX_O_RDWR);
    LT_CHECK(m >= 0);
    if (m >= 0) {
        LT_EQ(SC(T, pread64, (uint64_t)m, gbulk() + 100, 12, (uint64_t)reg), 12);
        LT_MEMEQ((void *)(uintptr_t)(gbulk() + 100), "child-secret", 12);
        memcpy((void *)(uintptr_t)(gbulk() + 200), "PARENT-WROTE", 12);
        LT_EQ(SC(T, pwrite64, (uint64_t)m, gbulk() + 200, 12, (uint64_t)reg), 12);
        LT_ERRNO(SC(T, pread64, (uint64_t)m, gbulk() + 100, 8, 0x10), LX_EIO);
        SC(T, close, (uint64_t)m);
    }
    put64(go[1], 1);
    LT_EQ(get64(up[0]), 1);                                /* the child sees the bytes the parent wrote */
    /* the child exits: a zombie keeps its record until wait4 reaps it */
    put64(go[1], 1);
    int seen_z = 0;
    for (int i = 0; i < 400 && !seen_z; i++) {
        snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
        s = slurp(path, &len);
        if (s && !strncmp(fline(s, "State") + 1, "Z (zombie)", 10)) seen_z = 1;
        free(s);
        if (!seen_z) usleep(10000);
    }
    LT_CHECK(seen_z);
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pid);
    s = slurp(path, &len);
    LT_EQ(len, 0);
    free(s);
    int status = 0;
    LT_EQ(SC(T, wait4, (uint64_t)pid, P(SB + 72000), 0, 0), pid);
    memcpy(&status, SB + 72000, 4);
    LT_EQ(status, 7 << 8);
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    s = slurp(path, &len);
    LT_ERRNO(len, LX_ENOENT);                              /* reaped: the directory is gone */
    LT_ERRNO(gopen(path, LX_O_RDONLY), LX_ENOENT);
    close(up[0]); close(up[1]); close(go[0]); close(go[1]);
}

/* The parent of an orphan. Wine's double fork leaves the grandchild to the family's init (T here, its first process)
 * when the middle process exits. /proc/<pid>/status and stat are rendered by the orphan's own process, which made no
 * syscall since: they show the parent of the registry, not the copy taken at fork (the dead middle process). */
LT_TEST(orphan_ppid_follows_its_reaper)
{
    lkd_procfs_set_backend(NULL);
    int up[2], go[2];
    LT_EQ(pipe(up), 0);
    LT_EQ(pipe(go), 0);
    int32_t me = T->proc->pid;
    fflush(NULL);
    int64_t mid = SC(T, fork, 0);
    if (mid == 0) {
        alarm(40);
        int64_t g = SC(T, fork, 0);
        if (g == 0) {
            get64(go[0]);                                      /* no syscall until the entries were read */
            child_exit_group(0);
        }
        put64(up[1], g);
        child_exit_group(0);
    }
    LT_CHECK(mid > 0);
    int64_t g = get64(up[0]);
    LT_EQ(SC(T, wait4, (uint64_t)mid, 0, 0, 0), mid);
    char path[96], want[32];
    int64_t len;
    snprintf(want, sizeof want, "\t%d", (int)me);
    snprintf(path, sizeof path, "/proc/%d/status", (int)g);
    char *s = slurp(path, &len);
    LT_CHECK(s != NULL);
    if (s) LT_STREQ(fline(s, "PPid"), want);
    free(s);
    snprintf(path, sizeof path, "/proc/%d/stat", (int)g);
    s = slurp(path, &len);
    int ppid = -1;
    char state = 0;
    char *q = s ? strrchr(s, ')') : NULL;
    if (q) sscanf(q + 2, "%c %d", &state, &ppid);
    LT_EQ(ppid, me);
    free(s);
    put64(go[1], 1);
    LT_EQ(SC(T, wait4, (uint64_t)g, 0, 0, 0), g);
    close(up[0]); close(up[1]); close(go[0]); close(go[1]);
}

/* /proc/<pid>/lkd_host_pid, LKD's own file (no Linux model): the macOS pid of the host process that carries the
 * process, as "%d\n". Read from the registry, for any process of the family, without asking the other process.
 * Reachable by name, absent from the listing. A zombie has no host process left: -ESRCH. Threads share the pid of
 * their process; a forked child has its own host process. */
LT_TEST(host_pid_file)
{
    char path[96], want[32];
    int64_t len;
    static char names[64][32];
    /* registry of the fake backend: the host pid of 7001 (91001), with no request to that process */
    install_fake();
    char *s = slurp("/proc/7001/lkd_host_pid", &len);
    LT_STREQ(s, "91001\n");
    free(s);
    LT_EQ(g_remote_calls, 0);
    s = slurp("/proc/7001/task/7005/lkd_host_pid", &len);
    LT_STREQ(s, "91001\n");
    free(s);
    s = slurp("/proc/7002/lkd_host_pid", &len);   /* zombie */
    LT_ERRNO(len, LX_ESRCH);
    lkd_procfs_set_backend(NULL);

    /* the caller itself: this test program is the host process */
    snprintf(want, sizeof want, "%d\n", (int)getpid());
    s = slurp("/proc/self/lkd_host_pid", &len);
    LT_STREQ(s, want);
    free(s);
    struct lx_stat st;
    LT_EQ(gstat("/proc/self/lkd_host_pid", &st), 0);
    LT_EQ(st.st_mode, LX_S_IFREG | 0444);
    LT_ERRNO(gopen("/proc/self/lkd_host_pid", LX_O_WRONLY), LX_EACCES);
    int64_t n = names_of("/proc/self", names, 64);
    LT_CHECK(n > 10);
    for (int i = 0; i < n && i < 64; i++) LT_CHECK(strcmp(names[i], "lkd_host_pid") != 0);

    /* a forked child: its own host process, seen the same way by itself and by its parent */
    int up[2], go[2];
    LT_EQ(pipe(up), 0);
    LT_EQ(pipe(go), 0);
    int32_t parent = T->proc->pid;
    int64_t parent_host = getpid();
    fflush(NULL);
    int64_t pid = SC(T, fork, 0);
    if (pid == 0) {
        alarm(20);
        char cw[32], cp[96];
        int64_t l;
        snprintf(cw, sizeof cw, "%d\n", (int)getpid());
        char *own = slurp("/proc/self/lkd_host_pid", &l);
        snprintf(cp, sizeof cp, "/proc/%d/lkd_host_pid", parent);
        char *par = slurp(cp, &l);
        put64(up[1], getpid());
        put64(up[1], own && !strcmp(own, cw));
        put64(up[1], par ? strtoll(par, NULL, 10) : -1);
        free(own); free(par);
        get64(go[0]);
        child_exit_group(0);
    }
    LT_CHECK(pid > 0);
    int64_t child_host = get64(up[0]);
    LT_EQ(get64(up[0]), 1);                       /* the child read its own host pid */
    LT_EQ(get64(up[0]), parent_host);             /* and its parent's */
    LT_CHECK(child_host != parent_host);
    snprintf(path, sizeof path, "/proc/%d/lkd_host_pid", (int)pid);
    s = slurp(path, &len);
    snprintf(want, sizeof want, "%d\n", (int)child_host);
    LT_STREQ(s, want);
    free(s);
    put64(go[1], 1);
    int status = 0;
    LT_EQ(SC(T, wait4, (uint64_t)pid, P(SB + 72000), 0, 0), pid);
    memcpy(&status, SB + 72000, 4);
    LT_EQ(status, 0);
    LT_ERRNO(gopen(path, LX_O_RDONLY), LX_ENOENT);
    close(up[0]); close(up[1]); close(go[0]); close(go[1]);

    /* a thread: the pid of its process, through thread-self and task/<tid> */
    lkd_task *th = fake_make_thread(T);
    snprintf(want, sizeof want, "%d\n", (int)getpid());
    s = slurp_as(th, "/proc/thread-self/lkd_host_pid", &len);
    LT_STREQ(s, want);
    free(s);
    snprintf(path, sizeof path, "/proc/self/task/%d/lkd_host_pid", th->tid);
    s = slurp(path, &len);
    LT_STREQ(s, want);
    free(s);
    snprintf(path, sizeof path, "/proc/self/task/%d", th->tid);
    n = names_of(path, names, 64);
    LT_CHECK(n > 10);
    for (int i = 0; i < n && i < 64; i++) LT_CHECK(strcmp(names[i], "lkd_host_pid") != 0);
}

int main(void)
{
    procfs_fixture();
    LT_RUN(fake_listing_and_attributes);
    LT_RUN(fake_access_decisions);
    LT_RUN(fake_zombie_and_fd_list);
    LT_RUN(real_other_process);
    LT_RUN(orphan_ppid_follows_its_reaper);
    LT_RUN(host_pid_file);
    return lt_finish();
}
