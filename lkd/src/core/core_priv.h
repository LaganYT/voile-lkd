/* core_priv.h: functions shared between core files and core's own tests. Not for other modules. */
#ifndef LKD_CORE_PRIV_H
#define LKD_CORE_PRIV_H

#include "lkd_internal.h"

int lkd_usermem_install_handlers(void);
void lkd_trace_init_from_env(void);
void lkd_trace_set_enosys_logging(int on);
int lkd_trace_level(void);
void lkd_core_register_syscalls(void);
void lkd_dispatch_set_init_module(const char *name);
int lkd_dispatch_conflicts(void);
void lkd_dispatch_reset(void); /* tests only */

/* Effective configuration (defaults filled). */
const lkd_config *lkd_config_get(void);

#endif
