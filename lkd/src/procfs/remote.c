/* remote.c: the default backend of procfs and the family transport.
 *
 * A /proc/<pid> file of a process that lives in ANOTHER host process of the family is rendered by that
 * process (famsvc opcode LKD_FAMSVC_OP_TASK_STATUS) from its own local state; the viewer only sends the
 * access verdict. The handler runs on the family service thread: it has no guest task (lkd_current() is
 * NULL), takes no lock a guest thread holds across a wait, and never calls lkd_famsvc_call.
 */
#include <sys/stat.h>
#include <unistd.h>

#include "famsvc.h"
#include "procfs_priv.h"

static int default_remote(int32_t host_pid, const struct lkd_procfs_wire *rq, char **text, size_t *len)
{
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = LKD_FAMSVC_OP_TASK_STATUS;
    req.data = (void *)rq;
    req.len = sizeof *rq;
    req.target_pid = rq->pid;
    req.target_tid = rq->tid;
    int r = lkd_famsvc_call(host_pid, &req, &resp, 0);
    if (r < 0) {
        lkd_famsvc_msg_free(&resp);
        /* the process vanished between the registry lookup and the call: the file has no task any more */
        return r == -LX_ETIMEDOUT ? -LX_EIO : r;
    }
    *text = resp.data;
    *len = resp.len;
    resp.data = NULL;
    resp.len = 0;
    if (!*text) { *text = malloc(1); if (!*text) { lkd_famsvc_msg_free(&resp); return -LX_ENOMEM; } (*text)[0] = 0; }
    lkd_famsvc_msg_free(&resp);
    return 0;
}

static const struct lkd_procfs_backend g_default = {
    .proc_lookup = lkd_proc_lookup, .tid_lookup = lkd_tid_lookup, .proc_list = lkd_proc_list, .tid_list = lkd_tid_list,
    .access = lkd_ptrace_check_access, .remote = default_remote,
};
struct lkd_procfs_backend pf_be = {
    .proc_lookup = lkd_proc_lookup, .tid_lookup = lkd_tid_lookup, .proc_list = lkd_proc_list, .tid_list = lkd_tid_list,
    .access = lkd_ptrace_check_access, .remote = default_remote,
};

void lkd_procfs_set_backend(const struct lkd_procfs_backend *b)
{
    pf_be = g_default;
    if (!b) return;
    if (b->proc_lookup) pf_be.proc_lookup = b->proc_lookup;
    if (b->tid_lookup) pf_be.tid_lookup = b->tid_lookup;
    if (b->proc_list) pf_be.proc_list = b->proc_list;
    if (b->tid_list) pf_be.tid_list = b->tid_list;
    if (b->access) pf_be.access = b->access;
    if (b->remote) pf_be.remote = b->remote;
}

static void op_task_status(lkd_famsvc_msg *req, lkd_famsvc_msg *resp)
{
    if (req->len != sizeof(struct lkd_procfs_wire) || !req->data) { resp->status = -LX_EINVAL; return; }
    struct lkd_procfs_wire w;
    memcpy(&w, req->data, sizeof w);
    if (w.kind >= PK_COUNT) { resp->status = -LX_EINVAL; return; }
    lkd_task *tk = lkd_task_lookup(w.tid ? w.tid : w.pid);
    if (!tk || tk->proc->pid != w.pid) { if (tk) lkd_task_put(tk); resp->status = -LX_ESRCH; return; }
    if (w.open_fd) {
        lkd_fdobj *o = tk->fdt ? lkd_fd_get(tk, w.fd, NULL) : NULL, *no = NULL;
        if (!o) { lkd_task_put(tk); resp->status = -LX_ENOENT; return; }
        int rc = lkd_fdobj_reopen(tk, o, w.lx_flags, &no);
        lkd_fdobj_put(o);
        if (rc < 0) { lkd_task_put(tk); resp->status = rc; return; }
        char path[4096];
        int n = lkd_fs_describe(no, path, sizeof path - 1);
        int pin = lkd_fs_pin_raw(no);   /* the reader gets the raw host fd: guest offsets = host offsets */
        if (pin < 0) { lkd_fdobj_put(no); lkd_task_put(tk); resp->status = pin; return; }
        int fd = no->host_fd >= 0 ? dup(no->host_fd) : -1;
        uint32_t fl = atomic_load(&no->status_flags);
        lkd_fdobj_put(no);
        lkd_task_put(tk);
        if (fd < 0) { resp->status = no ? -LX_ENXIO : -LX_EBADF; return; }
        resp->data = malloc(sizeof fl + (size_t)(n > 0 ? n : 0) + 1);
        if (!resp->data) { close(fd); resp->status = -LX_ENOMEM; return; }
        memcpy(resp->data, &fl, sizeof fl);
        if (n > 0) memcpy((char *)resp->data + sizeof fl, path, (size_t)n);
        resp->len = sizeof fl + (size_t)(n > 0 ? n : 0);
        resp->fds[0] = fd;
        resp->nfds = 1;
        resp->status = 0;
        return;
    }
    struct pbuf b = { 0 };
    int rc = pf_render_local(&w, tk, &b);
    lkd_task_put(tk);
    if (rc < 0) { pb_free(&b); resp->status = rc; return; }
    resp->data = b.p;
    resp->len = b.n;
    resp->status = 0;
}

int pf_remote_open_fd(int32_t host_pid, const struct pf_node *n, uint32_t fl, lkd_fdobj **out)
{
    struct lkd_procfs_wire w;
    memset(&w, 0, sizeof w);
    w.kind = PK_FD; w.pid = n->pid; w.tid = n->tid ? n->tid : n->pid; w.fd = n->fd; w.open_fd = 1; w.lx_flags = fl;
    lkd_famsvc_msg req, resp;
    memset(&req, 0, sizeof req);
    memset(&resp, 0, sizeof resp);
    req.op = LKD_FAMSVC_OP_TASK_STATUS;
    req.data = &w;
    req.len = sizeof w;
    req.target_pid = w.pid;
    int r = lkd_famsvc_call(host_pid, &req, &resp, 0);
    if (r < 0 || resp.nfds < 1 || resp.len < sizeof(uint32_t)) {
        lkd_famsvc_msg_free(&resp);
        return r < 0 ? (r == -LX_ESRCH ? -LX_ENOENT : r) : -LX_EPROTO;
    }
    int fd = resp.fds[0];
    resp.fds[0] = -1;
    uint32_t sf;
    memcpy(&sf, resp.data, sizeof sf);
    char *path = strndup((char *)resp.data + sizeof sf, resp.len - sizeof sf);
    lkd_famsvc_msg_free(&resp);
    int rc = pf_dupfile_new(fd, sf, path, out);
    free(path);
    if (rc < 0) close(fd);
    return rc;
}

void pf_remote_init(void)
{
    int rc = lkd_famsvc_register(LKD_FAMSVC_OP_TASK_STATUS, op_task_status);
    if (rc < 0 && rc != -LX_EEXIST) lkd_log("procfs: cannot register the famsvc handler (%d)", rc);
}

/* ---- Object on a descriptor received from another process ------------------------------------------- */

/* The received descriptor shares its host open file description with the other process: the object keeps
 * its own offset and always reads and writes at an explicit offset (pread/pwrite), so the other process'
 * position is never moved. Regular files and memfds only (the sender refuses pipes and sockets). */
struct dup_priv { int64_t pos; };

static int64_t dup_rw(lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset, int w)
{
    struct dup_priv *p = o->priv;
    pthread_mutex_lock(&o->lock);
    int64_t pos = offset ? *offset : p->pos;
    pthread_mutex_unlock(&o->lock);
    int64_t r = fs_host_io(o->host_fd, gva, count, &pos, w);
    if (r > 0 && !offset) { pthread_mutex_lock(&o->lock); p->pos = pos + r; pthread_mutex_unlock(&o->lock); }
    return r;
}
static int64_t dup_read(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{ (void)t; return dup_rw(o, gva, count, offset, 0); }
static int64_t dup_write(lkd_task *t, lkd_fdobj *o, uint64_t gva, uint64_t count, const int64_t *offset)
{ (void)t; return dup_rw(o, gva, count, offset, 1); }
static int64_t dup_lseek(lkd_task *t, lkd_fdobj *o, int64_t off, int whence)
{
    (void)t;
    struct dup_priv *p = o->priv;
    int64_t base = 0;
    if (whence == LX_SEEK_CUR) base = p->pos;
    else if (whence == LX_SEEK_END) { struct stat hs; if (fstat(o->host_fd, &hs) < 0) return LKD_HOST_ERR(); base = hs.st_size; }
    else if (whence != LX_SEEK_SET) return -LX_EINVAL;
    if (base + off < 0) return -LX_EINVAL;
    p->pos = base + off;
    return p->pos;
}
static int dup_fstat(lkd_task *t, lkd_fdobj *o, struct lx_stat *out)
{
    (void)t;
    struct stat hs;
    if (fstat(o->host_fd, &hs) < 0) return (int)LKD_HOST_ERR();
    fs_stat_from_host(&hs, out);
    return 0;
}
static int dup_ftruncate(lkd_task *t, lkd_fdobj *o, int64_t len)
{ (void)t; return ftruncate(o->host_fd, len) < 0 ? (int)LKD_HOST_ERR() : 0; }
static int dup_mmap_fd(lkd_fdobj *o) { return o->host_fd; }
static void dup_release(lkd_fdobj *o) { free(o->priv); o->priv = NULL; }

static const lkd_fd_ops g_dup_ops = {
    .name = "procfile", .read = dup_read, .write = dup_write, .lseek = dup_lseek, .fstat = dup_fstat,
    .ftruncate = dup_ftruncate, .mmap_fd = dup_mmap_fd, .release = dup_release,
};

int pf_dupfile_new(int host_fd, uint32_t status_flags, const char *gpath, lkd_fdobj **out)
{
    struct dup_priv *p = calloc(1, sizeof *p);
    if (!p) return -LX_ENOMEM;
    lkd_fdobj *o = lkd_fdobj_new(LKD_FD_FILE, &g_dup_ops, host_fd, status_flags, p);
    if (!o) { free(p); return -LX_ENOMEM; }
    if (gpath) o->gpath = strdup(gpath);
    struct stat hs;
    if (fstat(host_fd, &hs) == 0) o->ino = hs.st_ino;
    *out = o;
    return 0;
}
