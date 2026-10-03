/* fake_engine.c: see fake_engine.h. */
#include <stdlib.h>
#include <string.h>

#include "fake_engine.h"
#include "fd.h"
#include "mm.h"
#include "sig.h"
#include "vfs.h"

int fake_kick_count;
lkd_task *fake_last_kick;
int fake_invalidate_count;
uint64_t fake_inval_start[16], fake_inval_len[16];
int fake_frame_build_count;
lkd_delivery fake_last_delivery;
int fake_frame_restore_count;
int fake_spawn_count;
lkd_task *fake_last_spawned;
lkd_regs fake_last_spawn_regs;
int fake_exec_count;
int fake_fork_prepare_count, fake_fork_parent_count, fake_fork_child_count;
int fake_order[32];
int fake_order_n;
int fake_spawn_result, fake_frame_build_result, fake_frame_restore_result, fake_exec_result;
int fake_single_step_supported;

static void ev(int e)
{
    if (fake_order_n < 32) fake_order[fake_order_n++] = e;
}

struct fake_cpu *fake_cpu_of(lkd_task *t)
{
    if (!t->engine_data) t->engine_data = calloc(1, sizeof(struct fake_cpu));
    return t->engine_data;
}

static int f_get_regs(lkd_task *t, lkd_regs *out)
{
    struct fake_cpu *c = fake_cpu_of(t);
    if (c->running) return -LX_ESRCH;
    *out = c->regs;
    return 0;
}
static int f_set_regs(lkd_task *t, const lkd_regs *in)
{
    struct fake_cpu *c = fake_cpu_of(t);
    if (c->running) return -LX_ESRCH;
    c->regs = *in;
    return 0;
}
static int f_get_fp(lkd_task *t, int kind, void *buf, size_t *len)
{
    struct fake_cpu *c = fake_cpu_of(t);
    if (c->running) return -LX_ESRCH;
    const void *src = kind == LKD_FP_FXSAVE ? (void *)&c->fp : kind == LKD_FP_XSAVE ? (void *)c->xsave : NULL;
    size_t n = kind == LKD_FP_FXSAVE ? sizeof c->fp : c->xsave_len;
    if (!src) return -LX_EINVAL;
    memcpy(buf, src, n < *len ? n : *len);
    *len = n;
    return 0;
}
static int f_set_fp(lkd_task *t, int kind, const void *buf, size_t len)
{
    struct fake_cpu *c = fake_cpu_of(t);
    if (c->running) return -LX_ESRCH;
    if (kind == LKD_FP_FXSAVE) { if (len > sizeof c->fp) return -LX_EINVAL; memcpy(&c->fp, buf, len); return 0; }
    if (kind == LKD_FP_XSAVE) { if (len > sizeof c->xsave) return -LX_EINVAL; memcpy(c->xsave, buf, len); c->xsave_len = len; return 0; }
    return -LX_EINVAL;
}
static void f_kick(lkd_task *t) { fake_kick_count++; fake_last_kick = t; ev(FAKE_EV_KICK); }
static int f_spawn(lkd_task *child, const lkd_regs *start)
{
    fake_spawn_count++;
    fake_last_spawned = child;
    fake_last_spawn_regs = *start;
    ev(FAKE_EV_SPAWN);
    return fake_spawn_result;
}
static void f_invalidate(uint64_t gva, uint64_t len)
{
    fake_inval_start[fake_invalidate_count % 16] = gva;
    fake_inval_len[fake_invalidate_count % 16] = len;
    fake_invalidate_count++;
    ev(FAKE_EV_INVALIDATE);
}
static int f_frame_build(lkd_task *t, const lkd_delivery *d)
{
    (void)t;
    fake_frame_build_count++;
    fake_last_delivery = *d;
    ev(FAKE_EV_FRAME_BUILD);
    return fake_frame_build_result;
}
static int f_frame_restore(lkd_task *t, uint64_t *mask, struct lx_stack_t *st)
{
    struct fake_cpu *c = fake_cpu_of(t);
    fake_frame_restore_count++;
    ev(FAKE_EV_FRAME_RESTORE);
    if (fake_frame_restore_result) return fake_frame_restore_result;
    *mask = c->saved_mask;
    *st = c->saved_stack;
    return 0;
}
static int f_exec(lkd_task *t, const struct lkd_exec_image *img)
{
    (void)t; (void)img;
    fake_exec_count++;
    ev(FAKE_EV_EXEC);
    return fake_exec_result;
}
static void f_fork_prepare(lkd_task *t) { (void)t; fake_fork_prepare_count++; ev(FAKE_EV_FORK_PREPARE); }
static void f_fork_parent(lkd_task *t, pid_t c) { (void)t; (void)c; fake_fork_parent_count++; ev(FAKE_EV_FORK_PARENT); }
static void f_fork_child(lkd_task *t) { (void)t; fake_fork_child_count++; ev(FAKE_EV_FORK_CHILD); }
static int f_single_step(lkd_task *t, int on)
{
    if (!fake_single_step_supported) return -LX_ENOSYS;
    fake_cpu_of(t)->single_step = on;
    return 0;
}

const lkd_engine_ops fake_engine_ops = {
    .size = sizeof(lkd_engine_ops),
    .get_regs = f_get_regs, .set_regs = f_set_regs, .get_fp = f_get_fp, .set_fp = f_set_fp,
    .kick = f_kick, .spawn_thread = f_spawn, .invalidate_code = f_invalidate,
    .frame_build = f_frame_build, .frame_restore = f_frame_restore, .exec_image = f_exec,
    .fork_prepare = f_fork_prepare, .fork_parent = f_fork_parent, .fork_child = f_fork_child,
    .set_single_step = f_single_step,
};

static int init_rc = 1;
static int did_init;

int fake_engine_init(void)
{
    if (!did_init) {
        did_init = 1;
        init_rc = lkd_init(&fake_engine_ops);
    }
    return init_rc;
}

void fake_engine_reset(void)
{
    fake_kick_count = 0; fake_last_kick = NULL;
    fake_invalidate_count = 0; memset(fake_inval_start, 0, sizeof fake_inval_start); memset(fake_inval_len, 0, sizeof fake_inval_len);
    fake_frame_build_count = 0; memset(&fake_last_delivery, 0, sizeof fake_last_delivery);
    fake_frame_restore_count = 0; fake_spawn_count = 0; fake_last_spawned = NULL;
    memset(&fake_last_spawn_regs, 0, sizeof fake_last_spawn_regs);
    fake_exec_count = 0; fake_fork_prepare_count = fake_fork_parent_count = fake_fork_child_count = 0;
    memset(fake_order, 0, sizeof fake_order); fake_order_n = 0;
    fake_spawn_result = fake_frame_build_result = fake_frame_restore_result = fake_exec_result = 0;
    fake_single_step_supported = 0;
}

lkd_task *fake_make_task(void)
{
    lkd_task *t = lkd_task_alloc(NULL, 0);
    if (!t) return NULL;
    lkd_task_publish(t);
    fake_cpu_of(t);
    lkd_set_current(t);
    return t;
}

lkd_task *fake_make_thread(lkd_task *leader)
{
    uint64_t fl = LX_CLONE_THREAD | LX_CLONE_VM | LX_CLONE_FILES | LX_CLONE_FS | LX_CLONE_SIGHAND;
    lkd_task *t = lkd_task_alloc(leader, fl);
    if (!t) return NULL;
    t->mm = lkd_mm_clone(leader->mm, fl);
    t->fdt = lkd_fdtable_clone(leader->fdt, fl);
    t->fsctx = lkd_fsctx_clone(leader->fsctx, fl);
    t->sighand = lkd_sighand_clone(leader->sighand, fl);
    lkd_task_publish(t);
    fake_cpu_of(t);
    return t;
}
