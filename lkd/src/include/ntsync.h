/* ntsync.h: /dev/ntsync, NT synchronization objects (drivers/misc/ntsync.c). OWNER: sig.
 * The device and its objects live in a file of the family directory (ntsync.shm) that every process of the
 * family maps, so an object passed to another process (SCM_RIGHTS, the way wineserver hands objects to its
 * clients) is the same object there. Across host processes an object travels as its anchor: the read end of
 * a host pipe whose inode identifies the object. Status and limits: docs/status/ntsync.md. */
#ifndef LKD_NTSYNC_H
#define LKD_NTSYNC_H

#include "fd.h"

/* /dev/ntsync exists: the family directory is there and LKD_NTSYNC is not "0". */
int lkd_ntsync_available(void);
/* open("/dev/ntsync"): a new device. */
int lkd_ntsync_open_device(lkd_task *t, uint32_t lx_flags, lkd_fdobj **out);
/* SCM_RIGHTS. export: 1 with hfds[0] a host fd to send when o is an ntsync device or object, 0 when it is
 * not one, -LX_E* on failure. import: 1 with *out when the host fd is the anchor of a live object (the fd is
 * then owned by the object), 0 when it is not one. */
int lkd_ntsync_export(lkd_fdobj *o, int *hfds, int cap);
int lkd_ntsync_import(int host_fd, lkd_fdobj **out);

#endif
