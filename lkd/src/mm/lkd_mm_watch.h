/* lkd_mm_watch.h: host pages holding translated guest code. Owner: mm. Self-contained: the engine may
 * include it (C or C++). See src/mm/watch.c for the protocol and the lock rules. */
#ifndef LKD_MM_WATCH_H
#define LKD_MM_WATCH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The engine translated guest code from [gva, gva+len) of the current task's address space. Every
 * 16 KiB host page touching the range loses PROT_WRITE until lkd_mm_code_write_fault (or an LKD write)
 * unwatches it. Idempotent and lock-free when already watched. ANY thread. Returns 0, -LX_EFAULT for a
 * range outside the guest window, -LX_ESRCH when there is no address space. */
int lkd_mm_watch_code(uint64_t gva, uint64_t len);

/* A store fault (data abort EC 0x24, WnR = 1, ANY fault status: macOS reports a store to a read-only page
 * that is not resident as a translation fault, DFSC 0x06, and a resident one as a permission fault) at
 * guest address gva. If the
 * host page is watched and the guest VMA allows the write: ops->invalidate_code(page, 16 KiB) is called
 * once, the watch is cleared, the page becomes writable, and 1 is returned (retry the store). 1 is also
 * returned when another thread already did this for the page. Otherwise 0: a real guest fault.
 * Callable from a SIGSEGV/SIGBUS handler of a guest thread (it holds no LKD lock); may block briefly on
 * the watch lock and the VMA read lock, never while guest code runs elsewhere under them. */
int lkd_mm_code_write_fault(uint64_t gva);

/* Is the host page of gva watched? (tests, diagnostics) */
int lkd_mm_code_watched(uint64_t gva);

#ifdef __cplusplus
}
#endif
#endif
