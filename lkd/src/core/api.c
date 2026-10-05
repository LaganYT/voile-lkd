/* api.c: lkd_init, configuration, per-thread binding and accessors. Owner: core. */
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "core_priv.h"
#include "task.h"

static lkd_engine_ops g_ops;
static int g_inited;
static int g_have_ops;   /* ops are usable from the first module init on */
static lkd_config g_cfg = { .size = sizeof(lkd_config) };
static int g_cfg_set;
static _Thread_local lkd_task *g_current;

const lkd_config *lkd_config_get(void)
{
    if (!g_cfg.hostname) g_cfg.hostname = "obelisk";
    if (!g_cfg.uid) g_cfg.uid = 1000;
    if (!g_cfg.gid) g_cfg.gid = 1000;
    if (!g_cfg.first_pid) g_cfg.first_pid = 2;
    if (!g_cfg.guest_window_bytes) g_cfg.guest_window_bytes = 1ull << 46;
    return &g_cfg;
}

int lkd_init_config(const lkd_config *cfg)
{
    if (!cfg || cfg->size < offsetof(lkd_config, guest_root) + sizeof(char *)) return -LX_EINVAL;
    if (g_inited) return -LX_EBUSY;
    size_t n = cfg->size < sizeof g_cfg ? cfg->size : sizeof g_cfg;
    memset(&g_cfg, 0, sizeof g_cfg);
    memcpy(&g_cfg, cfg, n);
    g_cfg.size = sizeof g_cfg;
    g_cfg_set = 1;
    return 0;
}

const lkd_engine_ops *lkd_engine(void) { return g_have_ops ? &g_ops : NULL; }

static int ops_valid(const lkd_engine_ops *o)
{
    return o && o->size >= offsetof(lkd_engine_ops, fork_prepare) &&
           o->get_regs && o->set_regs && o->get_fp && o->set_fp && o->kick && o->spawn_thread &&
           o->invalidate_code && o->frame_build && o->frame_restore && o->exec_image;
}

int lkd_init(const lkd_engine_ops *ops)
{
    if (!ops_valid(ops)) return -LX_EINVAL;
    size_t n = ops->size < sizeof g_ops ? ops->size : sizeof g_ops;
    memset(&g_ops, 0, sizeof g_ops);
    memcpy(&g_ops, ops, n);
    g_ops.size = sizeof g_ops;
    g_have_ops = 1;
    if (g_inited) return 0; /* idempotent: ops replaced, modules stay initialized */
    (void)lkd_config_get();
    lkd_trace_init_from_env();
    if (lkd_usermem_install_handlers() < 0) return -LX_ENOMEM;
    lkd_dispatch_set_init_module("core");
    lkd_core_register_syscalls();
#define INIT(m) do { lkd_dispatch_set_init_module(#m); lkd_##m##_init(); } while (0);
    LKD_MODULES(INIT)
#undef INIT
    lkd_dispatch_set_init_module("");
    g_inited = 1;
    return lkd_dispatch_conflicts() ? -LX_EEXIST : 0;
}

void lkd_set_current(lkd_task *t) { g_current = t; }
lkd_task *lkd_current(void) { return g_current; }
void *lkd_task_engine_data(const lkd_task *t) { return t->engine_data; }
void lkd_task_set_engine_data(lkd_task *t, void *p) { t->engine_data = p; }
int32_t lkd_task_tid(const lkd_task *t) { return t->tid; }
int32_t lkd_task_pid(const lkd_task *t) { return t->proc->pid; }

void lkd_exec_image_free(lkd_exec_image *img)
{
    if (!img) return;
    free(img->guest_path);
    free(img->host_path);
    free(img->original_path);
    if (img->argv) { for (char **p = img->argv; *p; p++) free(*p); free(img->argv); }
    if (img->envp) { for (char **p = img->envp; *p; p++) free(*p); free(img->envp); }
    if (img->host_fd >= 0) close(img->host_fd);
    memset(img, 0, sizeof *img);
    img->host_fd = -1;
}
