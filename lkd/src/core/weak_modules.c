/* weak_modules.c: default no-op module hooks. Owner: core.
 * Each module defines the strong version of the hooks it needs in its own files; the rest stay here.
 * The library must be linked with -force_load / -all_load so that the strong definitions win. */
#include "lkd_internal.h"

#define DEFINE_WEAK_HOOKS(m) \
    LKD_WEAK void lkd_##m##_init(void) { } \
    LKD_WEAK void lkd_##m##_fork_prepare(lkd_task *t, uint64_t clone_flags) { (void)t; (void)clone_flags; } \
    LKD_WEAK void lkd_##m##_fork_parent(lkd_task *t, pid_t host_child) { (void)t; (void)host_child; } \
    LKD_WEAK void lkd_##m##_fork_child(lkd_task *t) { (void)t; } \
    LKD_WEAK void lkd_##m##_exec_commit(lkd_task *t) { (void)t; } \
    LKD_WEAK void lkd_##m##_task_exit(lkd_task *t) { (void)t; } \
    LKD_WEAK void lkd_##m##_proc_exit(lkd_task *t) { (void)t; }

LKD_MODULES(DEFINE_WEAK_HOOKS)

/* procfs service used by mm (mmap of /dev/zero, request procfs-2): no procfs, no /dev/zero object. */
struct lkd_fdobj;
LKD_WEAK int lkd_procfs_dev_is_zero(struct lkd_fdobj *o) { (void)o; return 0; }
