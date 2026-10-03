/* dir.c: getdents/getdents64 on host, virtual and synthetic directories.
 *
 * The first getdents after open (or after lseek to 0) takes a snapshot of the directory: host entries
 * (readdir on a dup of the directory fd), or the virtual mount's readdir preceded by "." and "..", plus
 * every mount point directly below this directory that the listing lacks. d_off of an entry is the
 * index of the next one, so lseek(fd, d_off, SEEK_SET) resumes after it (fs/readdir.c, filldir64).
 * Formats: linux_dirent64 {d_ino, d_off, d_reclen, d_type, d_name} aligned to 8; legacy linux_dirent
 * {d_ino, d_off, d_reclen, d_name, NUL, ..., d_type in the last byte} aligned to 8 (fs/readdir.c).
 */
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include "fs_priv.h"

/* unique: the caller knows the name is not in the snapshot yet (a host readdir lists each name once). The check
 * is linear, and a host directory can hold tens of thousands of names (Rocket League's CookedPCConsole: 20,746). */
static int snap_add(struct fs_file *f, size_t *cap, const char *name, uint64_t ino, uint8_t type, int unique)
{
    if (!unique)
        for (size_t i = 0; i < f->nents; i++)
            if (strcmp(f->ents[i].name, name) == 0) return 0;
    if (f->nents == *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        struct fs_dirent *ne = realloc(f->ents, nc * sizeof *ne);
        if (!ne) return -LX_ENOMEM;
        f->ents = ne;
        *cap = nc;
    }
    char *n = strdup(name);
    if (!n) return -LX_ENOMEM;
    f->ents[f->nents].name = n;
    f->ents[f->nents].ino = ino;
    f->ents[f->nents].type = type;
    f->nents++;
    return 0;
}

static uint64_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h | 1;
}

static int snapshot(lkd_task *t, lkd_fdobj *o, struct fs_file *f)
{
    size_t cap = 0;
    int rc = 0;
    const char *gp = o->gpath ? o->gpath : "/";
    if (o->host_fd >= 0) {
        /* the descriptor is open for search only (fs_open_host): read the same directory through "." */
        int d = openat(o->host_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d < 0) return LKD_HOST_ERR();
        DIR *dir = fdopendir(d);
        if (!dir) { int e = (int)LKD_HOST_ERR(); close(d); return e; }
        rewinddir(dir);
        struct dirent *de;
        errno = 0;
        while ((de = readdir(dir)) != NULL) {
            rc = snap_add(f, &cap, de->d_name, de->d_ino, lkd_dtype_from_host(de->d_type), 1);
            if (rc < 0) break;
        }
        closedir(dir);
    } else {
        rc = snap_add(f, &cap, ".", o->ino ? o->ino : name_hash(gp), LX_DT_DIR, 1);
        if (rc == 0) rc = snap_add(f, &cap, "..", name_hash(".."), LX_DT_DIR, 0);
        const struct fs_mount *m = fs_mount_find(gp);
        if (rc == 0 && m && m->kind == FS_MNT_VIRT && m->ops->readdir) {
            const char *rel = m->glen == 1 ? gp : gp + m->glen;
            while (*rel == '/') rel++;
            char name[256];
            for (uint64_t i = 0; rc == 0; i++) {
                uint8_t ty = LX_DT_UNKNOWN;
                uint64_t ino = 0;
                name[0] = 0;
                int r = m->ops->readdir(t, m->priv, rel, i, name, &ty, &ino);
                if (r < 0) { rc = r; break; }
                if (r == 0) break;
                name[255] = 0;
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
                rc = snap_add(f, &cap, name, ino ? ino : name_hash(name), ty, 0);
            }
        }
    }
    /* Mount points directly below this directory. */
    size_t gl = strlen(gp);
    int n = fs_mount_count();
    for (int i = 0; rc == 0 && i < n; i++) {
        const struct fs_mount *m = fs_mount_at(i);
        const char *mp = m->gpath;
        const char *rest;
        if (gl == 1) rest = mp + 1;
        else if (strncmp(mp, gp, gl) == 0 && mp[gl] == '/') rest = mp + gl + 1;
        else continue;
        if (!*rest) continue;
        char comp[256];
        const char *sl = strchr(rest, '/');
        size_t cl = sl ? (size_t)(sl - rest) : strlen(rest);
        if (cl == 0 || cl >= sizeof comp) continue;
        memcpy(comp, rest, cl);
        comp[cl] = 0;
        rc = snap_add(f, &cap, comp, name_hash(mp), LX_DT_DIR, 0);
    }
    if (rc == 0) f->snap_valid = 1;
    return rc;
}

int64_t fs_getdents(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, int legacy)
{
    struct fs_file *f = o->priv;
    if (!f) return -LX_ENOTDIR;
    if (count > (uint64_t)FS_MAX_RW) count = (uint64_t)FS_MAX_RW;
    pthread_mutex_lock(&o->lock);
    if (!f->snap_valid) {
        int rc = snapshot(t, o, f);
        if (rc < 0) { pthread_mutex_unlock(&o->lock); return rc; }
    }
    size_t bcap = count < 65536 ? (size_t)count : 65536;
    char *buf = malloc(bcap ? bcap : 1);
    if (!buf) { pthread_mutex_unlock(&o->lock); return -LX_ENOMEM; }
    size_t used = 0;
    uint64_t pos = f->dpos;
    while (pos < f->nents) {
        const struct fs_dirent *e = &f->ents[pos];
        size_t nl = strlen(e->name);
        size_t reclen = legacy ? ((18 + nl + 2 + 7) & ~(size_t)7) : ((19 + nl + 1 + 7) & ~(size_t)7);
        if (used + reclen > bcap) break;
        char *p = buf + used;
        memset(p, 0, reclen);
        uint64_t ino = e->ino;
        int64_t off = (int64_t)(pos + 1);
        uint16_t rl = (uint16_t)reclen;
        memcpy(p, &ino, 8);
        memcpy(p + 8, &off, 8);
        memcpy(p + 16, &rl, 2);
        if (legacy) {
            memcpy(p + 18, e->name, nl);
            p[reclen - 1] = (char)e->type;
        } else {
            p[18] = (char)e->type;
            memcpy(p + 19, e->name, nl);
        }
        used += reclen;
        pos++;
    }
    int64_t ret;
    if (used == 0 && pos < f->nents) ret = -LX_EINVAL;   /* buffer too small for one entry */
    else if (used && lkd_user_write(gva, buf, used) < 0) ret = -LX_EFAULT;
    else { f->dpos = pos; ret = (int64_t)used; }
    pthread_mutex_unlock(&o->lock);
    free(buf);
    return ret;
}
