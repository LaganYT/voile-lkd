/* exec.c: execve, execveat, lkd_exec_prepare, the task exec_commit hook and lkd_exec_finish.
 *
 * Models fs/exec.c (do_execveat_common, bprm_execve, begin_new_exec, de_thread), fs/binfmt_script.c
 * and the binfmt_elf header checks of Linux v6.12. Loading the ELF (segments, stack, auxv, the program
 * interpreter PT_INTERP) is the engine's job (ops->exec_image); LKD hands it a checked image.
 *
 * Sequence in the syscall:
 *   1. read path, argv, envp from guest memory (E2BIG, ENAMETOOLONG, EFAULT)
 *   2. lkd_exec_prepare: resolve through the vfs, permissions, #! (5 levels), ELF class/machine check;
 *      nothing changes in the process before this point
 *   3. de_thread: other threads are killed; a non-leader caller takes the leader's tid
 *   4. a cohosted process (vfork/posix_spawn child sharing the parent's memory) moves to a host fork()
 *      child here; the old host thread ends silently and releases the waiting parent
 *   5. lkd_exec_commit (core): module exec hooks, task first: robust list, clear_child_tid, comm, exe,
 *      cmdline, dumpable, did-exec flag
 *   6. ops->exec_image; a failure after the point of no return kills the process with SIGSEGV
 *      (force_fatal_sig in bprm_execve)
 *   7. lkd_exec_finish: PTRACE_EVENT_EXEC or the legacy SIGTRAP
 */
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fd.h"
#include "lkd_fs.h"
#include "mm.h"
#include "ptrace.h"
#include "sig.h"
#include "task_priv.h"
#include "vfs.h"

pid_t clone_migrate_fork(lkd_task *t);

#define BINPRM_BUF_SIZE 256
#define MAX_ARG_STRLEN (32u * 4096u)
#define PATH_MAX_LX 4096

/* State handed from the syscall to the exec_commit hook (same thread). */
static _Thread_local struct {
    const lkd_exec_image *img;
    int readable;
    const char *filename;
} g_pending;

static _Thread_local int32_t g_exec_old_tid;

/* ---- Pure helpers ---------------------------------------------------------------------------------- */

int exec_classify(const unsigned char *b, size_t n)
{
    if (n >= 2 && b[0] == '#' && b[1] == '!') return EXEC_KIND_SCRIPT;
    if (n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        if (n < 20) return -LX_ENOEXEC;
        if (b[4] != 2 || b[5] != 1) return -LX_ENOEXEC;          /* ELFCLASS64, ELFDATA2LSB */
        unsigned type = b[16] | (b[17] << 8), mach = b[18] | (b[19] << 8);
        if (type != 2 && type != 3) return -LX_ENOEXEC;          /* ET_EXEC, ET_DYN */
        if (mach != 62) return -LX_ENOEXEC;                      /* EM_X86_64; no IA32 emulation */
        return EXEC_KIND_ELF64;
    }
    return -LX_ENOEXEC;
}

static int spacetab(char c) { return c == ' ' || c == '\t'; }
static const char *next_non_spacetab(const char *s, const char *e) { for (; s < e; s++) if (!spacetab(*s)) return s; return NULL; }
static const char *next_terminator(const char *s, const char *e) { for (; s < e; s++) if (spacetab(*s) || !*s) return s; return NULL; }

int exec_parse_shebang(const unsigned char *ubuf, size_t n, char *interp, size_t icap, char *arg, size_t acap)
{
    char buf[BINPRM_BUF_SIZE];
    memset(buf, 0, sizeof buf);
    memcpy(buf, ubuf, n < sizeof buf ? n : sizeof buf);
    if (buf[0] != '#' || buf[1] != '!') return -LX_ENOEXEC;
    /* fs/binfmt_script.c load_script() */
    const char *buf_end = buf + sizeof buf - 1;
    const char *i_end = memchr(buf, '\n', sizeof buf);
    if (!i_end) {
        i_end = next_non_spacetab(buf + 2, buf_end);
        if (!i_end) return -LX_ENOEXEC;              /* entire buffer is spaces/tabs */
        if (!next_terminator(i_end, buf_end)) return -LX_ENOEXEC; /* interpreter path truncated */
        i_end = buf_end;
    }
    while (i_end > buf + 2 && spacetab(i_end[-1])) i_end--;
    const char *i_name = next_non_spacetab(buf + 2, i_end);
    if (!i_name || i_name == i_end) return -LX_ENOEXEC; /* no interpreter name */
    const char *i_arg = NULL, *i_sep = next_terminator(i_name, i_end);
    if (i_sep && *i_sep) i_arg = next_non_spacetab(i_sep, i_end);
    size_t nlen = (size_t)((i_sep ? i_sep : i_end) - i_name);
    if (nlen + 1 > icap) return -LX_ENAMETOOLONG;
    memcpy(interp, i_name, nlen);
    interp[nlen] = 0;
    /* a NUL inside the name ends it (strlen semantics of the kernel string) */
    arg[0] = 0;
    if (i_arg) {
        size_t alen = (size_t)(i_end - i_arg);
        if (alen + 1 > acap) return -LX_ENAMETOOLONG;
        memcpy(arg, i_arg, alen);
        arg[alen] = 0;
    }
    return 0;
}

/* ---- String vectors ----------------------------------------------------------------------------------- */

static void free_vec(char **v)
{
    if (!v) return;
    for (char **p = v; *p; p++) free(*p);
    free(v);
}

static char **dup_vec(char *const *src, int *count, size_t *bytes)
{
    int n = 0;
    while (src && src[n]) n++;
    char **v = calloc((size_t)n + 1, sizeof *v);
    if (!v) return NULL;
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        v[i] = strdup(src[i]);
        if (!v[i]) { free_vec(v); return NULL; }
        total += strlen(src[i]) + 1;
    }
    if (count) *count = n;
    if (bytes) *bytes = total;
    return v;
}

/* Insert `items` (k of them) in front of v[skip..]; returns the new vector, frees the old one. */
static char **rebuild_argv(char **v, int skip, const char *const *items, int k)
{
    int n = 0;
    while (v[n]) n++;
    if (skip > n) skip = n;
    char **out = calloc((size_t)(n - skip + k) + 1, sizeof *out);
    if (!out) return NULL;
    int j = 0;
    for (int i = 0; i < k; i++) {
        out[j] = strdup(items[i]);
        if (!out[j]) { free_vec(out); return NULL; }
        j++;
    }
    for (int i = skip; i < n; i++) { out[j++] = v[i]; v[i] = NULL; }
    free_vec(v);
    return out;
}

static size_t vec_bytes(char **v, int *count)
{
    size_t b = 0;
    int n = 0;
    for (; v && v[n]; n++) b += strlen(v[n]) + 1;
    if (count) *count = n;
    return b;
}

/* ---- Opening the file ----------------------------------------------------------------------------------- */

static int may_exec(lkd_task *t, const struct lx_stat *st, int *readable)
{
    const struct lkd_creds *c = &t->proc->cred;
    uint32_t mode = st->st_mode, bits;
    if (c->fsuid == st->st_uid) bits = (mode >> 6) & 7;
    else {
        int in_group = c->fsgid == st->st_gid;
        for (uint32_t i = 0; i < c->ngroups && !in_group; i++) in_group = c->groups[i] == st->st_gid;
        bits = in_group ? (mode >> 3) & 7 : mode & 7;
    }
    *readable = (bits & 4) != 0;
    if (!LX_S_ISREG(mode)) return -LX_EACCES;
    if (!(bits & 1)) return -LX_EACCES;
    return 0;
}

struct opened {
    int host_fd;
    char gpath[PATH_MAX_LX];
    char hpath[PATH_MAX_LX];
    int readable;
    uint32_t mode;
};

static int open_exec(lkd_task *t, int dirfd, const char *path, int flags, struct opened *o)
{
    struct lx_stat st;
    memset(o, 0, sizeof *o);
    o->host_fd = -1;
    if ((flags & LX_AT_EMPTY_PATH) && !path[0]) {
        lkd_fdobj *obj = lkd_fd_get(t, dirfd, NULL);
        if (!obj) return -LX_EBADF;
        int e = obj->ops && obj->ops->fstat ? obj->ops->fstat(t, obj, &st) : -LX_EACCES;
        if (e == 0) e = may_exec(t, &st, &o->readable);
        if (e == 0 && obj->host_fd < 0) e = -LX_EACCES; /* nothing the engine could map */
        if (e == 0) e = lkd_fs_pin_raw(obj);              /* the engine loads the ELF by host offsets */
        if (e == 0) {
            o->host_fd = fcntl(obj->host_fd, F_DUPFD_CLOEXEC, 0);
            if (o->host_fd < 0) e = (int)LKD_HOST_ERR();
        }
        if (e == 0) {
            snprintf(o->gpath, sizeof o->gpath, "%s", obj->gpath ? obj->gpath : "");
            char hp[MAXPATHLEN];
            if (fcntl(o->host_fd, F_GETPATH, hp) == 0) snprintf(o->hpath, sizeof o->hpath, "%s", hp);
            o->mode = st.st_mode;
        }
        lkd_fdobj_put(obj);
        if (e < 0 && o->host_fd >= 0) { close(o->host_fd); o->host_fd = -1; }
        return e;
    }
    if (!path[0]) return -LX_ENOENT;
    int e = lkd_vfs_stat(t, dirfd, path, (flags & LX_AT_SYMLINK_NOFOLLOW) ? LX_AT_SYMLINK_NOFOLLOW : 0, &st);
    if (e < 0) return e;
    if ((flags & LX_AT_SYMLINK_NOFOLLOW) && LX_S_ISLNK(st.st_mode)) return -LX_ELOOP;
    e = may_exec(t, &st, &o->readable);
    if (e < 0) return e;
    o->mode = st.st_mode;
    if (lkd_vfs_realpath(t, dirfd, path, o->gpath, sizeof o->gpath) < 0) snprintf(o->gpath, sizeof o->gpath, "%s", path);
    if (lkd_vfs_to_host(t, dirfd, path, 0, o->hpath, sizeof o->hpath) == 0) {
        o->host_fd = open(o->hpath, O_RDONLY | O_CLOEXEC);
        if (o->host_fd < 0) return (int)LKD_HOST_ERR();
        return 0;
    }
    /* virtual mount (e.g. /proc/self/exe): ask the vfs for the object behind it */
    lkd_fdobj *obj = NULL;
    e = lkd_vfs_open(t, dirfd, path, LX_O_RDONLY | LX_O_CLOEXEC, 0, &obj);
    if (e < 0) return e;
    if (obj->host_fd < 0) { lkd_fdobj_put(obj); return -LX_EACCES; }
    o->host_fd = fcntl(obj->host_fd, F_DUPFD_CLOEXEC, 0);
    lkd_fdobj_put(obj);
    if (o->host_fd < 0) return (int)LKD_HOST_ERR();
    char hp[MAXPATHLEN];
    if (fcntl(o->host_fd, F_GETPATH, hp) == 0) snprintf(o->hpath, sizeof o->hpath, "%s", hp);
    return 0;
}

static int exec_prepare_at(lkd_task *t, int dirfd, const char *path, int flags, char *const argv[], char *const envp[],
                           lkd_exec_image *img, int *readable_out)
{
    memset(img, 0, sizeof *img);
    img->host_fd = -1;
    if (flags & ~(LX_AT_EMPTY_PATH | LX_AT_SYMLINK_NOFOLLOW)) return -LX_EINVAL;
    /* filename as Linux builds it (alloc_bprm): the path, or /dev/fd/N[/path] for fd-relative execs */
    char filename[PATH_MAX_LX + 32];
    int fd_path = 0;
    if (path[0] == '/' || dirfd == LX_AT_FDCWD) snprintf(filename, sizeof filename, "%s", path);
    else if (!path[0] && (flags & LX_AT_EMPTY_PATH)) { snprintf(filename, sizeof filename, "/dev/fd/%d", dirfd); fd_path = 1; }
    else { snprintf(filename, sizeof filename, "/dev/fd/%d/%s", dirfd, path); fd_path = 1; }
    int path_inaccessible = 0;
    if (fd_path) {
        int cx = 0;
        lkd_fdobj *o = lkd_fd_get(t, dirfd, &cx);
        if (o) { path_inaccessible = cx; lkd_fdobj_put(o); }
    }

    int argc = 0, envc = 0;
    size_t ab = 0, eb = 0;
    char **av = dup_vec(argv, &argc, &ab), **ev = dup_vec(envp, &envc, &eb);
    if (!av || !ev) { free_vec(av); free_vec(ev); return -LX_ENOMEM; }
    if (argc == 0) {
        /* do_execveat_common(): an empty argv gets "" as argv[0] (since 5.18) */
        const char *empty[1] = { "" };
        av = rebuild_argv(av, 0, empty, 1);
        if (!av) { free_vec(ev); return -LX_ENOMEM; }
    }

    struct opened o;
    int e = open_exec(t, dirfd, path, flags, &o);
    int depth = 0, readable = 0;
    char cur_interp[PATH_MAX_LX + 32];
    snprintf(cur_interp, sizeof cur_interp, "%s", filename);
    while (e == 0) {
        unsigned char hdr[BINPRM_BUF_SIZE];
        memset(hdr, 0, sizeof hdr);
        ssize_t n = pread(o.host_fd, hdr, sizeof hdr, 0);
        if (n < 0) { e = (int)LKD_HOST_ERR(); break; }
        if (depth == 0) readable = o.readable;
        else readable = readable && o.readable;
        if (o.mode & (LX_S_ISUID | LX_S_ISGID)) img->set_uid_ignored = 1;
        int kind = exec_classify(hdr, (size_t)n);
        if (kind < 0) { e = kind; break; }
        if (kind == EXEC_KIND_ELF64) break;
        /* script */
        if (path_inaccessible && depth == 0) { e = -LX_ENOENT; break; } /* BINPRM_FLAGS_PATH_INACCESSIBLE */
        if (++depth > 5) { e = -LX_ELOOP; break; }
        char interp[PATH_MAX_LX], arg[BINPRM_BUF_SIZE];
        e = exec_parse_shebang(hdr, (size_t)n, interp, sizeof interp, arg, sizeof arg);
        if (e < 0) break;
        const char *items[3];
        int k = 0;
        items[k++] = interp;
        if (arg[0]) items[k++] = arg;
        items[k++] = cur_interp;
        char **nav = rebuild_argv(av, 1, items, k); /* argv[0] is replaced */
        if (!nav) { av = NULL; e = -LX_ENOMEM; break; }
        av = nav;
        snprintf(cur_interp, sizeof cur_interp, "%s", interp);
        close(o.host_fd);
        e = open_exec(t, LX_AT_FDCWD, interp, 0, &o);
    }
    if (e == 0) {
        /* prepare_arg_pages(): strings and pointers within min(RLIMIT_STACK/4, 6 MiB), capped by LKD */
        uint64_t stack = t->proc->rlimits[LX_RLIMIT_STACK].rlim_cur;
        uint64_t limit = stack / 4 < (6ull << 20) ? stack / 4 : (6ull << 20);
        if (limit > LKD_EXEC_MAX_ARGS_BYTES) limit = LKD_EXEC_MAX_ARGS_BYTES;
        int na = 0;
        size_t bytes = vec_bytes(av, &na) + vec_bytes(ev, NULL);
        uint64_t ptrs = ((uint64_t)na + (uint64_t)envc) * 8;
        if (limit <= ptrs || bytes > limit - ptrs) e = -LX_E2BIG;
        img->argc = na;
    }
    if (e < 0) {
        if (o.host_fd >= 0) close(o.host_fd);
        free_vec(av);
        free_vec(ev);
        memset(img, 0, sizeof *img);
        img->host_fd = -1;
        return e;
    }
    img->guest_path = strdup(o.gpath);
    img->host_path = strdup(o.hpath);
    img->host_fd = o.host_fd;
    img->argv = av;
    img->envp = ev;
    img->interp_depth = depth;
    img->original_path = strdup(filename);
    if (!img->guest_path || !img->host_path || !img->original_path) { lkd_exec_image_free(img); return -LX_ENOMEM; }
    if (readable_out) *readable_out = readable;
    return 0;
}

int lkd_exec_prepare(lkd_task *t, const char *path, char *const argv[], char *const envp[], lkd_exec_image *img)
{
    return exec_prepare_at(t, LX_AT_FDCWD, path, 0, argv, envp, img, NULL);
}

/* ---- Commit hook and finish ----------------------------------------------------------------------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

void lkd_task_exec_commit(lkd_task *t)
{
    struct lkd_proc *p = t->proc;
    atomic_fetch_or(&t->flags, LKD_TF_EXEC);
    if (p->nthreads > 1) task_de_thread(t, &g_exec_old_tid);
    task_mm_release(t); /* exec_mm_release(): robust list and clear_child_tid of the old image */
    t->restart.fn = NULL;
    t->sud_on = 0; /* syscall user dispatch does not survive exec */
    t->sud_offset = t->sud_len = t->sud_selector = 0;
    const lkd_exec_image *img = g_pending.img;
    if (!img) return; /* commit called outside execve: nothing to rename */
    char comm[LKD_COMM_LEN];
    memset(comm, 0, sizeof comm);
    strncpy(comm, base_name(g_pending.filename ? g_pending.filename : img->original_path), LKD_COMM_LEN - 1);
    memcpy(t->comm, comm, sizeof comm); /* __set_task_comm(me, kbasename(bprm->filename)) */
    pthread_mutex_lock(&p->lock);
    free(p->exe_path);
    p->exe_path = strdup(img->guest_path);
    free(p->cmdline);
    p->cmdline = NULL;
    p->cmdline_len = 0;
    size_t n = vec_bytes(img->argv, NULL);
    if (n && (p->cmdline = malloc(n))) {
        char *q = p->cmdline;
        for (char **a = img->argv; *a; a++) { size_t l = strlen(*a) + 1; memcpy(q, *a, l); q += l; }
        p->cmdline_len = n;
    }
    free(p->environ_snap);
    p->environ_snap = NULL;
    p->environ_len = 0;
    n = vec_bytes(img->envp, NULL);
    if (n && (p->environ_snap = malloc(n))) {
        char *q = p->environ_snap;
        for (char **a = img->envp; *a; a++) { size_t l = strlen(*a) + 1; memcpy(q, *a, l); q += l; }
        p->environ_len = n;
    }
    /* begin_new_exec(): dumpable is SUID_DUMP_USER unless the file was not readable (would_dump) */
    p->dumpable = g_pending.readable ? 1 : 0;
    if (p->layout) { free(p->layout->auxv); free(p->layout); p->layout = NULL; }
    pthread_mutex_unlock(&p->lock);
    fam_lock();
    struct fam_proc *r = p->wait->rec;
    if (r && r->info.pid == p->pid) atomic_fetch_or(&r->flags, RF_DID_EXEC);
    fam_unlock();
    lkd_registry_publish(t);
}

void lkd_exec_finish(lkd_task *t)
{
    atomic_fetch_and(&t->flags, ~LKD_TF_EXEC);
    int32_t old = g_exec_old_tid ? g_exec_old_tid : t->tid;
    g_exec_old_tid = 0;
    /* fs/exec.c exec_binprm(): ptrace_event(PTRACE_EVENT_EXEC, old_vpid); a plain PTRACE_TRACEME
     * tracee without the option gets SIGTRAP instead (send_sig(SIGTRAP, current, 0)). */
    if (lkd_ptrace_event(t, LX_PTRACE_EVENT_EXEC, (uint64_t)old)) return;
    uint32_t pf = atomic_load(&t->ptrace.flags);
    if ((pf & LKD_PT_TRACED) && !(pf & LKD_PT_SEIZED)) {
        struct lx_siginfo si;
        memset(&si, 0, sizeof si);
        si.si_signo = LX_SIGTRAP;
        si.si_code = LX_SI_USER;
        si.u.kill.pid = t->proc->pid;
        si.u.kill.uid = t->proc->cred.uid;
        lkd_sig_send_task(t, LX_SIGTRAP, &si, 0);
    }
}

/* ---- Syscalls ------------------------------------------------------------------------------------------- */

static int read_vec(uint64_t arr, char ***out)
{
    *out = NULL;
    size_t cap = 16, n = 0, total = 0;
    char **v = calloc(cap + 1, sizeof *v);
    if (!v) return -LX_ENOMEM;
    char *buf = malloc(MAX_ARG_STRLEN);
    if (!buf) { free(v); return -LX_ENOMEM; }
    int e = 0;
    for (uint64_t i = 0; arr; i++) {
        uint64_t p;
        if (lkd_user_read(arr + i * 8, &p, 8)) { e = -LX_EFAULT; break; }
        if (!p) break;
        int64_t len = lkd_user_strn(p, buf, MAX_ARG_STRLEN);
        if (len == -LX_ENAMETOOLONG) { e = -LX_E2BIG; break; }  /* valid_arg_len() */
        if (len < 0) { e = (int)len; break; }
        total += (size_t)len + 1;
        if (total > LKD_EXEC_MAX_ARGS_BYTES || n >= 0x7fffffff) { e = -LX_E2BIG; break; }
        if (n == cap) {
            char **nv = realloc(v, (cap * 2 + 1) * sizeof *v);
            if (!nv) { e = -LX_ENOMEM; break; }
            v = nv;
            cap *= 2;
        }
        v[n] = strdup(buf);
        if (!v[n]) { e = -LX_ENOMEM; break; }
        v[++n] = NULL;
    }
    free(buf);
    if (e < 0) { free_vec(v); return e; }
    *out = v;
    return 0;
}

static int64_t do_execve(lkd_task *t, int dirfd, uint64_t path_gva, uint64_t argv_gva, uint64_t envp_gva, int flags)
{
    char *path = malloc(PATH_MAX_LX);
    if (!path) return -LX_ENOMEM;
    int64_t pl = lkd_user_strn(path_gva, path, PATH_MAX_LX);
    if (pl < 0) { free(path); return pl; }
    char **argv = NULL, **envp = NULL;
    int64_t e = read_vec(argv_gva, &argv);
    if (!e) e = read_vec(envp_gva, &envp);
    lkd_exec_image img;
    int readable = 0;
    if (!e) e = exec_prepare_at(t, dirfd, path, flags, argv, envp, &img, &readable);
    free_vec(argv);
    free_vec(envp);
    if (e < 0) { free(path); return e; }

    /* de_thread before anything else: the other threads must not survive into the new image */
    int32_t old_tid = t->tid;
    e = task_de_thread(t, &old_tid);
    if (e < 0) { lkd_exec_image_free(&img); free(path); return e; }
    g_exec_old_tid = old_tid;

    if (t->proc->wait->cohosted) {
        pid_t hp = clone_migrate_fork(t);
        if (hp < 0) { lkd_exec_image_free(&img); free(path); return hp; }
        if (hp > 0) {
            /* old host process: the guest process continues in host process hp */
            wait_note_host_child(hp, t->proc->pid);
            task_mm_release(t); /* exec_mm_release() in the old (shared) memory */
            task_set_xflags(t, LT_MIGRATED, 0);
            fam_lock();
            struct fam_proc *r = fam_proc_find(t->proc->pid);
            if (r) { r->info.host_pid = hp; atomic_fetch_or(&r->flags, RF_VFORK_DONE); atomic_fetch_and(&r->flags, ~RF_COHOSTED); }
            int32_t ppid = r ? r->info.ppid : 0;
            fam_unlock();
            if (ppid) lkd_task_wake_waiters(ppid);
            lkd_exec_image_free(&img);
            free(path);
            return LKD_RET_EXIT_THREAD;
        }
    }

    g_pending.img = &img;
    g_pending.readable = readable;
    g_pending.filename = img.original_path;
    lkd_exec_commit(t, &img);
    g_pending.img = NULL;
    g_pending.filename = NULL;

    /* exec_mm_release() -> complete_vfork_done(): the vfork parent may run again */
    fam_lock();
    struct fam_proc *r = fam_proc_find(t->proc->pid);
    int32_t ppid = 0;
    if (r) { atomic_fetch_or(&r->flags, RF_VFORK_DONE); ppid = r->info.ppid; }
    fam_unlock();
    if (ppid) lkd_task_wake_waiters(ppid);

    int er = lkd_engine()->exec_image(t, &img);
    lkd_exec_image_free(&img);
    free(path);
    if (er < 0) {
        lkd_log("exec_image failed with %d after the point of no return: killing pid %d with SIGSEGV", er, t->proc->pid);
        return lkd_task_do_exit_group(t, LX_SIGSEGV);
    }
    lkd_exec_finish(t);
    return LKD_RET_CONTEXT_REPLACED;
}

LKD_SYSCALL(execve) { return do_execve(t, LX_AT_FDCWD, LKD_ARGP(0), LKD_ARGP(1), LKD_ARGP(2), 0); }
LKD_SYSCALL(execveat) { return do_execve(t, LKD_ARGI(0), LKD_ARGP(1), LKD_ARGP(2), LKD_ARGP(3), LKD_ARGI(4)); }

void task_exec_register(void)
{
    LKD_REGISTER(execve);
    LKD_REGISTER(execveat);
}
