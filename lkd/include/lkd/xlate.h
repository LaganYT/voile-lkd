/* xlate.h: Linux <-> Darwin translation helpers (src/core/xlate.c). Owner: core.
 *
 * All functions are pure (no locks, no allocation) and callable from any thread. "lx" values are
 * Linux x86-64 ABI values (abi_x64.h), "host" values are Darwin values from the system headers.
 * Functions that can fail return a negative Linux errno or a documented sentinel.
 */
#ifndef LKD_XLATE_H
#define LKD_XLATE_H

#include <stdint.h>
#include <sys/types.h>
#include <lkd/abi_x64.h>

#ifdef __cplusplus
extern "C" {
#endif

struct stat;
struct statfs;

/* errno. lkd_errno_from_darwin returns the POSITIVE Linux errno for a Darwin errno (0 stays 0).
 * Darwin codes with no Linux counterpart map to the closest honest one (documented in xlate.c);
 * an unknown value maps to LX_EIO. lkd_errno_to_darwin is the inverse for the codes that exist on
 * both sides, 0 when Darwin has none. lkd_errno_name gives "ENOENT" for 2, or NULL. */
int lkd_errno_from_darwin(int host_errno);
int lkd_errno_to_darwin(int lx_errno);
const char *lkd_errno_name(int lx_errno);

/* open(2) flags. Translates the bits that have a Darwin equivalent into *host_flags (access mode,
 * O_CREAT, O_EXCL, O_NOCTTY, O_TRUNC, O_APPEND, O_NONBLOCK, O_DSYNC/O_SYNC, O_ASYNC, O_DIRECTORY,
 * O_NOFOLLOW, O_CLOEXEC) and sets *untranslated to the Linux bits the caller must handle itself or
 * may ignore: O_DIRECT (F_NOCACHE after open), O_NOATIME (ignorable), O_PATH, O_TMPFILE, and any
 * bit this table does not know. O_LARGEFILE is dropped silently. Returns 0, or -LX_EINVAL when the
 * access mode is 3. */
int lkd_open_flags_to_host(uint32_t lx_flags, int *host_flags, uint32_t *untranslated);
/* Status flags of a host fd (fcntl F_GETFL) as Linux O_* values, with O_LARGEFILE set as Linux
 * x86-64 reports it. */
uint32_t lkd_open_flags_from_host(int host_flags);

/* mmap(2) flags. Translates MAP_SHARED/PRIVATE/FIXED/ANONYMOUS/NORESERVE into *host_flags; other
 * Linux bits are left in *untranslated (MAP_GROWSDOWN, MAP_POPULATE, MAP_STACK, MAP_32BIT,
 * MAP_FIXED_NOREPLACE, MAP_DENYWRITE, MAP_EXECUTABLE, MAP_HUGETLB... the mm module decides).
 * Returns 0 or -LX_EINVAL (neither/both of SHARED and PRIVATE; SHARED_VALIDATE counts as SHARED). */
int lkd_mmap_flags_to_host(uint32_t lx_flags, int *host_flags, uint32_t *untranslated);
/* PROT_* bits: identical values on both sides; this validates and drops PROT_SEM/GROWSDOWN/GROWSUP.
 * Returns the host prot, or -LX_EINVAL for unknown bits. */
int lkd_prot_to_host(int lx_prot);

/* Signals. 0 when there is no equivalent (Linux SIGSTKFLT, SIGPWR, real-time signals; Darwin SIGEMT,
 * SIGINFO). */
int lkd_signo_to_host(int lx_sig);
int lkd_signo_from_host(int host_sig);

/* fcntl(2) commands that map to a Darwin command: F_DUPFD, F_GETFD, F_SETFD, F_GETFL, F_SETFL,
 * F_GETLK, F_SETLK, F_SETLKW, F_SETOWN, F_GETOWN, F_DUPFD_CLOEXEC, F_OFD_*. Returns 0 and *host_cmd,
 * or -LX_EINVAL for commands the fs module must implement itself (seals, pipe size, leases...). */
int lkd_fcntl_cmd_to_host(int lx_cmd, int *host_cmd);

/* Sockets. Families: AF_UNIX, AF_INET, AF_INET6; others -LX_EAFNOSUPPORT. Types 1..5 are identical
 * (flags SOCK_NONBLOCK/SOCK_CLOEXEC must be removed first with LX_SOCK_TYPE_MASK). */
int lkd_af_to_host(int lx_af);
int lkd_af_from_host(int host_af);
int lkd_socktype_to_host(int lx_type);
/* (level, optname) at the Linux numbering to the Darwin pair; -LX_ENOPROTOOPT when unsupported.
 * Covers SOL_SOCKET (REUSEADDR REUSEPORT KEEPALIVE BROADCAST SNDBUF RCVBUF LINGER OOBINLINE ERROR TYPE
 * ACCEPTCONN RCVLOWAT SNDLOWAT RCVTIMEO SNDTIMEO DONTROUTE DEBUG), IPPROTO_TCP (NODELAY, KEEPIDLE ->
 * TCP_KEEPALIVE, KEEPINTVL, KEEPCNT), IPPROTO_IP (TOS TTL MULTICAST_*), IPPROTO_IPV6 (V6ONLY, hops). */
int lkd_sockopt_to_host(int lx_level, int lx_opt, int *host_level, int *host_opt);
/* Reverse, for getsockopt names in tracing and for round trips in tests. */
int lkd_sockopt_from_host(int host_level, int host_opt, int *lx_level, int *lx_opt);
/* send/recv flags (MSG_*). Unknown Linux bits are returned in *untranslated (MSG_NOSIGNAL is one: the
 * net module sets SO_NOSIGPIPE instead). */
int lkd_msgflags_to_host(int lx_flags, int *untranslated);
int lkd_msgflags_from_host(int host_flags);

/* Device numbers: Darwin dev_t (major << 24 | minor) to the Linux new_encode_dev layout used in
 * stat.st_dev / st_rdev, and back. */
uint64_t lkd_dev_to_linux(dev_t host_dev);
dev_t lkd_dev_from_linux(uint64_t lx_dev);

/* stat. File type and permission bits have the same values on both systems. st_blocks is in 512-byte
 * units on both. st_blksize is the host value. The birth time is dropped (statx has it). */
void lkd_stat_from_host(const struct stat *hs, struct lx_stat *ls);
/* statx from a host stat. `mask` is the caller's requested STATX_* mask; stx_mask reports the fields
 * actually filled (BASIC_STATS plus BTIME when the host has it), as Linux does. */
void lkd_statx_from_host(const struct stat *hs, uint32_t mask, struct lx_statx *lx);
/* statfs. f_type is a Linux magic chosen from the host file system type name (apfs 0x42535041 "APSB",
 * hfs 0x4244, hfs+ 0x482b, msdos/exfat 0x4d44, nfs 0x6969, tmpfs 0x01021994, devfs 0x01021994);
 * unknown types report 0. f_flags carries ST_RDONLY/ST_NOSUID/ST_NODEV/ST_NOEXEC. */
void lkd_statfs_from_host(const struct statfs *hs, struct lx_statfs *ls);

/* d_type of getdents from a Darwin DT_* value. */
uint8_t lkd_dtype_from_host(uint8_t host_dtype);

#ifdef __cplusplus
}
#endif
#endif /* LKD_XLATE_H */
