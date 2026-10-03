/* fs_init.c: module init, host resource setup and the module hooks (fork, exec, exit).
 *
 * Host setup done once by lkd_fs_init:
 *   - RLIMIT_NOFILE of the host process is raised to min(rlim_max, kern.maxfilesperproc): every
 *     host-backed guest descriptor holds a host descriptor, and the guest table grows to RLIMIT_NOFILE of
 *     the guest (65536 hard by default). MESURE on this Mac: kern.maxfilesperproc = 138240.
 *   - the host umask is set to 0: LKD applies the guest umask itself to every creation.
 *   - mounts: the guest root (lkd_config.guest_root), and when a guest root is configured, host
 *     directories bound at /tmp and /dev/shm ($LKD_HOST_TMP / $LKD_HOST_SHM, default
 *     <darwin user temp dir>/lkd-<uid>/tmp and .../shm, created mode 01777).
 */
#include <fcntl.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "fs_priv.h"

void lkd_fs_fsctx_fork_prepare(void);
void lkd_fs_fsctx_fork_parent(void);
void lkd_fs_fsctx_fork_child(void);
void fs_side_lock_fork_child(void);

static char g_tmp_dir[PATH_MAX];
static char g_shm_dir[PATH_MAX];

const char *lkd_fs_host_tmp_dir(void) { return g_tmp_dir; }
const char *lkd_fs_host_shm_dir(void) { return g_shm_dir; }

static void raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) return;
    int maxf = 0;
    size_t sz = sizeof maxf;
    rlim_t target = rl.rlim_max;
    if (sysctlbyname("kern.maxfilesperproc", &maxf, &sz, NULL, 0) == 0 && maxf > 0 && (rlim_t)maxf < target) target = (rlim_t)maxf;
    if (rl.rlim_cur >= target) return;
    rl.rlim_cur = target;
    if (setrlimit(RLIMIT_NOFILE, &rl) < 0) lkd_log("fs: cannot raise RLIMIT_NOFILE to %llu (%d)", (unsigned long long)target, errno);
}

static int mkdirs(const char *p, mode_t m)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", p);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(buf, 0700);
        *s = '/';
    }
    if (mkdir(buf, m) < 0 && errno != EEXIST) return -1;
    chmod(buf, m);
    return 0;
}

static void setup_runtime_dirs(void)
{
    char base[PATH_MAX];
    const char *et = getenv("LKD_HOST_TMP"), *es = getenv("LKD_HOST_SHM");
    size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, base, sizeof base);
    if (!n) snprintf(base, sizeof base, "/tmp/");
    size_t bl = strlen(base);
    if (bl && base[bl - 1] == '/') base[bl - 1] = 0;
    if (et && et[0]) snprintf(g_tmp_dir, sizeof g_tmp_dir, "%s", et);
    else snprintf(g_tmp_dir, sizeof g_tmp_dir, "%s/lkd-%u/tmp", base, (unsigned)getuid());
    if (es && es[0]) snprintf(g_shm_dir, sizeof g_shm_dir, "%s", es);
    else snprintf(g_shm_dir, sizeof g_shm_dir, "%s/lkd-%u/shm", base, (unsigned)getuid());
    if (mkdirs(g_tmp_dir, 01777) < 0) g_tmp_dir[0] = 0;
    if (mkdirs(g_shm_dir, 01777) < 0) g_shm_dir[0] = 0;
}

void lkd_fs_init(void)
{
    raise_nofile();
    umask(0);
    setup_runtime_dirs();
    const lkd_config *cfg = lkd_config_get();
    const char *root = cfg && cfg->guest_root ? cfg->guest_root : "/";
    int rc = lkd_vfs_set_root(root);
    if (rc < 0) lkd_log("fs: guest root %s: error %d", root, rc);
    if (cfg && cfg->guest_root) {
        if (g_tmp_dir[0] && lkd_vfs_mount_bind("/tmp", g_tmp_dir, 0) < 0) lkd_log("fs: cannot bind /tmp");
        if (g_shm_dir[0] && lkd_vfs_mount_bind("/dev/shm", g_shm_dir, 0) < 0) lkd_log("fs: cannot bind /dev/shm");
    }
    fs_register_open();
    fs_register_rw();
    fs_register_stat();
    fs_register_path();
    fs_register_misc();
    fs_register_pipe();
    fs_register_memfd();
    fs_register_xattr();
}

/* ---- Hooks ------------------------------------------------------------------------------------- */

void lkd_fs_fork_prepare(lkd_task *t, uint64_t clone_flags)
{
    (void)t; (void)clone_flags;
    lkd_fs_fsctx_fork_prepare();
    fs_fdtables_fork_prepare();
}

void lkd_fs_fork_parent(lkd_task *t, pid_t host_child)
{
    (void)t; (void)host_child;
    fs_fdtables_fork_parent();
    lkd_fs_fsctx_fork_parent();
}

void lkd_fs_fork_child(lkd_task *t)
{
    (void)t;
    fs_fdtables_fork_child();
    lkd_fs_fsctx_fork_child();
    fs_side_lock_fork_child();
}

/* exec: close the O_CLOEXEC descriptors (fs/exec.c do_close_on_exec). */
void lkd_fs_exec_commit(lkd_task *t) { lkd_fd_close_on_exec(t); }

/* Each thread holds one reference on its fd table and fsctx (shared by CLONE_FILES / CLONE_FS). */
void lkd_fs_task_exit(lkd_task *t)
{
    if (!t) return;
    lkd_fdtable *f = t->fdt;
    t->fdt = NULL;
    lkd_fdtable_put(f);
    lkd_fsctx *c = t->fsctx;
    t->fsctx = NULL;
    lkd_fsctx_put(c);
}

void lkd_fs_proc_exit(lkd_task *t) { lkd_fs_task_exit(t); }
