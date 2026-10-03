/* vfs.c: mount table, fsctx (cwd, root, umask) and rooted path resolution.
 *
 * Resolution walks the guest path component by component (imported from HL vfs.c
 * hl_vfs_resolve_at, adapted): every component is looked up with lstat on its host path (host mounts)
 * or getattr (virtual mounts); symlinks are read and their target is resolved in the GUEST namespace
 * (absolute targets restart at the task's root), so ".." after a link applies to the directory the link
 * led to, a chroot cannot be escaped by "..", and the host kernel never follows a link itself. Host
 * calls on the result use O_NOFOLLOW / lstat-style variants; a symlink that appears between the walk
 * and the host call makes that call fail (ELOOP) instead of escaping (TOCTOU limit, documented).
 *
 * Original copyright of the resolver loop:
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <fcntl.h>
#include <limits.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

#include "fs_priv.h"

/* ---- Mount table ------------------------------------------------------------------------------- */

static struct fs_mount g_mounts[FS_MAX_MOUNTS];
static _Atomic int g_nmounts;
static pthread_mutex_t g_mount_add_lock = PTHREAD_MUTEX_INITIALIZER;

int fs_mount_count(void) { return atomic_load_explicit(&g_nmounts, memory_order_acquire); }
const struct fs_mount *fs_mount_at(int i) { return (i >= 0 && i < fs_mount_count()) ? &g_mounts[i] : NULL; }

/* Canonical form of an absolute guest path given by the runtime: one '/' between components, no
 * trailing '/'. "." and ".." are refused (mount points are given canonically). */
static int canon_mountpoint(const char *in, char *out, size_t cap)
{
    if (!in || in[0] != '/') return -LX_EINVAL;
    size_t n = 0;
    out[n++] = '/';
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t l = (size_t)(p - s);
        if ((l == 1 && s[0] == '.') || (l == 2 && s[0] == '.' && s[1] == '.')) return -LX_EINVAL;
        if (n > 1) out[n++] = '/';
        if (n + l + 1 > cap) return -LX_ENAMETOOLONG;
        memcpy(out + n, s, l);
        n += l;
    }
    out[n] = 0;
    return 0;
}

static int prefix_match(const char *path, const char *mp, size_t len)
{
    if (len == 1) return path[0] == '/';
    return strncmp(path, mp, len) == 0 && (path[len] == 0 || path[len] == '/');
}

const struct fs_mount *fs_mount_find(const char *gpath)
{
    const struct fs_mount *best = NULL;
    int n = fs_mount_count();
    for (int i = 0; i < n; i++) {
        const struct fs_mount *m = &g_mounts[i];
        if (!m->gpath) continue;
        if (prefix_match(gpath, m->gpath, m->glen) && (!best || m->glen > best->glen)) best = m;
    }
    return best;
}

int fs_is_mount_point(const char *gpath)
{
    int n = fs_mount_count();
    for (int i = 0; i < n; i++)
        if (g_mounts[i].gpath && strcmp(g_mounts[i].gpath, gpath) == 0) return 1;
    return 0;
}

int fs_is_mount_ancestor(const char *gpath)
{
    size_t len = strlen(gpath);
    int n = fs_mount_count();
    for (int i = 0; i < n; i++) {
        const char *mp = g_mounts[i].gpath;
        if (!mp || strcmp(mp, gpath) == 0) continue;
        if (len == 1 ? mp[0] == '/' && mp[1] : (strncmp(mp, gpath, len) == 0 && mp[len] == '/')) return 1;
    }
    return 0;
}

static uint64_t lx_encode_dev(unsigned major, unsigned minor)
{
    return (minor & 0xffu) | ((uint64_t)(major & 0xfffu) << 8) | ((uint64_t)(minor & ~0xffu) << 12);
}

/* Add or replace (same mount point: the newest mount hides the old one, like Linux stacking). */
static int mount_add(const char *guest_path, int kind, const char *host_dir, int ro,
                     const lkd_vmount_ops *ops, void *priv)
{
    char gp[LX_PATH_MAX];
    int rc = canon_mountpoint(guest_path, gp, sizeof gp);
    if (rc < 0) return rc;
    char real[PATH_MAX];
    struct stat st;
    struct statfs sfs;
    if (kind == FS_MNT_HOST) {
        if (!realpath(host_dir, real)) return LKD_HOST_ERR();
        if (stat(real, &st) < 0) return LKD_HOST_ERR();
        if (!S_ISDIR(st.st_mode)) return -LX_ENOTDIR;
        if (statfs(real, &sfs) < 0) return LKD_HOST_ERR();
    }
    pthread_mutex_lock(&g_mount_add_lock);
    int n = atomic_load(&g_nmounts), idx = -1;
    for (int i = 0; i < n; i++)
        if (strcmp(g_mounts[i].gpath, gp) == 0) { idx = i; break; }
    if (idx >= 0 && kind == FS_MNT_VIRT && g_mounts[idx].kind == FS_MNT_VIRT) {
        pthread_mutex_unlock(&g_mount_add_lock);
        return -LX_EBUSY;
    }
    if (idx < 0 && n >= FS_MAX_MOUNTS) { pthread_mutex_unlock(&g_mount_add_lock); return -LX_ENOMEM; }
    struct fs_mount *m = idx >= 0 ? &g_mounts[idx] : &g_mounts[n];
    struct fs_mount nm = { 0 };
    nm.gpath = strdup(gp);
    nm.glen = strlen(gp);
    nm.kind = kind;
    nm.ro = ro;
    nm.id = idx >= 0 ? m->id : 21 + n;
    if (kind == FS_MNT_HOST) {
        nm.host = strdup(real);
        nm.lx_dev = lkd_dev_to_linux(st.st_dev);
        snprintf(nm.fstype, sizeof nm.fstype, "%s", sfs.f_fstypename);
    } else {
        nm.ops = ops;
        nm.priv = priv;
        nm.lx_dev = lx_encode_dev(0, (unsigned)(20 + (idx >= 0 ? idx : n)));
        snprintf(nm.fstype, sizeof nm.fstype, "%s", ops && ops->fstype ? ops->fstype : "none");
    }
    /* Old strings are leaked on replacement on purpose: concurrent lock-free readers may hold them. */
    *m = nm;
    if (idx < 0) atomic_store_explicit(&g_nmounts, n + 1, memory_order_release);
    pthread_mutex_unlock(&g_mount_add_lock);
    return 0;
}

int lkd_vfs_set_root(const char *host_dir) { return mount_add("/", FS_MNT_HOST, host_dir ? host_dir : "/", 0, NULL, NULL); }

void lkd_fs_set_root_ro(int ro)
{
    int n = fs_mount_count();
    for (int i = 0; i < n; i++)
        if (strcmp(g_mounts[i].gpath, "/") == 0) g_mounts[i].ro = ro;
}

int lkd_vfs_mount_bind(const char *guest_path, const char *host_dir, int ro)
{
    if (!host_dir) return -LX_EINVAL;
    return mount_add(guest_path, FS_MNT_HOST, host_dir, ro, NULL, NULL);
}

int lkd_vfs_mount_virtual(const char *guest_path, const lkd_vmount_ops *ops, void *priv)
{
    if (!ops || !ops->getattr) return -LX_EINVAL;
    return mount_add(guest_path, FS_MNT_VIRT, NULL, 0, ops, priv);
}

int lkd_fs_host_to_guest(const char *host_path, char *out, size_t cap)
{
    const struct fs_mount *best = NULL;
    size_t blen = 0;
    int n = fs_mount_count();
    for (int i = 0; i < n; i++) {
        const struct fs_mount *m = &g_mounts[i];
        if (m->kind != FS_MNT_HOST) continue;
        size_t hl = strlen(m->host);
        int match = (hl == 1) ? host_path[0] == '/'
                              : (strncmp(host_path, m->host, hl) == 0 && (host_path[hl] == 0 || host_path[hl] == '/'));
        if (match && (!best || hl > blen)) { best = m; blen = hl; }
    }
    if (!best) return -LX_ENOENT;
    const char *suffix = host_path + (blen == 1 ? 0 : blen);
    int w;
    if (!*suffix || (suffix[0] == '/' && !suffix[1])) w = snprintf(out, cap, "%s", best->gpath);
    else if (best->glen == 1) w = snprintf(out, cap, "%s", suffix);
    else w = snprintf(out, cap, "%s%s", best->gpath, suffix);
    return (w < 0 || (size_t)w >= cap) ? -LX_ENAMETOOLONG : 0;
}

/* ---- fsctx ------------------------------------------------------------------------------------- */

struct lkd_fsctx {
    _Atomic int refs;
    char *cwd;
    char *root;
    uint32_t umask;
};

/* One lock for every fsctx (rank LKD_RANK_VFS): operations are a few string copies. */
static pthread_mutex_t g_fsctx_lock = PTHREAD_MUTEX_INITIALIZER;

lkd_fsctx *lkd_fsctx_new(void)
{
    lkd_fsctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    atomic_init(&c->refs, 1);
    c->cwd = strdup("/");
    c->root = strdup("/");
    c->umask = 022;
    return c;
}

lkd_fsctx *lkd_fsctx_clone(lkd_fsctx *parent, uint64_t clone_flags)
{
    if (!parent) return lkd_fsctx_new();
    if (clone_flags & LX_CLONE_FS) { atomic_fetch_add(&parent->refs, 1); return parent; }
    lkd_fsctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    atomic_init(&c->refs, 1);
    pthread_mutex_lock(&g_fsctx_lock);
    c->cwd = strdup(parent->cwd);
    c->root = strdup(parent->root);
    c->umask = parent->umask;
    pthread_mutex_unlock(&g_fsctx_lock);
    return c;
}

void lkd_fsctx_put(lkd_fsctx *c)
{
    if (!c || atomic_fetch_sub(&c->refs, 1) != 1) return;
    free(c->cwd);
    free(c->root);
    free(c);
}

static void copy_field(lkd_task *t, int which, char *out, size_t cap)
{
    lkd_fsctx *c = t ? t->fsctx : NULL;
    if (!c) { snprintf(out, cap, "/"); return; }
    pthread_mutex_lock(&g_fsctx_lock);
    snprintf(out, cap, "%s", which ? c->root : c->cwd);
    pthread_mutex_unlock(&g_fsctx_lock);
}
void fs_get_cwd(lkd_task *t, char *out, size_t cap) { copy_field(t, 0, out, cap); }
void fs_get_root(lkd_task *t, char *out, size_t cap) { copy_field(t, 1, out, cap); }

static void set_field(lkd_task *t, int which, const char *v)
{
    lkd_fsctx *c = t ? t->fsctx : NULL;
    if (!c) return;
    char *n = strdup(v);
    if (!n) return;
    pthread_mutex_lock(&g_fsctx_lock);
    char **f = which ? &c->root : &c->cwd;
    char *old = *f;
    *f = n;
    pthread_mutex_unlock(&g_fsctx_lock);
    free(old);
}
void fs_set_cwd(lkd_task *t, const char *g) { set_field(t, 0, g); }
void fs_set_root(lkd_task *t, const char *g) { set_field(t, 1, g); }

uint32_t lkd_vfs_umask(lkd_task *t)
{
    lkd_fsctx *c = t ? t->fsctx : NULL;
    if (!c) return 022;
    pthread_mutex_lock(&g_fsctx_lock);
    uint32_t u = c->umask;
    pthread_mutex_unlock(&g_fsctx_lock);
    return u;
}

/* umask(2): set and return the previous value. */
uint32_t fs_swap_umask(lkd_task *t, uint32_t nu);
uint32_t fs_swap_umask(lkd_task *t, uint32_t nu)
{
    lkd_fsctx *c = t ? t->fsctx : NULL;
    if (!c) return 022;
    pthread_mutex_lock(&g_fsctx_lock);
    uint32_t old = c->umask;
    c->umask = nu & 0777;
    pthread_mutex_unlock(&g_fsctx_lock);
    return old;
}

void lkd_fs_fsctx_fork_prepare(void) { pthread_mutex_lock(&g_fsctx_lock); pthread_mutex_lock(&g_mount_add_lock); }
void lkd_fs_fsctx_fork_parent(void) { pthread_mutex_unlock(&g_mount_add_lock); pthread_mutex_unlock(&g_fsctx_lock); }
void lkd_fs_fsctx_fork_child(void)
{
    lkd_fork_reinit_mutex(&g_fsctx_lock, 0);
    lkd_fork_reinit_mutex(&g_mount_add_lock, 0);
}

/* Linux getcwd(2) (fs/d_path.c): "/path" relative to the root; "(unreachable)/path" when the cwd is
 * outside the root; -ENOENT when the directory was removed. */
int lkd_vfs_getcwd(lkd_task *t, char *out, size_t cap)
{
    char cwd[LX_PATH_MAX], root[LX_PATH_MAX], tmp[LX_PATH_MAX + 16];
    fs_get_cwd(t, cwd, sizeof cwd);
    fs_get_root(t, root, sizeof root);
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, LX_AT_FDCWD, cwd, FS_R_FOLLOW | FS_R_GLOBAL, r);
    int gone = rc < 0 || !r->exists || strcmp(r->gpath, cwd) != 0;
    free(r);
    if (gone) return -LX_ENOENT;
    size_t rl = strlen(root);
    if (rl == 1) snprintf(tmp, sizeof tmp, "%s", cwd);
    else if (strncmp(cwd, root, rl) == 0 && (cwd[rl] == 0 || cwd[rl] == '/'))
        snprintf(tmp, sizeof tmp, "%s", cwd[rl] ? cwd + rl : "/");
    else snprintf(tmp, sizeof tmp, "(unreachable)%s", cwd);
    size_t n = strlen(tmp);
    if (n + 1 > cap) return -LX_ERANGE;
    memcpy(out, tmp, n + 1);
    return (int)n;
}

/* ---- Lookup of one canonical guest path -------------------------------------------------------- */

static void host_of(const struct fs_mount *m, const char *gpath, char *out, size_t cap)
{
    const char *suffix = m->glen == 1 ? gpath : gpath + m->glen; /* "" or "/..." */
    if (m->host[0] == '/' && m->host[1] == 0) snprintf(out, cap, "%s", suffix[0] ? suffix : "/");
    else snprintf(out, cap, "%s%s", m->host, suffix);
}

static const char *rel_of(const struct fs_mount *m, const char *gpath)
{
    const char *s = m->glen == 1 ? gpath : gpath + m->glen;
    while (*s == '/') s++;
    return s;
}

/* Fill r for gpath (exists or not). Returns 0 (r->exists tells), or -errno other than ENOENT. */
static int lookup(lkd_task *t, const char *gpath, struct fs_res *r)
{
    size_t gl = strlen(gpath);
    if (gl >= sizeof r->gpath) return -LX_ENAMETOOLONG;
    memcpy(r->gpath, gpath, gl + 1);
    r->exists = 0;
    r->synthetic = 0;
    r->magic = 0;
    r->host[0] = 0;
    r->rel = NULL;
    const struct fs_mount *m = fs_mount_find(gpath);
    r->mnt = m;
    if (!m) {
        if (fs_is_mount_ancestor(gpath)) { r->exists = 1; r->synthetic = 1; }
        return 0;
    }
    if (m->kind == FS_MNT_HOST) {
        host_of(m, gpath, r->host, sizeof r->host);
        if (strlen(r->host) >= PATH_MAX) return -LX_ENAMETOOLONG; /* host PATH_MAX is 1024 */
        if (lstat(r->host, &r->st) == 0) { r->exists = 1; return 0; }
        int e = errno;
        if (e == ENOENT && fs_is_mount_ancestor(gpath)) { r->exists = 1; r->synthetic = 1; return 0; }
        if (e == ENOENT) return 0;
        return -lkd_errno_from_darwin(e);
    }
    r->rel = rel_of(m, r->gpath);
    memset(&r->va, 0, sizeof r->va);
    int rc = m->ops->getattr(t, m->priv, r->rel, 1, &r->va);
    if (rc == 0) { r->exists = 1; return 0; }
    if (rc == -LX_ENOENT && fs_is_mount_ancestor(gpath)) { r->exists = 1; r->synthetic = 1; return 0; }
    return rc == -LX_ENOENT ? 0 : rc;
}

uint32_t fs_res_mode(const struct fs_res *r)
{
    if (r->synthetic) return LX_S_IFDIR | 0555;
    if (r->mnt && r->mnt->kind == FS_MNT_HOST) return (uint32_t)r->st.st_mode;
    return r->va.mode;
}

static int res_is_link(const struct fs_res *r) { return !r->synthetic && LX_S_ISLNK(fs_res_mode(r)); }

static int64_t res_readlink(lkd_task *t, const struct fs_res *r, char *buf, size_t cap)
{
    if (r->mnt->kind == FS_MNT_HOST) {
        ssize_t n = readlink(r->host, buf, cap);
        return n < 0 ? LKD_HOST_ERR() : n;
    }
    if (!r->mnt->ops->readlink) return -LX_EINVAL;
    return r->mnt->ops->readlink(t, r->mnt->priv, r->rel, buf, cap);
}

int fs_res_stat(lkd_task *t, const struct fs_res *r, struct lx_stat *out)
{
    (void)t;
    if (!r->exists) return -LX_ENOENT;
    if (r->synthetic) {
        struct lkd_vattr va = { .mode = LX_S_IFDIR | 0555, .nlink = 2 };
        uint64_t h = 1469598103934665603ull;
        for (const char *p = r->gpath; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        va.ino = h | 1;
        fs_stat_from_vattr(r->mnt, &va, out);
        return 0;
    }
    if (r->mnt->kind == FS_MNT_HOST) { fs_stat_from_host(&r->st, out); return 0; }
    fs_stat_from_vattr(r->mnt, &r->va, out);
    return 0;
}

int fs_res_writable_host(const struct fs_res *r)
{
    if (!r->mnt || r->synthetic || r->mnt->kind != FS_MNT_HOST) return -LX_EACCES;
    if (r->mnt->ro) return -LX_EROFS;
    return 0;
}

/* ---- Resolution -------------------------------------------------------------------------------- */

static void parent_of(char *p)
{
    char *s = strrchr(p, '/');
    if (!s || s == p) { p[0] = '/'; p[1] = 0; }
    else *s = 0;
}

static int join(char *dst, size_t cap, const char *a, const char *b)
{
    int n = (a[0] == '/' && a[1] == 0) ? snprintf(dst, cap, "/%s", b) : snprintf(dst, cap, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= cap) ? -LX_ENAMETOOLONG : 0;
}

/* openat2 RESOLVE_* restrictions for the resolutions of the calling thread (fs_restrict_set). */
static _Thread_local const struct fs_restrict *tl_restrict;
void fs_restrict_set(const struct fs_restrict *rs) { tl_restrict = rs; }

/* imported from HL vfs.c (hl_vfs_resolve_at), adapted: per-task root and cwd, virtual mounts, magic
 * links, trailing-slash rules of Linux path_lookupat. */
int fs_resolve(lkd_task *t, int dirfd, const char *path, unsigned flags, struct fs_res *r)
{
    if (!path[0]) return -LX_ENOENT;
    char root[LX_PATH_MAX], resolved[LX_PATH_MAX], cand[LX_PATH_MAX];
    char pending[2 * LX_PATH_MAX], remainder[2 * LX_PATH_MAX], target[LX_PATH_MAX];
    const struct fs_restrict *rs = tl_restrict;
    if (flags & FS_R_GLOBAL) { root[0] = '/'; root[1] = 0; rs = NULL; }
    else fs_get_root(t, root, sizeof root);
    if (rs && (rs->resolve & (FS_RESOLVE_BENEATH | FS_RESOLVE_IN_ROOT))) {
        /* the starting directory becomes the root of this lookup */
        if (dirfd == LX_AT_FDCWD) fs_get_cwd(t, root, sizeof root);
        else {
            lkd_fdobj *o = lkd_fd_get(t, dirfd, NULL);
            if (!o) return -LX_EBADF;
            int rc = fs_dirobj_gpath(o, root, sizeof root);
            lkd_fdobj_put(o);
            if (rc < 0) return rc;
        }
        if ((rs->resolve & FS_RESOLVE_BENEATH) && path[0] == '/') return -LX_EXDEV;
    }
    if (path[0] == '/') {
        snprintf(resolved, sizeof resolved, "%s", root);
    } else if (dirfd == LX_AT_FDCWD) {
        fs_get_cwd(t, resolved, sizeof resolved);
    } else {
        lkd_fdobj *o = lkd_fd_get(t, dirfd, NULL);
        if (!o) return -LX_EBADF;
        int rc = fs_dirobj_gpath(o, resolved, sizeof resolved);
        lkd_fdobj_put(o);
        if (rc < 0) return rc;
    }
    if (strlen(path) >= sizeof pending) return -LX_ENAMETOOLONG;
    snprintf(pending, sizeof pending, "%s", path);
    unsigned links = 0;
    int looked = 0; /* r describes `resolved` */
    r->trailing_slash = 0;
    for (;;) {
        char *comp = pending;
        while (*comp == '/') comp++;
        if (!*comp) break;
        char *slash = strchr(comp, '/');
        int had_slash = slash != NULL;
        char *rest = slash ? slash + 1 : comp + strlen(comp);
        while (*rest == '/') rest++;
        int is_final = *rest == 0;
        snprintf(remainder, sizeof remainder, "%s", rest);
        if (slash) *slash = 0;
        if (is_final) r->trailing_slash = had_slash;

        if (strcmp(comp, ".") == 0) {
            memmove(pending, remainder, strlen(remainder) + 1);
            continue;
        }
        if (strcmp(comp, "..") == 0) {
            if (rs && (rs->resolve & FS_RESOLVE_BENEATH) && strcmp(resolved, root) == 0) return -LX_EXDEV;
            if (strcmp(resolved, root) != 0) parent_of(resolved);
            looked = 0;
            memmove(pending, remainder, strlen(remainder) + 1);
            continue;
        }
        if (strlen(comp) > LX_NAME_MAX) return -LX_ENAMETOOLONG;
        int rc = join(cand, sizeof cand, resolved, comp);
        if (rc < 0) return rc;
        rc = lookup(t, cand, r);
        if (rc < 0) return rc;
        if (rs && (rs->resolve & FS_RESOLVE_NO_XDEV) && r->mnt != fs_mount_find(resolved)) return -LX_EXDEV;
        if (!r->exists) {
            if (is_final && (flags & FS_R_CREATE)) { snprintf(resolved, sizeof resolved, "%s", cand); looked = 1; break; }
            return -LX_ENOENT;
        }
        if (res_is_link(r) && (!is_final || had_slash || (flags & FS_R_FOLLOW))) {
            if (++links > FS_MAXSYMLINKS) return -LX_ELOOP;
            if (rs && (rs->resolve & FS_RESOLVE_NO_SYMLINKS)) return -LX_ELOOP;
            if (rs && (rs->resolve & FS_RESOLVE_NO_MAGICLINKS) && r->mnt->kind == FS_MNT_VIRT && r->va.magic_link)
                return -LX_ELOOP;
            if (r->mnt->kind == FS_MNT_VIRT && r->va.magic_link && is_final && (flags & FS_R_FOLLOW) &&
                !(flags & FS_R_NOMAGIC)) {
                r->magic = 1;
                snprintf(resolved, sizeof resolved, "%s", cand);
                looked = 1;
                break;
            }
            int64_t n = res_readlink(t, r, target, sizeof target - 1);
            if (n < 0) return (int)n;
            if ((size_t)n >= sizeof target - 1) return -LX_ENAMETOOLONG;
            if (n == 0) return -LX_ENOENT;
            target[n] = 0;
            if (r->mnt->kind == FS_MNT_VIRT && r->va.magic_link && target[0] != '/') return -LX_ENOTDIR;
            if (target[0] == '/') {
                if (rs && (rs->resolve & FS_RESOLVE_BENEATH)) return -LX_EXDEV;
                snprintf(resolved, sizeof resolved, "%s", root);
            }
            int w = snprintf(pending, sizeof pending, "%s%s%s", target, (had_slash || !is_final) ? "/" : "", remainder);
            if (w < 0 || (size_t)w >= sizeof pending) return -LX_ENAMETOOLONG;
            looked = 0;
            continue;
        }
        if ((!is_final || had_slash) && !LX_S_ISDIR(fs_res_mode(r))) return -LX_ENOTDIR;
        snprintf(resolved, sizeof resolved, "%s", cand);
        looked = 1;
        memmove(pending, remainder, strlen(remainder) + 1);
    }
    if (!looked) {
        int rc = lookup(t, resolved, r);
        if (rc < 0) return rc;
        if (!r->exists) return -LX_ENOENT;
        if (!LX_S_ISDIR(fs_res_mode(r))) return -LX_ENOTDIR;
    }
    return 0;
}

/* ---- Path API of vfs.h ------------------------------------------------------------------------- */

int64_t lkd_vfs_readlink(lkd_task *t, int dirfd, const char *gpath, char *buf, size_t cap)
{
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int64_t rc = fs_resolve(t, dirfd, gpath, 0, r);
    if (rc == 0) {
        if (!res_is_link(r)) rc = -LX_EINVAL;
        else rc = res_readlink(t, r, buf, cap);
    }
    free(r);
    return rc;
}

int lkd_vfs_realpath(lkd_task *t, int dirfd, const char *gpath, char *out, size_t cap)
{
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    int rc = fs_resolve(t, dirfd, gpath, FS_R_FOLLOW | FS_R_NOMAGIC, r);
    if (rc == 0) {
        size_t n = strlen(r->gpath);
        if (n + 1 > cap) rc = -LX_ENAMETOOLONG;
        else { memcpy(out, r->gpath, n + 1); rc = (int)n; }
    }
    free(r);
    return rc;
}

int lkd_vfs_to_host(lkd_task *t, int dirfd, const char *gpath, int flags, char *host_out, size_t cap)
{
    struct fs_res *r = malloc(sizeof *r);
    if (!r) return -LX_ENOMEM;
    unsigned rf = (flags & LX_AT_SYMLINK_NOFOLLOW) ? 0 : FS_R_FOLLOW;
    int rc = fs_resolve(t, dirfd, gpath, rf | FS_R_NOMAGIC, r);
    if (rc == 0 && !r->exists) rc = -LX_ENOENT;
    if (rc == 0 && (r->synthetic || r->mnt->kind != FS_MNT_HOST)) rc = -LX_EXDEV;
    if (rc == 0) {
        size_t n = strlen(r->host);
        if (n + 1 > cap) rc = -LX_ENAMETOOLONG;
        else memcpy(host_out, r->host, n + 1);
    }
    free(r);
    return rc;
}

/* /proc/mounts (fs/proc_namespace.c show_vfsmnt) and mountinfo (show_mountinfo). The source field of a
 * host mount is "hostfs" and its type is the host file system name (apfs): what really serves it. */
char *lkd_vfs_render_mounts(lkd_task *t, int mountinfo, size_t *len_out)
{
    (void)t;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { if (len_out) *len_out = 0; return NULL; }
    buf[0] = 0;
    int n = fs_mount_count();
    for (int i = 0; i < n; i++) {
        const struct fs_mount *m = &g_mounts[i];
        char line[LX_PATH_MAX + 256];
        const char *opts = m->ro ? "ro" : "rw";
        const char *src = m->kind == FS_MNT_HOST ? "hostfs" : m->fstype;
        int w;
        if (!mountinfo) {
            w = snprintf(line, sizeof line, "%s %s %s %s,relatime 0 0\n", src, m->gpath, m->fstype, opts);
        } else {
            /* parent: the mount that contains the mount point's parent (the root is its own parent). */
            int parent = m->id;
            if (m->glen > 1) {
                char pp[LX_PATH_MAX];
                snprintf(pp, sizeof pp, "%s", m->gpath);
                parent_of(pp);
                const struct fs_mount *pm = fs_mount_find(pp);
                if (pm) parent = pm->id;
            }
            unsigned maj = (unsigned)((m->lx_dev >> 8) & 0xfff), min = (unsigned)((m->lx_dev & 0xff) | ((m->lx_dev >> 12) & ~0xffull));
            w = snprintf(line, sizeof line, "%d %d %u:%u / %s %s,relatime - %s %s %s\n", m->id, parent, maj, min,
                         m->gpath, opts, m->fstype, src, opts);
        }
        if (w < 0) continue;
        if (len + (size_t)w + 1 > cap) {
            while (len + (size_t)w + 1 > cap) cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        memcpy(buf + len, line, (size_t)w + 1);
        len += (size_t)w;
    }
    if (len_out) *len_out = len;
    return buf;
}
