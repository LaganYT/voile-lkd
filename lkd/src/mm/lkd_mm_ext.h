/* lkd_mm_ext.h: mm entry points that are not (yet) in src/include/mm.h. Owner: mm.
 * Requested for mm.h in docs/requests/mm-2.md; until then the engine and tests include this file. */
#ifndef LKD_MM_EXT_H
#define LKD_MM_EXT_H

#include "mm.h"

/* The loader entry points (set_brk_start, set_stack_start, set_mmap_base, window_guest_lo/hi) are in mm.h
 * (request mm-2). */
/* mremap with an explicit task (rlimits, capabilities). */
int64_t lkd_mm_remap_task(lkd_task *t, lkd_mm *mm, uint64_t addr, uint64_t old_len, uint64_t new_len,
                          uint64_t flags, uint64_t new_addr);
/* Guest memory of this process with FOLL_FORCE rules when force != 0 (bytes, or -errno when none). */
int64_t mm_local_rw(lkd_mm *mm, uint64_t gva, void *buf, uint64_t n, int write, int force);
/* Same through the service of another host process (tests, and the guest-pid path). */
int64_t mm_remote_rw_host(pid_t host_pid, uint64_t gva, void *buf, uint64_t n, int write, int force);
char *mm_remote_maps_host(pid_t host_pid, size_t *len_out, int *err);
int mm_remote_start(void);
void mm_remote_stop(void);

#endif
