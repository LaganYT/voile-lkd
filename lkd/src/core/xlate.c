/* xlate.c: Linux x86-64 <-> Darwin translation helpers. See include/lkd/xlate.h. Owner: core. */
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#include <lkd/xlate.h>

/* ---- errno ------------------------------------------------------------------------------------ */

/* Each row: host errno, Linux errno, Linux name. Darwin codes with no Linux twin are mapped to the
 * nearest honest code and noted: EPROCLIM -> EAGAIN (fork limit), the RPC and policy codes
 * -> EIO, EAUTH/ENEEDAUTH -> EACCES, EFTYPE -> EINVAL, EBADEXEC/EBADARCH/ESHLIBVERS/EBADMACHO ->
 * ENOEXEC, ENOATTR -> ENODATA (what Linux returns for a missing xattr), EQFULL -> ENOBUFS,
 * EPWROFF/EDEVERR -> EIO. */
struct errmap { int host; int lx; };
static const struct errmap errmap[] = {
    { EPERM, LX_EPERM }, { ENOENT, LX_ENOENT }, { ESRCH, LX_ESRCH }, { EINTR, LX_EINTR },
    { EIO, LX_EIO }, { ENXIO, LX_ENXIO }, { E2BIG, LX_E2BIG }, { ENOEXEC, LX_ENOEXEC },
    { EBADF, LX_EBADF }, { ECHILD, LX_ECHILD }, { EDEADLK, LX_EDEADLK }, { ENOMEM, LX_ENOMEM },
    { EACCES, LX_EACCES }, { EFAULT, LX_EFAULT }, { ENOTBLK, LX_ENOTBLK }, { EBUSY, LX_EBUSY },
    { EEXIST, LX_EEXIST }, { EXDEV, LX_EXDEV }, { ENODEV, LX_ENODEV }, { ENOTDIR, LX_ENOTDIR },
    { EISDIR, LX_EISDIR }, { EINVAL, LX_EINVAL }, { ENFILE, LX_ENFILE }, { EMFILE, LX_EMFILE },
    { ENOTTY, LX_ENOTTY }, { ETXTBSY, LX_ETXTBSY }, { EFBIG, LX_EFBIG }, { ENOSPC, LX_ENOSPC },
    { ESPIPE, LX_ESPIPE }, { EROFS, LX_EROFS }, { EMLINK, LX_EMLINK }, { EPIPE, LX_EPIPE },
    { EDOM, LX_EDOM }, { ERANGE, LX_ERANGE }, { EAGAIN, LX_EAGAIN }, { EINPROGRESS, LX_EINPROGRESS },
    { EALREADY, LX_EALREADY }, { ENOTSOCK, LX_ENOTSOCK }, { EDESTADDRREQ, LX_EDESTADDRREQ },
    { EMSGSIZE, LX_EMSGSIZE }, { EPROTOTYPE, LX_EPROTOTYPE }, { ENOPROTOOPT, LX_ENOPROTOOPT },
    { EPROTONOSUPPORT, LX_EPROTONOSUPPORT }, { ESOCKTNOSUPPORT, LX_ESOCKTNOSUPPORT },
    { EOPNOTSUPP, LX_EOPNOTSUPP }, { ENOTSUP, LX_EOPNOTSUPP }, { EPFNOSUPPORT, LX_EPFNOSUPPORT }, { EAFNOSUPPORT, LX_EAFNOSUPPORT },
    { EADDRINUSE, LX_EADDRINUSE }, { EADDRNOTAVAIL, LX_EADDRNOTAVAIL }, { ENETDOWN, LX_ENETDOWN },
    { ENETUNREACH, LX_ENETUNREACH }, { ENETRESET, LX_ENETRESET }, { ECONNABORTED, LX_ECONNABORTED },
    { ECONNRESET, LX_ECONNRESET }, { ENOBUFS, LX_ENOBUFS }, { EISCONN, LX_EISCONN },
    { ENOTCONN, LX_ENOTCONN }, { ESHUTDOWN, LX_ESHUTDOWN }, { ETOOMANYREFS, LX_ETOOMANYREFS },
    { ETIMEDOUT, LX_ETIMEDOUT }, { ECONNREFUSED, LX_ECONNREFUSED }, { ELOOP, LX_ELOOP },
    { ENAMETOOLONG, LX_ENAMETOOLONG }, { EHOSTDOWN, LX_EHOSTDOWN }, { EHOSTUNREACH, LX_EHOSTUNREACH },
    { ENOTEMPTY, LX_ENOTEMPTY }, { EUSERS, LX_EUSERS }, { EDQUOT, LX_EDQUOT }, { ESTALE, LX_ESTALE },
    { EREMOTE, LX_EREMOTE }, { ENOLCK, LX_ENOLCK }, { ENOSYS, LX_ENOSYS }, { EOVERFLOW, LX_EOVERFLOW },
    { ECANCELED, LX_ECANCELED }, { EIDRM, LX_EIDRM }, { ENOMSG, LX_ENOMSG }, { EILSEQ, LX_EILSEQ },
    { EBADMSG, LX_EBADMSG }, { EMULTIHOP, LX_EMULTIHOP }, { ENODATA, LX_ENODATA }, { ENOLINK, LX_ENOLINK },
    { ENOSR, LX_ENOSR }, { ENOSTR, LX_ENOSTR }, { EPROTO, LX_EPROTO }, { ETIME, LX_ETIME },
    { ENOTRECOVERABLE, LX_ENOTRECOVERABLE }, { EOWNERDEAD, LX_EOWNERDEAD },
    /* Darwin-only codes, one way (host -> Linux) */
    { EPROCLIM, LX_EAGAIN }, { EBADRPC, LX_EIO }, { ERPCMISMATCH, LX_EIO }, { EPROGUNAVAIL, LX_EIO },
    { EPROGMISMATCH, LX_EIO }, { EPROCUNAVAIL, LX_EIO }, { EFTYPE, LX_EINVAL }, { EAUTH, LX_EACCES },
    { ENEEDAUTH, LX_EACCES }, { EPWROFF, LX_EIO }, { EDEVERR, LX_EIO }, { EBADEXEC, LX_ENOEXEC },
    { EBADARCH, LX_ENOEXEC }, { ESHLIBVERS, LX_ENOEXEC }, { EBADMACHO, LX_ENOEXEC },
    { ENOATTR, LX_ENODATA }, { ENOPOLICY, LX_EIO }, { EQFULL, LX_ENOBUFS },
};

int lkd_errno_from_darwin(int e)
{
    if (e == 0) return 0;
    for (size_t i = 0; i < sizeof errmap / sizeof errmap[0]; i++)
        if (errmap[i].host == e) return errmap[i].lx;
    return LX_EIO;
}

int lkd_errno_to_darwin(int lx)
{
    if (lx == 0) return 0;
    /* First match wins: the two-way rows come before the Darwin-only rows. */
    for (size_t i = 0; i < sizeof errmap / sizeof errmap[0]; i++)
        if (errmap[i].lx == lx) return errmap[i].host;
    return 0;
}

static const char *const errnames[LX_ELAST + 1] = {
    [LX_EPERM] = "EPERM", [LX_ENOENT] = "ENOENT", [LX_ESRCH] = "ESRCH", [LX_EINTR] = "EINTR",
    [LX_EIO] = "EIO", [LX_ENXIO] = "ENXIO", [LX_E2BIG] = "E2BIG", [LX_ENOEXEC] = "ENOEXEC",
    [LX_EBADF] = "EBADF", [LX_ECHILD] = "ECHILD", [LX_EAGAIN] = "EAGAIN", [LX_ENOMEM] = "ENOMEM",
    [LX_EACCES] = "EACCES", [LX_EFAULT] = "EFAULT", [LX_ENOTBLK] = "ENOTBLK", [LX_EBUSY] = "EBUSY",
    [LX_EEXIST] = "EEXIST", [LX_EXDEV] = "EXDEV", [LX_ENODEV] = "ENODEV", [LX_ENOTDIR] = "ENOTDIR",
    [LX_EISDIR] = "EISDIR", [LX_EINVAL] = "EINVAL", [LX_ENFILE] = "ENFILE", [LX_EMFILE] = "EMFILE",
    [LX_ENOTTY] = "ENOTTY", [LX_ETXTBSY] = "ETXTBSY", [LX_EFBIG] = "EFBIG", [LX_ENOSPC] = "ENOSPC",
    [LX_ESPIPE] = "ESPIPE", [LX_EROFS] = "EROFS", [LX_EMLINK] = "EMLINK", [LX_EPIPE] = "EPIPE",
    [LX_EDOM] = "EDOM", [LX_ERANGE] = "ERANGE", [LX_EDEADLK] = "EDEADLK", [LX_ENAMETOOLONG] = "ENAMETOOLONG",
    [LX_ENOLCK] = "ENOLCK", [LX_ENOSYS] = "ENOSYS", [LX_ENOTEMPTY] = "ENOTEMPTY", [LX_ELOOP] = "ELOOP",
    [LX_ENOMSG] = "ENOMSG", [LX_EIDRM] = "EIDRM", [LX_ENOSTR] = "ENOSTR", [LX_ENODATA] = "ENODATA",
    [LX_ETIME] = "ETIME", [LX_ENOSR] = "ENOSR", [LX_EREMOTE] = "EREMOTE", [LX_ENOLINK] = "ENOLINK",
    [LX_EPROTO] = "EPROTO", [LX_EMULTIHOP] = "EMULTIHOP", [LX_EBADMSG] = "EBADMSG",
    [LX_EOVERFLOW] = "EOVERFLOW", [LX_EILSEQ] = "EILSEQ", [LX_ERESTART] = "ERESTART", [LX_EUSERS] = "EUSERS",
    [LX_ENOTSOCK] = "ENOTSOCK", [LX_EDESTADDRREQ] = "EDESTADDRREQ", [LX_EMSGSIZE] = "EMSGSIZE",
    [LX_EPROTOTYPE] = "EPROTOTYPE", [LX_ENOPROTOOPT] = "ENOPROTOOPT",
    [LX_EPROTONOSUPPORT] = "EPROTONOSUPPORT", [LX_ESOCKTNOSUPPORT] = "ESOCKTNOSUPPORT",
    [LX_EOPNOTSUPP] = "EOPNOTSUPP", [LX_EPFNOSUPPORT] = "EPFNOSUPPORT", [LX_EAFNOSUPPORT] = "EAFNOSUPPORT",
    [LX_EADDRINUSE] = "EADDRINUSE", [LX_EADDRNOTAVAIL] = "EADDRNOTAVAIL", [LX_ENETDOWN] = "ENETDOWN",
    [LX_ENETUNREACH] = "ENETUNREACH", [LX_ENETRESET] = "ENETRESET", [LX_ECONNABORTED] = "ECONNABORTED",
    [LX_ECONNRESET] = "ECONNRESET", [LX_ENOBUFS] = "ENOBUFS", [LX_EISCONN] = "EISCONN",
    [LX_ENOTCONN] = "ENOTCONN", [LX_ESHUTDOWN] = "ESHUTDOWN", [LX_ETOOMANYREFS] = "ETOOMANYREFS",
    [LX_ETIMEDOUT] = "ETIMEDOUT", [LX_ECONNREFUSED] = "ECONNREFUSED", [LX_EHOSTDOWN] = "EHOSTDOWN",
    [LX_EHOSTUNREACH] = "EHOSTUNREACH", [LX_EALREADY] = "EALREADY", [LX_EINPROGRESS] = "EINPROGRESS",
    [LX_ESTALE] = "ESTALE", [LX_EDQUOT] = "EDQUOT", [LX_ECANCELED] = "ECANCELED",
    [LX_EOWNERDEAD] = "EOWNERDEAD", [LX_ENOTRECOVERABLE] = "ENOTRECOVERABLE",
};

const char *lkd_errno_name(int lx)
{
    if (lx > 0 && lx <= LX_ELAST) return errnames[lx];
    switch (lx) {
    case LX_ERESTARTSYS: return "ERESTARTSYS";
    case LX_ERESTARTNOINTR: return "ERESTARTNOINTR";
    case LX_ERESTARTNOHAND: return "ERESTARTNOHAND";
    case LX_ERESTART_RESTARTBLOCK: return "ERESTART_RESTARTBLOCK";
    }
    return NULL;
}

/* ---- open flags ------------------------------------------------------------------------------- */

int lkd_open_flags_to_host(uint32_t lx, int *host, uint32_t *untranslated)
{
    int h = 0;
    uint32_t left = lx;
    uint32_t acc = lx & LX_O_ACCMODE;
    if (acc == 3) return -LX_EINVAL;
    h |= (int)acc; /* O_RDONLY 0, O_WRONLY 1, O_RDWR 2 on both */
    left &= ~(uint32_t)LX_O_ACCMODE;

    int tmpfile = (lx & LX_O_TMPFILE) == LX_O_TMPFILE;
    if (tmpfile) left &= ~(uint32_t)LX_O_TMPFILE;

#define MAP(l, hv) do { if (left & (l)) { h |= (hv); left &= ~(uint32_t)(l); } } while (0)
    MAP(LX_O_CREAT, O_CREAT);
    MAP(LX_O_EXCL, O_EXCL);
    MAP(LX_O_NOCTTY, O_NOCTTY);
    MAP(LX_O_TRUNC, O_TRUNC);
    MAP(LX_O_APPEND, O_APPEND);
    MAP(LX_O_NONBLOCK, O_NONBLOCK);
    /* LX_O_SYNC includes the O_DSYNC bit: test the full value first. */
    if ((left & LX_O_SYNC) == LX_O_SYNC) { h |= O_SYNC; left &= ~(uint32_t)LX_O_SYNC; }
    MAP(LX_O_DSYNC, O_DSYNC);
    MAP(LX_O_ASYNC, O_ASYNC);
    MAP(LX_O_DIRECTORY, O_DIRECTORY);
    MAP(LX_O_NOFOLLOW, O_NOFOLLOW);
    MAP(LX_O_CLOEXEC, O_CLOEXEC);
#undef MAP
    left &= ~(uint32_t)LX_O_LARGEFILE; /* the kernel adds it itself on x86-64 */
    uint32_t un = left; /* DIRECT, NOATIME, PATH and unknown bits */
    if (tmpfile) un |= LX_O_TMPFILE;
    if (host) *host = h;
    if (untranslated) *untranslated = un;
    return 0;
}

uint32_t lkd_open_flags_from_host(int h)
{
    uint32_t lx = LX_O_LARGEFILE;
    lx |= (uint32_t)(h & O_ACCMODE);
    if (h & O_NONBLOCK) lx |= LX_O_NONBLOCK;
    if (h & O_APPEND) lx |= LX_O_APPEND;
    if (h & O_ASYNC) lx |= LX_O_ASYNC;
    if ((h & O_SYNC) == O_SYNC) lx |= LX_O_SYNC;
    else if (h & O_DSYNC) lx |= LX_O_DSYNC;
    return lx;
}

/* ---- mmap ------------------------------------------------------------------------------------- */

int lkd_mmap_flags_to_host(uint32_t lx, int *host, uint32_t *untranslated)
{
    int h = 0;
    uint32_t type = lx & LX_MAP_TYPE;
    if (type == LX_MAP_SHARED || type == LX_MAP_SHARED_VALIDATE) h |= MAP_SHARED;
    else if (type == LX_MAP_PRIVATE) h |= MAP_PRIVATE;
    else return -LX_EINVAL;
    uint32_t left = lx & ~(uint32_t)LX_MAP_TYPE;
    if (left & LX_MAP_FIXED) { h |= MAP_FIXED; left &= ~(uint32_t)LX_MAP_FIXED; }
    if (left & LX_MAP_ANONYMOUS) { h |= MAP_ANON; left &= ~(uint32_t)LX_MAP_ANONYMOUS; }
    if (left & LX_MAP_NORESERVE) { h |= MAP_NORESERVE; left &= ~(uint32_t)LX_MAP_NORESERVE; }
    if (host) *host = h;
    if (untranslated) *untranslated = left;
    return 0;
}

int lkd_prot_to_host(int p)
{
    const int known = LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC | LX_PROT_SEM | LX_PROT_GROWSDOWN | LX_PROT_GROWSUP;
    if (p & ~known) return -LX_EINVAL;
    return p & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC); /* == PROT_READ|PROT_WRITE|PROT_EXEC */
}

/* ---- signals ---------------------------------------------------------------------------------- */

static const struct { int lx, host; } sigmap[] = {
    { LX_SIGHUP, SIGHUP }, { LX_SIGINT, SIGINT }, { LX_SIGQUIT, SIGQUIT }, { LX_SIGILL, SIGILL },
    { LX_SIGTRAP, SIGTRAP }, { LX_SIGABRT, SIGABRT }, { LX_SIGBUS, SIGBUS }, { LX_SIGFPE, SIGFPE },
    { LX_SIGKILL, SIGKILL }, { LX_SIGUSR1, SIGUSR1 }, { LX_SIGSEGV, SIGSEGV }, { LX_SIGUSR2, SIGUSR2 },
    { LX_SIGPIPE, SIGPIPE }, { LX_SIGALRM, SIGALRM }, { LX_SIGTERM, SIGTERM }, { LX_SIGCHLD, SIGCHLD },
    { LX_SIGCONT, SIGCONT }, { LX_SIGSTOP, SIGSTOP }, { LX_SIGTSTP, SIGTSTP }, { LX_SIGTTIN, SIGTTIN },
    { LX_SIGTTOU, SIGTTOU }, { LX_SIGURG, SIGURG }, { LX_SIGXCPU, SIGXCPU }, { LX_SIGXFSZ, SIGXFSZ },
    { LX_SIGVTALRM, SIGVTALRM }, { LX_SIGPROF, SIGPROF }, { LX_SIGWINCH, SIGWINCH }, { LX_SIGIO, SIGIO },
    { LX_SIGSYS, SIGSYS },
};

int lkd_signo_to_host(int lx)
{
    for (size_t i = 0; i < sizeof sigmap / sizeof sigmap[0]; i++)
        if (sigmap[i].lx == lx) return sigmap[i].host;
    return 0;
}

int lkd_signo_from_host(int h)
{
    for (size_t i = 0; i < sizeof sigmap / sizeof sigmap[0]; i++)
        if (sigmap[i].host == h) return sigmap[i].lx;
    return 0;
}

/* ---- fcntl ------------------------------------------------------------------------------------ */

int lkd_fcntl_cmd_to_host(int lx, int *host)
{
    int h;
    switch (lx) {
    case LX_F_DUPFD: h = F_DUPFD; break;
    case LX_F_GETFD: h = F_GETFD; break;
    case LX_F_SETFD: h = F_SETFD; break;
    case LX_F_GETFL: h = F_GETFL; break;
    case LX_F_SETFL: h = F_SETFL; break;
    case LX_F_GETLK: h = F_GETLK; break;
    case LX_F_SETLK: h = F_SETLK; break;
    case LX_F_SETLKW: h = F_SETLKW; break;
    case LX_F_SETOWN: h = F_SETOWN; break;
    case LX_F_GETOWN: h = F_GETOWN; break;
    case LX_F_DUPFD_CLOEXEC: h = F_DUPFD_CLOEXEC; break;
#ifdef F_OFD_SETLK
    case LX_F_OFD_SETLK: h = F_OFD_SETLK; break;
    case LX_F_OFD_SETLKW: h = F_OFD_SETLKW; break;
    case LX_F_OFD_GETLK: h = F_OFD_GETLK; break;
#endif
    default: return -LX_EINVAL;
    }
    if (host) *host = h;
    return 0;
}

/* ---- sockets ---------------------------------------------------------------------------------- */

int lkd_af_to_host(int lx)
{
    switch (lx) {
    case LX_AF_UNSPEC: return AF_UNSPEC;
    case LX_AF_UNIX: return AF_UNIX;
    case LX_AF_INET: return AF_INET;
    case LX_AF_INET6: return AF_INET6;
    }
    return -LX_EAFNOSUPPORT;
}

int lkd_af_from_host(int h)
{
    switch (h) {
    case AF_UNSPEC: return LX_AF_UNSPEC;
    case AF_UNIX: return LX_AF_UNIX;
    case AF_INET: return LX_AF_INET;
    case AF_INET6: return LX_AF_INET6;
    }
    return -LX_EAFNOSUPPORT;
}

int lkd_socktype_to_host(int lx)
{
    switch (lx) {
    case LX_SOCK_STREAM: return SOCK_STREAM;
    case LX_SOCK_DGRAM: return SOCK_DGRAM;
    case LX_SOCK_RAW: return SOCK_RAW;
    case LX_SOCK_RDM: return SOCK_RDM;
    case LX_SOCK_SEQPACKET: return SOCK_SEQPACKET;
    }
    return -LX_EINVAL;
}

static const struct { int lx, host; } so_map[] = {
    { LX_SO_DEBUG, SO_DEBUG }, { LX_SO_REUSEADDR, SO_REUSEADDR }, { LX_SO_TYPE, SO_TYPE },
    { LX_SO_ERROR, SO_ERROR }, { LX_SO_DONTROUTE, SO_DONTROUTE }, { LX_SO_BROADCAST, SO_BROADCAST },
    { LX_SO_SNDBUF, SO_SNDBUF }, { LX_SO_RCVBUF, SO_RCVBUF }, { LX_SO_KEEPALIVE, SO_KEEPALIVE },
    { LX_SO_OOBINLINE, SO_OOBINLINE }, { LX_SO_LINGER, SO_LINGER }, { LX_SO_REUSEPORT, SO_REUSEPORT },
    { LX_SO_RCVLOWAT, SO_RCVLOWAT }, { LX_SO_SNDLOWAT, SO_SNDLOWAT }, { LX_SO_RCVTIMEO, SO_RCVTIMEO },
    { LX_SO_SNDTIMEO, SO_SNDTIMEO }, { LX_SO_ACCEPTCONN, SO_ACCEPTCONN },
};
static const struct { int lx, host; } tcp_map[] = {
    { LX_TCP_NODELAY, TCP_NODELAY }, { LX_TCP_MAXSEG, TCP_MAXSEG },
    { LX_TCP_KEEPIDLE, TCP_KEEPALIVE }, { LX_TCP_KEEPINTVL, TCP_KEEPINTVL }, { LX_TCP_KEEPCNT, TCP_KEEPCNT },
};
static const struct { int lx, host; } ip_map[] = {
    { LX_IP_TOS, IP_TOS }, { LX_IP_TTL, IP_TTL }, { LX_IP_MULTICAST_IF, IP_MULTICAST_IF },
    { LX_IP_MULTICAST_TTL, IP_MULTICAST_TTL }, { LX_IP_MULTICAST_LOOP, IP_MULTICAST_LOOP },
    { LX_IP_ADD_MEMBERSHIP, IP_ADD_MEMBERSHIP }, { LX_IP_DROP_MEMBERSHIP, IP_DROP_MEMBERSHIP },
};
static const struct { int lx, host; } ip6_map[] = {
    { LX_IPV6_UNICAST_HOPS, IPV6_UNICAST_HOPS }, { LX_IPV6_MULTICAST_IF, IPV6_MULTICAST_IF },
    { LX_IPV6_MULTICAST_HOPS, IPV6_MULTICAST_HOPS }, { LX_IPV6_MULTICAST_LOOP, IPV6_MULTICAST_LOOP },
    { LX_IPV6_V6ONLY, IPV6_V6ONLY },
};

#define TABLE_LOOKUP(tab, key, from, to, out) do { \
    for (size_t i_ = 0; i_ < sizeof(tab) / sizeof((tab)[0]); i_++) \
        if ((tab)[i_].from == (key)) { *(out) = (tab)[i_].to; return 0; } } while (0)

int lkd_sockopt_to_host(int lx_level, int lx_opt, int *host_level, int *host_opt)
{
    int dummy_l, dummy_o;
    if (!host_level) host_level = &dummy_l;
    if (!host_opt) host_opt = &dummy_o;
    switch (lx_level) {
    case LX_SOL_SOCKET: *host_level = SOL_SOCKET; TABLE_LOOKUP(so_map, lx_opt, lx, host, host_opt); break;
    case LX_IPPROTO_TCP: *host_level = IPPROTO_TCP; TABLE_LOOKUP(tcp_map, lx_opt, lx, host, host_opt); break;
    case LX_IPPROTO_IP: *host_level = IPPROTO_IP; TABLE_LOOKUP(ip_map, lx_opt, lx, host, host_opt); break;
    case LX_IPPROTO_IPV6: *host_level = IPPROTO_IPV6; TABLE_LOOKUP(ip6_map, lx_opt, lx, host, host_opt); break;
    }
    return -LX_ENOPROTOOPT;
}

int lkd_sockopt_from_host(int host_level, int host_opt, int *lx_level, int *lx_opt)
{
    int dl, dopt;
    if (!lx_level) lx_level = &dl;
    if (!lx_opt) lx_opt = &dopt;
    if (host_level == SOL_SOCKET) { *lx_level = LX_SOL_SOCKET; TABLE_LOOKUP(so_map, host_opt, host, lx, lx_opt); }
    else if (host_level == IPPROTO_TCP) { *lx_level = LX_IPPROTO_TCP; TABLE_LOOKUP(tcp_map, host_opt, host, lx, lx_opt); }
    else if (host_level == IPPROTO_IP) { *lx_level = LX_IPPROTO_IP; TABLE_LOOKUP(ip_map, host_opt, host, lx, lx_opt); }
    else if (host_level == IPPROTO_IPV6) { *lx_level = LX_IPPROTO_IPV6; TABLE_LOOKUP(ip6_map, host_opt, host, lx, lx_opt); }
    return -LX_ENOPROTOOPT;
}

static const struct { int lx, host; } msg_map[] = {
    { LX_MSG_OOB, MSG_OOB }, { LX_MSG_PEEK, MSG_PEEK }, { LX_MSG_DONTROUTE, MSG_DONTROUTE },
    { LX_MSG_CTRUNC, MSG_CTRUNC }, { LX_MSG_TRUNC, MSG_TRUNC }, { LX_MSG_DONTWAIT, MSG_DONTWAIT },
    { LX_MSG_EOR, MSG_EOR }, { LX_MSG_WAITALL, MSG_WAITALL },
};

int lkd_msgflags_to_host(int lx, int *untranslated)
{
    int h = 0, left = lx;
    for (size_t i = 0; i < sizeof msg_map / sizeof msg_map[0]; i++)
        if (left & msg_map[i].lx) { h |= msg_map[i].host; left &= ~msg_map[i].lx; }
    if (untranslated) *untranslated = left;
    return h;
}

int lkd_msgflags_from_host(int host)
{
    int lx = 0;
    for (size_t i = 0; i < sizeof msg_map / sizeof msg_map[0]; i++)
        if (host & msg_map[i].host) lx |= msg_map[i].lx;
    return lx;
}

/* ---- devices, stat ---------------------------------------------------------------------------- */

/* Linux new_encode_dev(MKDEV(major, minor)) with the 20-bit minor of kdev_t. */
static uint64_t encode_dev(unsigned major, unsigned minor)
{
    return (uint64_t)((minor & 0xff) | (major << 8) | ((minor & ~0xffu) << 12));
}

uint64_t lkd_dev_to_linux(dev_t d)
{
    return encode_dev((unsigned)major(d), (unsigned)minor(d));
}

dev_t lkd_dev_from_linux(uint64_t e)
{
    unsigned maj = (unsigned)((e & 0xfff00) >> 8);
    unsigned min = (unsigned)((e & 0xff) | ((e >> 12) & 0xfff00));
    return makedev(maj, min);
}

void lkd_stat_from_host(const struct stat *hs, struct lx_stat *ls)
{
    memset(ls, 0, sizeof *ls);
    ls->st_dev = lkd_dev_to_linux(hs->st_dev);
    ls->st_ino = hs->st_ino;
    ls->st_nlink = hs->st_nlink;
    ls->st_mode = hs->st_mode;
    ls->st_uid = hs->st_uid;
    ls->st_gid = hs->st_gid;
    ls->st_rdev = lkd_dev_to_linux(hs->st_rdev);
    ls->st_size = hs->st_size;
    ls->st_blksize = hs->st_blksize;
    ls->st_blocks = hs->st_blocks;
    ls->st_atime_sec = hs->st_atimespec.tv_sec;
    ls->st_atime_nsec = hs->st_atimespec.tv_nsec;
    ls->st_mtime_sec = hs->st_mtimespec.tv_sec;
    ls->st_mtime_nsec = hs->st_mtimespec.tv_nsec;
    ls->st_ctime_sec = hs->st_ctimespec.tv_sec;
    ls->st_ctime_nsec = hs->st_ctimespec.tv_nsec;
}

void lkd_statx_from_host(const struct stat *hs, uint32_t mask, struct lx_statx *x)
{
    (void)mask; /* Linux may return more than asked; we always fill everything we know */
    memset(x, 0, sizeof *x);
    x->stx_mask = LX_STATX_BASIC_STATS | LX_STATX_BTIME;
    x->stx_blksize = (uint32_t)hs->st_blksize;
    x->stx_nlink = hs->st_nlink;
    x->stx_uid = hs->st_uid;
    x->stx_gid = hs->st_gid;
    x->stx_mode = (uint16_t)hs->st_mode;
    x->stx_ino = hs->st_ino;
    x->stx_size = (uint64_t)hs->st_size;
    x->stx_blocks = (uint64_t)hs->st_blocks;
    x->stx_atime.tv_sec = hs->st_atimespec.tv_sec; x->stx_atime.tv_nsec = (uint32_t)hs->st_atimespec.tv_nsec;
    x->stx_btime.tv_sec = hs->st_birthtimespec.tv_sec; x->stx_btime.tv_nsec = (uint32_t)hs->st_birthtimespec.tv_nsec;
    x->stx_ctime.tv_sec = hs->st_ctimespec.tv_sec; x->stx_ctime.tv_nsec = (uint32_t)hs->st_ctimespec.tv_nsec;
    x->stx_mtime.tv_sec = hs->st_mtimespec.tv_sec; x->stx_mtime.tv_nsec = (uint32_t)hs->st_mtimespec.tv_nsec;
    x->stx_rdev_major = (uint32_t)major(hs->st_rdev);
    x->stx_rdev_minor = (uint32_t)minor(hs->st_rdev);
    x->stx_dev_major = (uint32_t)major(hs->st_dev);
    x->stx_dev_minor = (uint32_t)minor(hs->st_dev);
}

void lkd_statfs_from_host(const struct statfs *hs, struct lx_statfs *ls)
{
    memset(ls, 0, sizeof *ls);
    int64_t type = 0;
    if (!strcmp(hs->f_fstypename, "apfs")) type = 0x42535041;
    else if (!strcmp(hs->f_fstypename, "hfs")) type = 0x4244;
    else if (!strcmp(hs->f_fstypename, "msdos") || !strcmp(hs->f_fstypename, "exfat")) type = 0x4d44;
    else if (!strcmp(hs->f_fstypename, "nfs")) type = 0x6969;
    else if (!strcmp(hs->f_fstypename, "devfs")) type = 0x01021994;
    ls->f_type = type;
    ls->f_bsize = hs->f_bsize;
    ls->f_blocks = hs->f_blocks;
    ls->f_bfree = hs->f_bfree;
    ls->f_bavail = hs->f_bavail;
    ls->f_files = hs->f_files;
    ls->f_ffree = hs->f_ffree;
    ls->f_fsid[0] = hs->f_fsid.val[0];
    ls->f_fsid[1] = hs->f_fsid.val[1];
    ls->f_namelen = 255;
    ls->f_frsize = hs->f_bsize;
    int64_t fl = 0;
    if (hs->f_flags & MNT_RDONLY) fl |= 1;   /* ST_RDONLY */
    if (hs->f_flags & MNT_NOSUID) fl |= 2;   /* ST_NOSUID */
    if (hs->f_flags & MNT_NODEV) fl |= 4;    /* ST_NODEV */
    if (hs->f_flags & MNT_NOEXEC) fl |= 8;   /* ST_NOEXEC */
    ls->f_flags = fl;
}

uint8_t lkd_dtype_from_host(uint8_t d)
{
    switch (d) {
    case DT_FIFO: return LX_DT_FIFO;
    case DT_CHR: return LX_DT_CHR;
    case DT_DIR: return LX_DT_DIR;
    case DT_BLK: return LX_DT_BLK;
    case DT_REG: return LX_DT_REG;
    case DT_LNK: return LX_DT_LNK;
    case DT_SOCK: return LX_DT_SOCK;
    }
    return LX_DT_UNKNOWN;
}
