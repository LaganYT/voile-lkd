/*
 * h3_pidinfo.c - H: what can be learned about another process WITHOUT a task port
 * (proc_pidinfo, proc_pidfdinfo, proc_pidpath, sysctl KERN_PROC / KERN_PROCARGS2).
 * This is the Darwin material for /proc/<pid>/{status,stat,maps,fd,task,cmdline,exe,cwd}.
 *
 *   H3.matrix   which flavors work against: own child, the parent shell (same uid, not debuggable), pid 1 (root)
 *   H3.status   fields usable for /proc/pid/status and stat
 *   H3.threads  thread list and per-thread info (task directory)
 *   H3.regions  PROC_PIDREGIONINFO walk (maps), cost against mach_vm_region
 *   H3.fds      descriptor list and vnode path of a descriptor
 *   H3.sysctl   kinfo_proc and KERN_PROCARGS2 (cmdline, environ), latency
 * Contents of another process's environment are never printed, only sizes.
 */
#include "hi_debuggee.h"
#include <libproc.h>
#include <sys/proc_info.h>

struct flav { const char *name; int flavor; int size; uint64_t arg; };

static const struct flav flavs[] = {
    {"PIDLISTFDS", PROC_PIDLISTFDS, 4096, 0},
    {"PIDTASKALLINFO", PROC_PIDTASKALLINFO, sizeof(struct proc_taskallinfo), 0},
    {"PIDTBSDINFO", PROC_PIDTBSDINFO, sizeof(struct proc_bsdinfo), 0},
    {"PIDTASKINFO", PROC_PIDTASKINFO, sizeof(struct proc_taskinfo), 0},
    {"PIDLISTTHREADS", PROC_PIDLISTTHREADS, 4096, 0},
    {"PIDREGIONINFO", PROC_PIDREGIONINFO, sizeof(struct proc_regioninfo), 0x100000000ull},
    {"PIDREGIONPATHINFO", PROC_PIDREGIONPATHINFO, sizeof(struct proc_regionwithpathinfo), 0x100000000ull},
    {"PIDVNODEPATHINFO", PROC_PIDVNODEPATHINFO, sizeof(struct proc_vnodepathinfo), 0},
    {"PIDT_SHORTBSDINFO", PROC_PIDT_SHORTBSDINFO, sizeof(struct proc_bsdshortinfo), 0},
    {"PIDUNIQIDENTIFIERINFO", 17, 256, 0},
    {"PIDT_BSDINFOWITHUNIQID", 18, 512, 0},
    {"PIDARCHINFO", PROC_PIDARCHINFO, 64, 0},
    {"PIDLISTTHREADIDS", 28, 4096, 0},
    {"PIDPLATFORMINFO", 30, 64, 0},
};

static void matrix(const char *who, pid_t pid)
{
    char line[1400]; int off = 0;
    for (size_t i = 0; i < sizeof flavs / sizeof flavs[0]; i++) {
        char *buf = calloc(1, flavs[i].size + 64);
        errno = 0;
        int n = proc_pidinfo(pid, flavs[i].flavor, flavs[i].arg, buf, flavs[i].size);
        int e = errno;
        if (n > 0) off += snprintf(line + off, sizeof line - off, " %s=ok(%d)", flavs[i].name, n);
        else off += snprintf(line + off, sizeof line - off, " %s=err(%d)", flavs[i].name, e);
        free(buf);
    }
    char path[PROC_PIDPATHINFO_MAXSIZE]; path[0] = 0;
    int pl = proc_pidpath(pid, path, sizeof path);
    off += snprintf(line + off, sizeof line - off, " proc_pidpath=%s", pl > 0 ? "ok" : "err");
    INFO("H3.matrix", "%s pid=%d:%s", who, (int)pid, line);
}

static pid_t find_by_name(const char *want)
{
    int n = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0) / (int)sizeof(pid_t);
    pid_t *l = malloc((n + 64) * sizeof(pid_t));
    int got = proc_listpids(PROC_ALL_PIDS, 0, l, (n + 64) * (int)sizeof(pid_t)) / (int)sizeof(pid_t);
    pid_t found = -1;
    for (int i = 0; i < got; i++) {
        char nm[64];
        if (l[i] > 0 && proc_name(l[i], nm, sizeof nm) > 0 && !strcmp(nm, want)) { found = l[i]; break; }
    }
    free(l);
    return found;
}

static void bench(const char *id, const char *label, int n, int (*fn)(void *), void *arg)
{
    uint64_t *v = malloc(sizeof(uint64_t) * n);
    int i;
    for (i = 0; i < n; i++) {
        uint64_t t0 = dp_ns();
        int r = fn(arg);
        v[i] = dp_ns() - t0;
        if (r <= 0) { INFO(id, "%s failed at iteration %d errno=%d", label, i, errno); break; }
    }
    if (i == n) hi_report_us(id, label, v, n);
    free(v);
}

static pid_t g_target;
static int f_bsdinfo(void *a) { struct proc_bsdinfo bi; (void)a; return proc_pidinfo(g_target, PROC_PIDTBSDINFO, 0, &bi, sizeof bi); }
static int f_taskinfo(void *a) { struct proc_taskinfo ti; (void)a; return proc_pidinfo(g_target, PROC_PIDTASKINFO, 0, &ti, sizeof ti); }
static int f_region(void *a) { struct proc_regioninfo ri; (void)a; return proc_pidinfo(g_target, PROC_PIDREGIONINFO, 0x100000000ull, &ri, sizeof ri); }
static int f_threadids(void *a) { uint64_t t[64]; (void)a; return proc_pidinfo(g_target, 28, 0, t, sizeof t); }
static uint64_t g_tid;
static int f_thread64(void *a) { struct proc_threadinfo ti; (void)a; return proc_pidinfo(g_target, PROC_PIDTHREADID64INFO, g_tid, &ti, sizeof ti); }
static int f_kinfo(void *a)
{
    struct kinfo_proc kp; size_t sz = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)g_target}; (void)a;
    return sysctl(mib, 4, &kp, &sz, NULL, 0) == 0 && sz > 0 ? 1 : -1;
}
static char *g_argbuf;
static int f_args(void *a)
{
    size_t sz = 1 << 20; int mib[3] = {CTL_KERN, KERN_PROCARGS2, (int)g_target}; (void)a;
    return sysctl(mib, 3, g_argbuf, &sz, NULL, 0) == 0 ? (int)sz : -1;
}
static int f_pidpath(void *a) { char p[PROC_PIDPATHINFO_MAXSIZE]; (void)a; return proc_pidpath(g_target, p, sizeof p); }
static int f_fds(void *a) { char b[4096]; (void)a; return proc_pidinfo(g_target, PROC_PIDLISTFDS, 0, b, sizeof b); }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "debuggee")) debuggee_main();
    dp_watchdog(120);
    signal(SIGPIPE, SIG_IGN);
    struct hi_child c; struct hi_dbg_info d;
    if (hi_spawn_debuggee(&c, &d)) { FAIL("H3.spawn", "debuggee did not start"); return dp_summary("h3_pidinfo"); }
    struct hi_msg r;
    hi_call(&c, DC_SPAWN_THREADS, 5, 0, 0, &r, 3000);
    INFO("H3.spawn", "debuggee %d with %llu extra threads (8 in total)", (int)c.pid, (unsigned long long)r.a[0]);
    usleep(50000);
    g_target = c.pid;
    g_argbuf = malloc(1 << 20);

    pid_t shell = getppid();
    pid_t root1 = 1;
    pid_t ws = find_by_name("WindowServer");
    matrix("own child (get-task-allow)", c.pid);
    matrix("parent process (same uid, not debuggable)", shell);
    matrix("pid 1 (root)", root1);
    if (ws > 0) matrix("WindowServer (other uid)", ws);
    matrix("self", getpid());

    /* status fields */
    struct proc_bsdinfo bi;
    int n = proc_pidinfo(c.pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi);
    if (n == (int)sizeof bi) {
        INFO("H3.status", "pbi_pid=%u ppid=%u pgid=%u uid=%u gid=%u ruid=%u rgid=%u svuid=%u status=%u(SRUN=2) flags=0x%x nice=%d start=%llu.%06llu name='%s' comm='%s' nfiles=%u",
             bi.pbi_pid, bi.pbi_ppid, bi.pbi_pgid, bi.pbi_uid, bi.pbi_gid, bi.pbi_ruid, bi.pbi_rgid, bi.pbi_svuid, bi.pbi_status, bi.pbi_flags, bi.pbi_nice,
             (unsigned long long)bi.pbi_start_tvsec, (unsigned long long)bi.pbi_start_tvusec, bi.pbi_name, bi.pbi_comm, bi.pbi_nfiles);
        CHECK(bi.pbi_ppid == (uint32_t)getpid() && bi.pbi_pid == (uint32_t)c.pid, "H3.status", "pid and ppid are exact");
        INFO("H3.status", "pbi_flags bits set: %s%s%s%s%s", (bi.pbi_flags & PROC_FLAG_SYSTEM) ? "SYSTEM " : "", (bi.pbi_flags & PROC_FLAG_TRACED) ? "TRACED " : "",
             (bi.pbi_flags & PROC_FLAG_EXEC) ? "EXEC " : "", (bi.pbi_flags & PROC_FLAG_LP64) ? "LP64 " : "", (bi.pbi_flags & PROC_FLAG_CTTY) ? "CTTY " : "");
    } else FAIL("H3.status", "PROC_PIDTBSDINFO failed errno=%d", errno);
    struct proc_taskinfo ti;
    n = proc_pidinfo(c.pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti);
    if (n == (int)sizeof ti)
        INFO("H3.status", "taskinfo: virtual=%llu KiB resident=%llu KiB threads=%d running=%d policy=%d faults=%d pageins=%d cow_faults=%d syscalls_mach=%d syscalls_unix=%d csw=%d user_ns=%llu sys_ns(mach ticks)=%llu",
             (unsigned long long)ti.pti_virtual_size / 1024, (unsigned long long)ti.pti_resident_size / 1024, ti.pti_threadnum, ti.pti_numrunning, ti.pti_policy,
             ti.pti_faults, ti.pti_pageins, ti.pti_cow_faults, ti.pti_syscalls_mach, ti.pti_syscalls_unix, ti.pti_csw, (unsigned long long)ti.pti_total_user, (unsigned long long)ti.pti_total_system);
    char path[PROC_PIDPATHINFO_MAXSIZE];
    n = proc_pidpath(c.pid, path, sizeof path);
    INFO("H3.status", "proc_pidpath (/proc/pid/exe) -> %s", n > 0 ? path : "failed");
    struct proc_vnodepathinfo vp;
    n = proc_pidinfo(c.pid, PROC_PIDVNODEPATHINFO, 0, &vp, sizeof vp);
    INFO("H3.status", "PROC_PIDVNODEPATHINFO (/proc/pid/cwd, root) -> %s cwd='%s'", n == (int)sizeof vp ? "ok" : "failed", n == (int)sizeof vp ? vp.pvi_cdir.vip_path : "");
    struct proc_archinfo ai; (void)ai;

    /* threads */
    {
        uint64_t ids[64];
        n = proc_pidinfo(c.pid, 28, 0, ids, sizeof ids);
        int cnt = n > 0 ? n / (int)sizeof(uint64_t) : 0;
        int hits = 0;
        char list[512]; int off = 0;
        for (int i = 0; i < cnt; i++) {
            off += snprintf(list + off, sizeof list - off, " %llu", (unsigned long long)ids[i]);
            if (ids[i] == d.main_tid || ids[i] == d.srv_tid || ids[i] == d.park_tid) hits++;
        }
        INFO("H3.threads", "PROC_PIDLISTTHREADIDS (28) -> %d thread ids:%s", cnt, list);
        CHECK(cnt == 8 && hits == 3, "H3.threads", "the 8 threads are listed and the three known thread ids (main %llu, server %llu, park %llu) match pthread_threadid_np of the child", (unsigned long long)d.main_tid, (unsigned long long)d.srv_tid, (unsigned long long)d.park_tid);
        uint64_t handles[64];
        n = proc_pidinfo(c.pid, PROC_PIDLISTTHREADS, 0, handles, sizeof handles);
        INFO("H3.threads", "PROC_PIDLISTTHREADS (6) -> %d entries, first values:%s", n > 0 ? n / 8 : 0, n >= 24 ? "" : " (few)");
        if (n >= 24) INFO("H3.threads", "  PIDLISTTHREADS first 3 = 0x%llx 0x%llx 0x%llx (thread handles, not ids)", (unsigned long long)handles[0], (unsigned long long)handles[1], (unsigned long long)handles[2]);
        if (n >= 8) {
            struct proc_threadinfo pti;
            int k = proc_pidinfo(c.pid, PROC_PIDTHREADINFO, handles[1], &pti, sizeof pti);
            INFO("H3.threads", "PROC_PIDTHREADINFO(handle) -> %s name='%s'", k == (int)sizeof pti ? "ok" : "failed", k == (int)sizeof pti ? pti.pth_name : "");
        }
        for (int i = 0; i < cnt && i < 8; i++) {
            struct proc_threadinfo pti;
            n = proc_pidinfo(c.pid, PROC_PIDTHREADID64INFO, ids[i], &pti, sizeof pti);
            if (n == (int)sizeof pti)
                INFO("H3.threads", "tid %llu name='%s' run_state=%d flags=0x%x cpu_usage=%d curpri=%d priority=%d maxpriority=%d user_ns=%llu sys_ns=%llu", (unsigned long long)ids[i], pti.pth_name, pti.pth_run_state, pti.pth_flags, pti.pth_cpu_usage, pti.pth_curpri, pti.pth_priority, pti.pth_maxpriority, (unsigned long long)pti.pth_user_time, (unsigned long long)pti.pth_system_time);
            else INFO("H3.threads", "tid %llu PROC_PIDTHREADID64INFO failed errno=%d", (unsigned long long)ids[i], errno);
        }
    }

    /* regions without a task port */
    {
        uint64_t addr = 0; int nreg = 0, with_path = 0; uint64_t t0 = dp_ns();
        for (;;) {
            struct proc_regionwithpathinfo rp;
            int k = proc_pidinfo(c.pid, PROC_PIDREGIONPATHINFO, addr, &rp, sizeof rp);
            if (k != (int)sizeof rp) break;
            nreg++;
            if (rp.prp_vip.vip_path[0]) with_path++;
            if (nreg <= 3 || (d.buf_addr >= rp.prp_prinfo.pri_address && d.buf_addr < rp.prp_prinfo.pri_address + rp.prp_prinfo.pri_size))
                INFO("H3.regions", "0x%llx-0x%llx prot=%d max=%d tag=%u share=%u pages_resident=%u dirtied=%u swapped=%u path='%s'", (unsigned long long)rp.prp_prinfo.pri_address, (unsigned long long)(rp.prp_prinfo.pri_address + rp.prp_prinfo.pri_size),
                     rp.prp_prinfo.pri_protection, rp.prp_prinfo.pri_max_protection, rp.prp_prinfo.pri_user_tag, rp.prp_prinfo.pri_share_mode, rp.prp_prinfo.pri_pages_resident, rp.prp_prinfo.pri_pages_dirtied, rp.prp_prinfo.pri_pages_swapped_out, rp.prp_vip.vip_path);
            addr = rp.prp_prinfo.pri_address + rp.prp_prinfo.pri_size;
        }
        double ms = (dp_ns() - t0) / 1e6;
        INFO("H3.regions", "PROC_PIDREGIONPATHINFO walked %d regions of the child (%d with a file path) in %.3f ms = %.1f us per region, no task port", nreg, with_path, ms, ms * 1000 / (nreg ? nreg : 1));
        /* the same on the parent process (not debuggable) */
        addr = 0; int nreg2 = 0; t0 = dp_ns(); int e2 = 0;
        for (;;) {
            struct proc_regioninfo ri;
            errno = 0;
            int k = proc_pidinfo(shell, PROC_PIDREGIONINFO, addr, &ri, sizeof ri);
            if (k != (int)sizeof ri) { e2 = errno; break; }
            nreg2++;
            addr = ri.pri_address + ri.pri_size;
        }
        INFO("H3.regions", "PROC_PIDREGIONINFO on the parent process (same uid, not debuggable): %d regions in %.3f ms, ended with errno=%d", nreg2, (dp_ns() - t0) / 1e6, e2);
    }

    /* fds */
    {
        struct proc_fdinfo fds[128];
        n = proc_pidinfo(c.pid, PROC_PIDLISTFDS, 0, fds, sizeof fds);
        int cnt = n > 0 ? n / (int)sizeof(struct proc_fdinfo) : 0;
        char list[512]; int off = 0;
        for (int i = 0; i < cnt; i++) off += snprintf(list + off, sizeof list - off, " %d:%s", fds[i].proc_fd, fds[i].proc_fdtype == PROX_FDTYPE_VNODE ? "vnode" : fds[i].proc_fdtype == PROX_FDTYPE_PIPE ? "pipe" : fds[i].proc_fdtype == PROX_FDTYPE_SOCKET ? "socket" : "other");
        INFO("H3.fds", "PROC_PIDLISTFDS -> %d descriptors:%s (expect 0,1,2 inherited, 3 and 4 the command pipes, 5 and 6 the park pipe)", cnt, list);
        struct vnode_fdinfowithpath vfi;
        for (int i = 0; i < cnt; i++) {
            if (fds[i].proc_fdtype != PROX_FDTYPE_VNODE) continue;
            int k = proc_pidfdinfo(c.pid, fds[i].proc_fd, PROC_PIDFDVNODEPATHINFO, &vfi, sizeof vfi);
            INFO("H3.fds", "fd %d vnode path -> %s", fds[i].proc_fd, k == (int)sizeof vfi ? vfi.pvip.vip_path : "failed");
            break;
        }
    }

    /* sysctl views */
    {
        struct kinfo_proc kp; size_t sz = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)c.pid};
        if (sysctl(mib, 4, &kp, &sz, NULL, 0) == 0 && sz > 0)
            INFO("H3.sysctl", "kinfo_proc: comm='%s' stat=%d flag=0x%x ppid=%d pgid=%d tdev=%d e_ucred.uid=%u cr_ngroups=%d p_nice=%d p_priority=%d", kp.kp_proc.p_comm, kp.kp_proc.p_stat, kp.kp_proc.p_flag, kp.kp_eproc.e_ppid, kp.kp_eproc.e_pgid, kp.kp_eproc.e_tdev, kp.kp_eproc.e_ucred.cr_uid, kp.kp_eproc.e_ucred.cr_ngroups, kp.kp_proc.p_nice, kp.kp_proc.p_priority);
        sz = 1 << 20;
        int m3[3] = {CTL_KERN, KERN_PROCARGS2, (int)c.pid};
        if (sysctl(m3, 3, g_argbuf, &sz, NULL, 0) == 0) {
            int argc2; memcpy(&argc2, g_argbuf, sizeof argc2);
            char *p = g_argbuf + sizeof argc2;
            INFO("H3.sysctl", "KERN_PROCARGS2 of the child: %zu bytes, argc=%d, exec_path='%s'", sz, argc2, p);
        }
        sz = 1 << 20;
        int m3b[3] = {CTL_KERN, KERN_PROCARGS2, (int)shell};
        errno = 0;
        int rr = sysctl(m3b, 3, g_argbuf, &sz, NULL, 0);
        INFO("H3.sysctl", "KERN_PROCARGS2 of the parent process (same uid) -> %s (%zu bytes returned, contents not printed)", rr == 0 ? "ok" : "failed", rr == 0 ? sz : 0);
        sz = 1 << 20; m3b[2] = 1; errno = 0;
        rr = sysctl(m3b, 3, g_argbuf, &sz, NULL, 0);
        INFO("H3.sysctl", "KERN_PROCARGS2 of pid 1 (root) -> %s errno=%d", rr == 0 ? "ok" : "failed", rr == 0 ? 0 : errno);
    }

    /* latency */
    bench("H3.latency", "proc_pidinfo(PROC_PIDTBSDINFO)", 20000, f_bsdinfo, NULL);
    bench("H3.latency", "proc_pidinfo(PROC_PIDTASKINFO)", 20000, f_taskinfo, NULL);
    bench("H3.latency", "proc_pidinfo(PROC_PIDREGIONINFO) single", 20000, f_region, NULL);
    bench("H3.latency", "proc_pidinfo(list thread ids, 8 threads)", 20000, f_threadids, NULL);
    g_tid = d.srv_tid;
    bench("H3.latency", "proc_pidinfo(PROC_PIDTHREADID64INFO)", 20000, f_thread64, NULL);
    bench("H3.latency", "sysctl(KERN_PROC_PID) kinfo_proc", 20000, f_kinfo, NULL);
    bench("H3.latency", "sysctl(KERN_PROCARGS2)", 20000, f_args, NULL);
    bench("H3.latency", "proc_pidpath", 20000, f_pidpath, NULL);
    bench("H3.latency", "proc_pidinfo(PROC_PIDLISTFDS)", 20000, f_fds, NULL);

    hi_call(&c, DC_EXIT, 0, 0, 0, NULL, 1000);
    hi_reap(&c, 2000);
    return dp_summary("h3_pidinfo");
}
