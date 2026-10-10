/* init.c: mounts the three trees through the virtual mount interface of the fs module (vfs.h).
 * procfs registers no syscall and keeps no lock: every file is generated from the state of the
 * modules at read time, so the fork/exec/exit hooks are the empty weak ones of the core.
 */
#include "procfs_priv.h"

static int v_getattr(lkd_task *t, void *priv, const char *rel, int nofollow, struct lkd_vattr *out)
{
    (void)nofollow;
    struct pf_node n;
    int rc = pf_resolve(t, (int)(intptr_t)priv, rel, &n);
    if (rc < 0) return rc;
    pf_vattr(&n, out);
    return 0;
}
static int64_t v_readlink(lkd_task *t, void *priv, const char *rel, char *buf, size_t cap)
{
    return pf_readlink(t, (int)(intptr_t)priv, rel, buf, cap);
}
static int v_open(lkd_task *t, void *priv, const char *rel, uint32_t fl, uint32_t mode, lkd_fdobj **out)
{
    return pf_open(t, (int)(intptr_t)priv, rel, fl, mode, out);
}
static int v_readdir(lkd_task *t, void *priv, const char *rel, uint64_t i, char name[256], uint8_t *dt, uint64_t *ino)
{
    return pf_readdir(t, (int)(intptr_t)priv, rel, i, name, dt, ino);
}

static const lkd_vmount_ops g_ops[3] = {
    { "proc", v_getattr, v_readlink, v_open, v_readdir },
    { "sysfs", v_getattr, v_readlink, v_open, v_readdir },
    { "devtmpfs", v_getattr, v_readlink, v_open, v_readdir },
};

void lkd_procfs_init(void)
{
    pf_remote_init();
    pf_dev_register();
    pf_file_register();
    static const char *const at[3] = { "/proc", "/sys", "/dev" };
    for (int i = 0; i < 3; i++) {
        int rc = lkd_vfs_mount_virtual(at[i], &g_ops[i], (void *)(intptr_t)i);
        if (rc < 0) lkd_log("procfs: cannot mount %s (%d)", at[i], rc);
    }
}
