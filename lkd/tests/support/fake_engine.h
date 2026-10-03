/* fake_engine.h: a recording fake of the engine services, for unit tests of every module. Owner: core.
 *
 * Each task gets a fake CPU (user_regs, FXSAVE image) in task->engine_data, created on first use.
 * Everything the ops are asked to do is recorded in the globals below and can be steered with the
 * fake_* knobs. Tests of other modules may extend it by writing requests to docs/requests/.
 */
#ifndef LKD_FAKE_ENGINE_H
#define LKD_FAKE_ENGINE_H

#include <lkd/lkd.h>
#include "task.h"

struct fake_cpu {
    lkd_regs regs;
    struct lx_user_fpregs fp;
    uint8_t xsave[1024];
    size_t xsave_len;
    int running;                 /* set by a test to make get_regs refuse (-ESRCH) like a running task */
    int single_step;
    uint64_t saved_mask;         /* what frame_restore returns */
    struct lx_stack_t saved_stack;
};

extern const lkd_engine_ops fake_engine_ops;

/* Initialize LKD with the fake engine (once per process; later calls are no-ops). Returns lkd_init's
 * result of the first call. */
int fake_engine_init(void);

/* Recorded effects. fake_engine_reset() clears them (not the cpus). */
extern int fake_kick_count;
extern lkd_task *fake_last_kick;
extern int fake_invalidate_count;
extern uint64_t fake_inval_start[16], fake_inval_len[16];   /* the last 16 calls, ring by count % 16 */
extern int fake_frame_build_count;
extern lkd_delivery fake_last_delivery;
extern int fake_frame_restore_count;
extern int fake_spawn_count;
extern lkd_task *fake_last_spawned;
extern lkd_regs fake_last_spawn_regs;
extern int fake_exec_count;
extern int fake_fork_prepare_count, fake_fork_parent_count, fake_fork_child_count;
extern int fake_order[32];                                  /* event log: FAKE_EV_* in call order */
extern int fake_order_n;
enum { FAKE_EV_FORK_PREPARE = 1, FAKE_EV_FORK_PARENT, FAKE_EV_FORK_CHILD, FAKE_EV_KICK, FAKE_EV_INVALIDATE,
       FAKE_EV_FRAME_BUILD, FAKE_EV_FRAME_RESTORE, FAKE_EV_SPAWN, FAKE_EV_EXEC };

/* Knobs. */
extern int fake_spawn_result;         /* returned by spawn_thread, default 0 */
extern int fake_frame_build_result;   /* default 0; -LX_EFAULT simulates an unwritable stack */
extern int fake_frame_restore_result;
extern int fake_exec_result;
extern int fake_single_step_supported; /* default 0: set_single_step returns -ENOSYS */

void fake_engine_reset(void);
struct fake_cpu *fake_cpu_of(lkd_task *t);

/* A published task with a fake CPU, bound as the current task of the calling thread. */
lkd_task *fake_make_task(void);
/* A thread of the same process (CLONE_THREAD semantics through lkd_task_alloc). */
lkd_task *fake_make_thread(lkd_task *leader);

#endif
