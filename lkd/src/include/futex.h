/* futex.h: futex waits and wakes. OWNER: sig (module "sig" owns futex and robust lists).
 * Core provides weak placeholders (src/core/stubs_sig.c).
 *
 * Keys. A private futex is identified by (address-space identity, guest address); a shared futex by the
 * backing object and offset, so that processes mapping the same memfd or file see one queue. The mm
 * module computes the key (lkd_mm_futex_key); the futex implementation hashes it into buckets kept in
 * shared memory when the key is shared, in process memory when private.
 */
#ifndef LKD_FUTEX_H
#define LKD_FUTEX_H

#include "mm.h"

/* struct lkd_futex_key and lkd_mm_futex_key() are declared in mm.h (mm computes the key). */

/* Wake up to nr waiters on the word at gva. Used by task for CLONE_CHILD_CLEARTID / set_tid_address at
 * thread exit (after writing 0 to the word) and by the robust-list walk. Returns the number woken or
 * -errno. Takes the futex bucket lock (rank 70); callable under locks of lower rank. */
int lkd_futex_wake_addr(lkd_task *t, uint64_t gva, int nr, int private_hint);

/* Robust futex list walk at thread exit (set_robust_list): for every held lock set FUTEX_OWNER_DIED,
 * wake one waiter. Bounded to 2048 entries like Linux. OWNER. */
void lkd_futex_exit_robust_list(lkd_task *t);

/* Interrupt protocol for futex waits. A waiter registers in its bucket, then sleeps on a host
 * primitive; lkd_task_wake(t) (core) and the kick make the sleep return, the waiter dequeues itself and
 * returns -LX_ERESTARTSYS (no timeout) or -LX_ERESTART_RESTARTBLOCK (with timeout), as Linux does. */

#endif /* LKD_FUTEX_H */
