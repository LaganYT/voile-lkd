/* net_priv.h: internal declarations of the net module (sockets, polling, event objects).
 * Not for other modules: they use net.h. */
#ifndef LKD_NET_PRIV_H
#define LKD_NET_PRIV_H

#include <poll.h>
#include <sys/socket.h>

#include "lkd_internal.h"
#include "fd.h"
#include "net.h"
#include "task.h"
#include "mm.h"
#include "sig.h"
#include "vfs.h"
#include <lkd/syscall_nr.h>

/* Effective configuration from core (src/core/core_priv.h; docs/requests/task-1.md asks to export it). */
const lkd_config *lkd_config_get(void);

/* ---- Readiness (wait.c) ----------------------------------------------------------------------- */

/* Events always reported by poll whatever was asked (fs/select.c: POLLERR | POLLHUP | POLLNVAL). */
#define NET_POLL_ALWAYS (LX_POLLERR | LX_POLLHUP | LX_POLLNVAL)

/* Host descriptor and host events that wake a waiter interested in `want` (LX_POLL*) on o.
 * Returns 1 and fills *hfd and *hev, or 0 when nothing can wake it (the truth never changes by itself). */
int net_obj_waitsrc(lkd_fdobj *o, uint32_t want, int *hfd, short *hev);
/* Linux events true now for o (all bits, the caller masks): uses ops->poll, or the host revents of a
 * host poll on the wait descriptor (hrev, valid when have_hrev), or a fresh zero-timeout host poll. */
uint32_t net_obj_revents(lkd_fdobj *o, uint32_t want, short hrev, int have_hrev);
/* Host revents (POLL*) to Linux LX_POLL* for a plain host descriptor. */
uint32_t net_host_revents_to_lx(short hrev);
/* Interruptible host poll of any size (same contract as lkd_block_poll, no 1024 limit). */
int net_block_poll(lkd_task *t, struct pollfd *pf, unsigned n, int64_t deadline_ns);
/* Deadline helpers. */
int64_t net_deadline_from_ms(int64_t ms);      /* ms < 0: -1 (forever) */
int64_t net_deadline_from_ts(const struct lx_timespec *ts); /* NULL: -1 */
int net_ts_valid(const struct lx_timespec *ts);

/* Restore the saved signal mask unless the syscall was interrupted (restore_saved_sigmask_unless). */
void net_finish_temp_mask(lkd_task *t, int64_t ret);
/* Read a sigset argument (ppoll, pselect6, epoll_pwait) and install it as a temporary mask. */
int net_install_sigmask(lkd_task *t, uint64_t gva, uint64_t size);

/* ---- Sockets (socket.c, sockmsg.c, sockopt.c, unixaddr.c) ------------------------------------- */

struct net_sock {
    int domain, type, protocol;     /* Linux values; type without SOCK_NONBLOCK/CLOEXEC */
    int owned_nonblock;             /* host fd is ours and permanently O_NONBLOCK; else use MSG_DONTWAIT */
    _Atomic int shut_rd, shut_wr;   /* shutdowns done through this description */
    _Atomic int listening;
    _Atomic int connecting;         /* nonblocking connect in progress */
    _Atomic int passcred;           /* SO_PASSCRED: recvmsg adds SCM_CREDENTIALS of the peer (real peer state) */
    _Atomic int pending_err;        /* SO_ERROR consumed by poll, handed to the next SO_ERROR/connect */
    _Atomic int recverr;            /* IP_RECVERR (1), IPV6_RECVERR (2): errors are also queued (wine-4) */
    _Atomic int errq_errno;         /* the error queue: one entry (Linux queues several), 0 = empty */
    _Atomic int pmtudisc, pmtudisc6; /* IP_MTU_DISCOVER, IPV6_MTU_DISCOVER: IP_PMTUDISC_* (WANT when created) */
    _Atomic int dontfrag6;          /* IPV6_DONTFRAG */
    uint8_t errq_addr[sizeof(struct lx_sockaddr_in6)];   /* destination of the failed datagram (msg_name) */
    uint32_t errq_addrlen;
    int64_t rcvtimeo_ns, sndtimeo_ns; /* SO_RCVTIMEO / SO_SNDTIMEO, 0 = none */
    /* AF_UNIX names as the guest gave them (getsockname, getpeername fallback) */
    uint8_t name[sizeof(struct lx_sockaddr_un)];
    uint32_t name_len;              /* 0 = unbound */
    uint8_t peer[sizeof(struct lx_sockaddr_un)];
    uint32_t peer_len;
    int abs_lock_fd;                /* flock'ed lock file of a bound abstract name, or -1 */
    int nl_peer;                    /* AF_NETLINK: the end LKD writes answers into (ifaces.c), else 0 */
    uint32_t nl_pid, nl_groups;     /* AF_NETLINK: port id and groups of the bound address */
    char *abs_host_path;            /* host stand-in of a bound abstract name, unlinked at release */
};

extern const lkd_fd_ops net_sock_ops;
static inline struct net_sock *net_sk(lkd_fdobj *o) { return o && o->ops == &net_sock_ops ? o->priv : NULL; }

/* Wrap a host socket descriptor (takes ownership). owned: we created it (set O_NONBLOCK, NOSIGPIPE). */
int net_sock_wrap(int hfd, int domain, int type, int protocol, uint32_t lx_flags, int owned, lkd_fdobj **out);
/* Socket object of fd with a reference, or NULL and *err = -EBADF / -ENOTSOCK. */
lkd_fdobj *net_sock_get(lkd_task *t, int fd, int *err);

/* Interruptible wait for a socket event (POLLIN/POLLOUT host bits) honouring SO_*TIMEO and the
 * guest O_NONBLOCK flag. Returns > 0 ready, -EAGAIN (nonblocking or timeout), -ERESTARTSYS or -EINTR. */
int net_sock_wait(lkd_task *t, lkd_fdobj *o, short hev, int nonblock, int64_t timeo_ns);
/* Translate the result of an interrupted socket wait per the SO_*TIMEO rule (CONVENTIONS 4.2). */

/* Address translation. Linux sockaddr in guest memory to a host sockaddr. For AF_UNIX the path is
 * resolved through the vfs; when the host path does not fit sun_path, *dirfd receives an open host
 * directory and the host address holds the relative leaf (bind/connect then run with that directory as
 * the thread's working directory). for_bind: the final component is created (not resolved). Returns
 * the host length or -errno. */
struct net_haddr {
    struct sockaddr_storage ss;
    socklen_t len;
    int dirfd;                      /* -1 or a directory for the relative AF_UNIX leaf */
    int abstract;                   /* AF_UNIX abstract name (stand-in file) */
    char abs_host[256];             /* stand-in path for an abstract name */
};
int net_addr_from_guest(lkd_task *t, struct net_sock *sk, uint64_t gva, uint32_t len, int for_bind, struct net_haddr *ha);
int net_addr_from_bytes(lkd_task *t, struct net_sock *sk, const uint8_t *raw, uint32_t len, int for_bind, struct net_haddr *ha);
void net_haddr_done(struct net_haddr *ha);
/* Run fn(arg) with the thread's working directory set to ha->dirfd when needed. */
int net_with_dir(struct net_haddr *ha, int (*fn)(void *), void *arg);
/* Host sockaddr to Linux bytes (AF_UNIX paths mapped back to guest paths, abstract stand-ins to their
 * name). Returns the Linux length. */
uint32_t net_addr_to_linux(const struct sockaddr *sa, socklen_t len, uint8_t *out, uint32_t cap);
/* Copy a Linux address to guest (addr, addrlen) pair with Linux truncation rules. */
int net_put_addr(uint64_t addr_gva, uint64_t len_gva, const uint8_t *lx, uint32_t lxlen);
/* Abstract namespace: bind bookkeeping. */
int net_abstract_bind_lock(struct net_sock *sk, const char *host_path, const uint8_t *name, uint32_t name_len);
void net_abstract_release(struct net_sock *sk);
int net_autobind_name(uint8_t *name, uint32_t *len);

/* Message I/O (sockmsg.c). */
int64_t net_sock_send(lkd_task *t, lkd_fdobj *o, uint64_t buf, uint64_t len, int lx_flags, struct net_haddr *to);
int64_t net_sock_recv(lkd_task *t, lkd_fdobj *o, uint64_t buf, uint64_t len, int lx_flags,
                      uint64_t addr_gva, uint64_t addrlen_gva);
void net_register_msg(void);
void net_register_sockopt(void);
void net_register_socket(void);
int net_sockopt_get_error(lkd_fdobj *o);
/* Credentials of the peer of an AF_UNIX socket as the guest sees them. Returns 1 when the peer is a
 * process of the family (pid known), else 0 with pid 0 and the overflow ids. */
int net_peer_cred(lkd_fdobj *o, struct lx_ucred *u);
/* 1 when the peer of the connected AF_UNIX socket o is another host process of this guest family. A peer in this
 * host process (a guest process cohosted before its exec) gets host descriptors, as a host program does: a pipe end
 * then arrives with the host's O_NONBLOCK (documented gap). */
int net_peer_is_family(lkd_fdobj *o);

/* ---- Interfaces and NETLINK_ROUTE (ifaces.c) ------------------------------------------------- */
int net_if_ioctl(uint32_t cmd, uint64_t arg, int64_t *ret);   /* 1 when cmd is an interface ioctl */
int net_nl_create(int type, int protocol, uint32_t lx_flags, lkd_fdobj **out);
int64_t net_nl_send(lkd_fdobj *o, const uint8_t *buf, uint64_t len);
uint32_t net_nl_name(struct net_sock *sk, uint8_t *out, int peer);
int net_nl_bind(lkd_task *t, struct net_sock *sk, uint64_t addr, uint32_t len);

/* ---- SCM_RIGHTS object transport (scm.c) ------------------------------------------------------ */

/* Host descriptors that carry object o to another process (dup'ed, the caller closes them). to_family: the receiving
 * end is another host process of the family (net_peer_is_family), which reads LKD's records; a host program gets
 * host descriptors only. Returns the count (1..3) or -errno (-EOPNOTSUPP for objects that cannot cross processes). */
int net_scm_export(lkd_fdobj *o, int *hfds, int cap, int to_family);
/* Rebuild objects from received host fds (takes ownership of every fd). Fills objs; returns count. */
int net_scm_import(lkd_task *t, const int *hfds, int n, lkd_fdobj **objs, int cap);

/* ---- Event objects --------------------------------------------------------------------------- */

void net_register_poll(void);
void net_register_epoll(void);
void net_register_eventfd(void);
void net_register_timerfd(void);
void net_register_inotify(void);

/* eventfd */
extern const lkd_fd_ops net_eventfd_ops;
int net_eventfd_from_host(int shm_fd, int rd, int wr, lkd_fdobj **out); /* SCM import */
int net_eventfd_export(lkd_fdobj *o, int *hfds);                        /* 3 fds */
int net_eventfd_is_region(int fd);                                       /* magic check of a shm fd */

/* Objects that hold a kqueue (not inherited by fork): they register here and are rebuilt in the
 * fork child, timers and inotify first, epoll sets last. */
struct net_kqobj {
    struct net_kqobj *next, *prev;
    int order;                       /* 0 rebuilt first, 1 after */
    lkd_fdobj *o;                    /* weak pointer: the object unregisters itself at release */
    void (*rebuild)(lkd_fdobj *o);   /* fork child: create a new kqueue and re-register */
};
void net_kqobj_add(struct net_kqobj *k);
void net_kqobj_del(struct net_kqobj *k);

/* epoll back-references: called when an object goes away. */
void net_epoll_forget_object(lkd_fdobj *o);
extern const lkd_fd_ops net_epoll_ops;
extern const lkd_fd_ops net_timerfd_ops;
extern const lkd_fd_ops net_inotify_ops;

/* Global lock of the module tables (LKD_RANK_EVENTS). */
void net_lock(void);
void net_unlock(void);

/* anon_inode fstat helper (fs/anon_inodes.c: mode 0600, no file type bits). */
int net_anon_fstat(lkd_fdobj *o, struct lx_stat *out);

/* Install a new object as a guest fd (lowest free); puts the caller's reference. Returns fd or -errno. */
int net_install(lkd_task *t, lkd_fdobj *o, int cloexec);

/* Linux clocks for timerfd (same host clocks as the task module's clock_gettime). */
int net_clock_valid_timerfd(int clk);
int64_t net_clock_now(int clk);

#endif
