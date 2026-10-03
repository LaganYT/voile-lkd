/* unixaddr.c: socket address translation between the Linux layout (u16 family) and Darwin (u8 len,
 * u8 family), AF_UNIX paths through the vfs, and the abstract namespace.
 *
 * AF_UNIX paths. A guest path is resolved by the vfs (rooted, binds, symlinks) to a host path. Linux
 * sun_path holds 108 bytes, Darwin 104; a host path that does not fit (the guest root adds a prefix) is
 * reached by opening its directory and binding or connecting the relative leaf with that directory as
 * the calling thread's working directory (pthread_fchdir_np, per thread). The name getsockname reports
 * is the one the guest gave (unix_getname).
 *
 * Abstract names (sun_path[0] == 0) have no Darwin equivalent; they become socket files in a private
 * per-user directory, one file per name (FNV-1a of the exact bytes), guarded by a flock'ed lock file so
 * that a second bind gets EADDRINUSE while the first owner lives. A "<hash>.name" file keeps the name
 * so that getpeername/accept/recvfrom can report it.
 */
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>

#include "net_priv.h"
#include "lkd_fs.h"

extern int pthread_fchdir_np(int fd); /* Darwin per-thread working directory (libSystem, no header) */

/* ---- Abstract namespace ------------------------------------------------------------------------ */

/* imported from HL syscall_net.c (hl_abstract_dir, abstract_to_path), adapted: LKD prefix, the
 * result is cached per process only after the ownership check passed. */
static const char *abstract_dir(void)
{
    static char dir[256];
    const char *base = getenv("TMPDIR");
    if (!base || base[0] != '/') base = "/tmp";
    size_t bl = strlen(base);
    int n = snprintf(dir, sizeof dir, "%s%slkd-abstract-%u", base, (bl && base[bl - 1] == '/') ? "" : "/",
                     (unsigned)geteuid());
    if (n < 0 || (size_t)n >= sizeof dir) return NULL;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return NULL;
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
        lkd_log("net: refusing abstract socket directory %s (not private)", dir);
        return NULL;
    }
    return dir;
}

static int abstract_to_path(const uint8_t *name, uint32_t name_len, char *out, size_t out_sz)
{
    const char *dir = abstract_dir();
    if (!dir) return -1;
    uint64_t h = 1469598103934665603ULL;
    h = (h ^ (uint64_t)name_len) * 1099511628211ULL;
    for (uint32_t i = 0; i < name_len; i++) h = (h ^ name[i]) * 1099511628211ULL;
    int n = snprintf(out, out_sz, "%s/%016llx", dir, (unsigned long long)h);
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return n;
}
/* end of import */

int net_abstract_bind_lock(struct net_sock *sk, const char *host_path, const uint8_t *name, uint32_t name_len)
{
    char lk[300];
    snprintf(lk, sizeof lk, "%s.lk", host_path);
    int fd = open(lk, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return (int)LKD_HOST_ERR();
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -LX_EADDRINUSE;                        /* a live socket owns the name */
    }
    unlink(host_path);                                /* stale stand-in of a dead owner */
    char nm[300];
    snprintf(nm, sizeof nm, "%s.name", host_path);
    int nf = open(nm, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (nf >= 0) { (void)!write(nf, name, name_len); close(nf); }
    sk->abs_lock_fd = fd;
    sk->abs_host_path = strdup(host_path);
    return 0;
}

/* The name is free when no description holds the lock any more (a fork child shares the lock through
 * the inherited descriptor): only then are the stand-in files removed. */
void net_abstract_release(struct net_sock *sk)
{
    if (sk->abs_lock_fd < 0) return;
    close(sk->abs_lock_fd);
    sk->abs_lock_fd = -1;
    if (!sk->abs_host_path) return;
    char lk[300], nm[300];
    snprintf(lk, sizeof lk, "%s.lk", sk->abs_host_path);
    snprintf(nm, sizeof nm, "%s.name", sk->abs_host_path);
    int fd = open(lk, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            unlink(sk->abs_host_path);
            unlink(nm);
        }
        close(fd);
    }
    free(sk->abs_host_path);
    sk->abs_host_path = NULL;
}

int net_autobind_name(uint8_t *name, uint32_t *len)
{
    static _Atomic uint32_t ctr;
    uint32_t v = (atomic_fetch_add(&ctr, 1) * 2654435761u + (uint32_t)getpid() * 40503u) & 0xfffff;
    uint16_t fam = LX_AF_UNIX;
    memcpy(name, &fam, 2);
    name[2] = 0;
    char hex[8];
    snprintf(hex, sizeof hex, "%05x", v);
    memcpy(name + 3, hex, 5);
    *len = 8;                                          /* offsetof(sun_path) + 1 + 5 */
    return 0;
}

/* ---- Guest to host ----------------------------------------------------------------------------- */

static int split_path(const char *p, char *dir, size_t dcap, char *leaf, size_t lcap)
{
    const char *s = strrchr(p, '/');
    if (!s) {
        snprintf(dir, dcap, ".");
        if ((size_t)snprintf(leaf, lcap, "%s", p) >= lcap) return -LX_ENAMETOOLONG;
    } else {
        size_t dl = (size_t)(s - p);
        if (dl == 0) dl = 1;                           /* "/x": the root */
        if (dl >= dcap) return -LX_ENAMETOOLONG;
        memcpy(dir, p, dl);
        dir[dl] = 0;
        if ((size_t)snprintf(leaf, lcap, "%s", s + 1) >= lcap) return -LX_ENAMETOOLONG;
    }
    return leaf[0] ? 0 : -LX_ENOENT;
}

/* Fill ha with a host AF_UNIX address for host_path (full when it fits, else dir fd + leaf). */
static int unix_host_addr(const char *host_path, struct net_haddr *ha)
{
    struct sockaddr_un *un = (struct sockaddr_un *)&ha->ss;
    un->sun_family = AF_UNIX;
    size_t l = strlen(host_path);
    if (l < sizeof un->sun_path) {
        memcpy(un->sun_path, host_path, l + 1);
    } else {
        char dir[LX_PATH_MAX], leaf[sizeof un->sun_path];
        int r = split_path(host_path, dir, sizeof dir, leaf, sizeof leaf);
        if (r < 0) return r;
        ha->dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (ha->dirfd < 0) return (int)LKD_HOST_ERR();
        memcpy(un->sun_path, leaf, strlen(leaf) + 1);
        l = strlen(leaf);
    }
    un->sun_len = (uint8_t)(offsetof(struct sockaddr_un, sun_path) + l + 1);
    ha->len = un->sun_len;
    return 0;
}

int net_addr_from_bytes(lkd_task *t, struct net_sock *sk, const uint8_t *raw, uint32_t len, int for_bind, struct net_haddr *ha)
{
    memset(ha, 0, sizeof *ha);
    ha->dirfd = -1;
    if (len > sizeof(struct lx_sockaddr_storage)) return -LX_EINVAL;
    if (len < 2) return -LX_EINVAL;
    uint16_t fam;
    memcpy(&fam, raw, 2);
    switch (sk->domain) {
    case LX_AF_UNIX: {
        if (fam != LX_AF_UNIX) return -LX_EINVAL;           /* unix_validate_addr */
        if (len <= 2) return -LX_EINVAL;
        if (len > sizeof(struct lx_sockaddr_un)) return -LX_EINVAL;
        if (raw[2] == 0) {
            if (abstract_to_path(raw + 3, len - 3, ha->abs_host, sizeof ha->abs_host) < 0) return -LX_ENOMEM;
            ha->abstract = 1;
            return unix_host_addr(ha->abs_host, ha);
        }
        char gp[sizeof(struct lx_sockaddr_un)];
        size_t pl = strnlen((const char *)raw + 2, len - 2);
        memcpy(gp, raw + 2, pl);
        gp[pl] = 0;
        char host[LX_PATH_MAX];
        int r;
        if (for_bind) {
            char dir[sizeof gp + 2], leaf[sizeof gp];
            if ((r = split_path(gp, dir, sizeof dir, leaf, sizeof leaf)) < 0) return r;
            char hdir[LX_PATH_MAX];
            r = lkd_vfs_to_host(t, LX_AT_FDCWD, dir, 0, hdir, sizeof hdir);
            if (r == -LX_EXDEV) return -LX_EACCES;           /* a virtual tree (/proc) cannot hold sockets */
            if (r < 0) return r;
            if ((size_t)snprintf(host, sizeof host, "%s/%s", strcmp(hdir, "/") ? hdir : "", leaf) >= sizeof host)
                return -LX_ENAMETOOLONG;
        } else {
            r = lkd_vfs_to_host(t, LX_AT_FDCWD, gp, 0, host, sizeof host);
            if (r == -LX_EXDEV) return -LX_ECONNREFUSED;     /* not a socket */
            if (r < 0) return r;
        }
        return unix_host_addr(host, ha);
    }
    case LX_AF_INET: {
        /* inet_bind / tcp_v4_connect: shorter than sockaddr_in is -EINVAL, another family -EAFNOSUPPORT
         * (AF_UNSPEC with INADDR_ANY is accepted by bind for compatibility) */
        if (len < sizeof(struct lx_sockaddr_in)) return -LX_EINVAL;
        struct lx_sockaddr_in in;
        memcpy(&in, raw, sizeof in);
        if (fam != LX_AF_INET && !(for_bind && fam == LX_AF_UNSPEC && in.sin_addr == 0)) return -LX_EAFNOSUPPORT;
        struct sockaddr_in *h = (struct sockaddr_in *)&ha->ss;
        h->sin_len = sizeof *h;
        h->sin_family = AF_INET;
        h->sin_port = in.sin_port;
        h->sin_addr.s_addr = in.sin_addr;
        ha->len = sizeof *h;
        return 0;
    }
    case LX_AF_INET6: {
        if (len < 24) return -LX_EINVAL;                     /* SIN6_LEN_RFC2133 */
        if (fam != LX_AF_INET6) return -LX_EAFNOSUPPORT;
        struct lx_sockaddr_in6 in6 = { 0 };
        memcpy(&in6, raw, len < sizeof in6 ? len : sizeof in6);
        struct sockaddr_in6 *h = (struct sockaddr_in6 *)&ha->ss;
        h->sin6_len = sizeof *h;
        h->sin6_family = AF_INET6;
        h->sin6_port = in6.sin6_port;
        h->sin6_flowinfo = in6.sin6_flowinfo;
        memcpy(&h->sin6_addr, in6.sin6_addr, 16);
        h->sin6_scope_id = len >= sizeof in6 ? in6.sin6_scope_id : 0;
        ha->len = sizeof *h;
        return 0;
    }
    }
    return -LX_EAFNOSUPPORT;
}

int net_addr_from_guest(lkd_task *t, struct net_sock *sk, uint64_t gva, uint32_t len, int for_bind, struct net_haddr *ha)
{
    uint8_t raw[sizeof(struct lx_sockaddr_storage)];
    memset(ha, 0, sizeof *ha);
    ha->dirfd = -1;
    if (len > sizeof raw) return -LX_EINVAL;
    if (len && lkd_user_read(gva, raw, len) < 0) return -LX_EFAULT;
    return net_addr_from_bytes(t, sk, raw, len, for_bind, ha);
}

void net_haddr_done(struct net_haddr *ha)
{
    if (ha->dirfd >= 0) close(ha->dirfd);
    ha->dirfd = -1;
}

int net_with_dir(struct net_haddr *ha, int (*fn)(void *), void *arg)
{
    if (ha->dirfd < 0) return fn(arg);
    if (pthread_fchdir_np(ha->dirfd) != 0) return -1;
    int r = fn(arg);
    int e = errno;
    pthread_fchdir_np(-1);
    errno = e;
    return r;
}

/* ---- Host to guest ----------------------------------------------------------------------------- */

uint32_t net_addr_to_linux(const struct sockaddr *sa, socklen_t len, uint8_t *out, uint32_t cap)
{
    memset(out, 0, cap);
    if (len < 2) return 0;
    switch (sa->sa_family) {
    case AF_INET: {
        const struct sockaddr_in *h = (const struct sockaddr_in *)sa;
        struct lx_sockaddr_in in = { LX_AF_INET, h->sin_port, h->sin_addr.s_addr, { 0 } };
        memcpy(out, &in, sizeof in);
        return sizeof in;
    }
    case AF_INET6: {
        const struct sockaddr_in6 *h = (const struct sockaddr_in6 *)sa;
        struct lx_sockaddr_in6 in6 = { LX_AF_INET6, h->sin6_port, h->sin6_flowinfo, { 0 }, h->sin6_scope_id };
        memcpy(in6.sin6_addr, &h->sin6_addr, 16);
        memcpy(out, &in6, sizeof in6);
        return sizeof in6;
    }
    case AF_UNIX: {
        const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
        uint16_t fam = LX_AF_UNIX;
        memcpy(out, &fam, 2);
        size_t max = len > offsetof(struct sockaddr_un, sun_path) ? len - offsetof(struct sockaddr_un, sun_path) : 0;
        if (max > sizeof un->sun_path) max = sizeof un->sun_path;
        char hp[sizeof un->sun_path + 1];
        size_t pl = strnlen(un->sun_path, max);
        memcpy(hp, un->sun_path, pl);
        hp[pl] = 0;
        if (!pl) return 2;                                  /* unbound: unix_getname gives only the family */
        const char *ad = abstract_dir();
        size_t adl = ad ? strlen(ad) : 0;
        if (ad && !strncmp(hp, ad, adl) && hp[adl] == '/') {
            char nm[300];
            snprintf(nm, sizeof nm, "%s.name", hp);
            int fd = open(nm, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t n = read(fd, out + 3, cap - 3);
                close(fd);
                if (n >= 0) { out[2] = 0; return (uint32_t)(3 + n); }
            }
        }
        char g[LX_PATH_MAX];
        const char *name = hp;
        if (hp[0] == '/' && lkd_fs_host_to_guest(hp, g, sizeof g) == 0) {
            name = g;
            while (name[0] == '/' && name[1] == '/') name++;   /* the root mount maps to "//x" */
        }
        size_t gl = strlen(name);
        if (gl > sizeof(((struct lx_sockaddr_un *)0)->sun_path) - 1) gl = sizeof(((struct lx_sockaddr_un *)0)->sun_path) - 1;
        memcpy(out + 2, name, gl);
        out[2 + gl] = 0;
        return (uint32_t)(2 + gl + 1);
    }
    }
    return 0;
}

/* move_addr_to_user: copy min(ulen, klen) bytes, store klen; negative ulen is -EINVAL. */
int net_put_addr(uint64_t addr_gva, uint64_t len_gva, const uint8_t *lx, uint32_t lxlen)
{
    if (!len_gva) return addr_gva ? -LX_EFAULT : 0;
    int32_t ulen;
    if (lkd_user_read(len_gva, &ulen, sizeof ulen) < 0) return -LX_EFAULT;
    if (ulen < 0) return -LX_EINVAL;
    uint32_t n = (uint32_t)ulen < lxlen ? (uint32_t)ulen : lxlen;
    if (n && lkd_user_write(addr_gva, lx, n) < 0) return -LX_EFAULT;
    int32_t kl = (int32_t)lxlen;
    if (lkd_user_write(len_gva, &kl, sizeof kl) < 0) return -LX_EFAULT;
    return 0;
}
