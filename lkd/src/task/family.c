/* family.c: the family registry shared by every host process of one guest "machine".
 *
 * One unlinked temporary file, mapped MAP_SHARED: a header (pid counter, change counter, UTS names, pid of the
 * family's init), a table of process records and a table of thread records. A host fork() inherits the mapping, a host
 * re-exec re-attaches through the fd exported by lkd_family_export. Writers take a process-local mutex
 * and then an fcntl byte lock on the file (released by the host kernel if the holder dies, rule 3.4 of
 * docs/CONVENTIONS.md). The pid counter is a lock-free shared atomic and never reuses an id during a
 * run (Linux reuses pids after wrap; a run never gets near 2^30 ids).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "task_priv.h"

#define FAM_MAGIC 0x4c4b4446u /* 'LKDF' */
#define FAM_VERSION 2

struct fam_hdr {
    uint32_t magic, version;
    uint64_t size;
    _Atomic int32_t next_pid;
    _Atomic uint32_t seq;
    uint32_t nprocs, ntids;
    char hostname[65];
    char domainname[65];
    int32_t child_reaper;          /* pid of the family's init, 0 until its first process is registered */
};

static struct fam_hdr *g_hdr;
static struct fam_proc *g_procs;
static struct fam_tid *g_tids;
static int g_fd = -1;
static size_t g_size;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_create_mutex = PTHREAD_MUTEX_INITIALIZER;

static size_t layout(size_t *off_procs, size_t *off_tids)
{
    size_t h = (sizeof(struct fam_hdr) + 63) & ~(size_t)63;
    *off_procs = h;
    *off_tids = h + (size_t)FAM_MAX_PROCS * sizeof(struct fam_proc);
    size_t total = *off_tids + (size_t)FAM_MAX_TIDS * sizeof(struct fam_tid);
    return (total + 16383) & ~(size_t)16383;
}

static int map_fd(int fd, size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return (int)LKD_HOST_ERR();
    size_t op, ot;
    layout(&op, &ot);
    g_hdr = p;
    g_procs = (struct fam_proc *)((char *)p + op);
    g_tids = (struct fam_tid *)((char *)p + ot);
    g_size = size;
    g_fd = fd;
    return 0;
}

int lkd_family_create(uint32_t first_pid)
{
    pthread_mutex_lock(&g_create_mutex);
    if (g_hdr) { pthread_mutex_unlock(&g_create_mutex); return 0; }
    const char *dir = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof path, "%s/lkd-family-XXXXXX", dir && *dir ? dir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) { int e = (int)LKD_HOST_ERR(); pthread_mutex_unlock(&g_create_mutex); return e; }
    unlink(path);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t op, ot, size = layout(&op, &ot);
    int e = 0;
    if (ftruncate(fd, (off_t)size) < 0) e = (int)LKD_HOST_ERR();
    if (!e) e = map_fd(fd, size);
    if (e) { close(fd); pthread_mutex_unlock(&g_create_mutex); return e; }
    g_hdr->magic = FAM_MAGIC;
    g_hdr->version = FAM_VERSION;
    g_hdr->size = size;
    g_hdr->nprocs = FAM_MAX_PROCS;
    g_hdr->ntids = FAM_MAX_TIDS;
    atomic_store(&g_hdr->next_pid, (int32_t)(first_pid ? first_pid : 2));
    const lkd_config *c = lkd_config_get();
    snprintf(g_hdr->hostname, sizeof g_hdr->hostname, "%s", c->hostname ? c->hostname : "obelisk");
    snprintf(g_hdr->domainname, sizeof g_hdr->domainname, "(none)"); /* Linux default UTS domainname */
    pthread_mutex_unlock(&g_create_mutex);
    return 0;
}

int fam_ready(void)
{
    if (g_hdr) return 0;
    const char *env = getenv("LKD_FAMILY_FD");
    if (env && *env && lkd_family_attach(-1) == 0) return 0;
    return lkd_family_create(lkd_config_get()->first_pid);
}

int lkd_family_export(void)
{
    if (fam_ready() < 0) return -LX_ENOMEM;
    int fd = dup(g_fd);
    if (fd < 0) return (int)LKD_HOST_ERR();
    char v[16];
    snprintf(v, sizeof v, "%d", fd);
    if (setenv("LKD_FAMILY_FD", v, 1) < 0) { int e = (int)LKD_HOST_ERR(); close(fd); return e; }
    return fd; /* dup() does not copy FD_CLOEXEC: inheritable */
}

int lkd_family_attach(int fd)
{
    pthread_mutex_lock(&g_create_mutex);
    if (g_hdr) { pthread_mutex_unlock(&g_create_mutex); return 0; }
    if (fd < 0) {
        const char *env = getenv("LKD_FAMILY_FD");
        if (!env || !*env) { pthread_mutex_unlock(&g_create_mutex); return -LX_ENOENT; }
        fd = atoi(env);
    }
    struct stat st;
    int e = 0;
    if (fstat(fd, &st) < 0) e = (int)LKD_HOST_ERR();
    size_t op, ot, want = layout(&op, &ot);
    if (!e && (size_t)st.st_size != want) e = -LX_EINVAL;
    if (!e) e = map_fd(fd, want);
    if (!e && (g_hdr->magic != FAM_MAGIC || g_hdr->version != FAM_VERSION)) {
        munmap(g_hdr, g_size);
        g_hdr = NULL;
        e = -LX_EINVAL;
    }
    if (!e) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        unsetenv("LKD_FAMILY_FD");
    }
    pthread_mutex_unlock(&g_create_mutex);
    return e;
}

int32_t lkd_pid_alloc(void)
{
    if (fam_ready() < 0) return -LX_EAGAIN;
    int32_t id = atomic_fetch_add(&g_hdr->next_pid, 1);
    if (id < 1 || id > LKD_PID_MAX) return -LX_EAGAIN;
    return id;
}

uint32_t lkd_pid_peek(void)
{
    if (fam_ready() < 0) return 0;
    return (uint32_t)atomic_load(&g_hdr->next_pid);
}

void fam_lock(void)
{
    pthread_mutex_lock(&g_mutex);
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
    while (fcntl(g_fd, F_SETLKW, &fl) < 0 && errno == EINTR) { }
}

void fam_unlock(void)
{
    struct flock fl = { .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1 };
    fcntl(g_fd, F_SETLK, &fl);
    pthread_mutex_unlock(&g_mutex);
}

uint32_t fam_seq(void) { return g_hdr ? atomic_load(&g_hdr->seq) : 0; }
int32_t fam_child_reaper(void) { return g_hdr ? g_hdr->child_reaper : 0; }
void fam_set_child_reaper(int32_t pid) { if (g_hdr && !g_hdr->child_reaper) g_hdr->child_reaper = pid; }
void fam_bump(void) { if (g_hdr) atomic_fetch_add(&g_hdr->seq, 1); }

struct fam_proc *fam_proc_at(int i) { return (g_procs && i >= 0 && i < FAM_MAX_PROCS) ? &g_procs[i] : NULL; }
struct fam_tid *fam_tid_at(int i) { return (g_tids && i >= 0 && i < FAM_MAX_TIDS) ? &g_tids[i] : NULL; }

struct fam_proc *fam_proc_find(int32_t pid)
{
    if (!g_procs || pid <= 0) return NULL;
    for (int i = 0; i < FAM_MAX_PROCS; i++)
        if (g_procs[i].info.pid == pid) return &g_procs[i];
    return NULL;
}

struct fam_proc *fam_proc_new(int32_t pid)
{
    if (!g_procs || pid <= 0 || fam_proc_find(pid)) return NULL;
    for (int i = 0; i < FAM_MAX_PROCS; i++)
        if (g_procs[i].info.pid == 0) {
            memset(&g_procs[i], 0, sizeof g_procs[i]);
            g_procs[i].info.pid = pid;
            return &g_procs[i];
        }
    return NULL;
}

void fam_proc_free(struct fam_proc *r)
{
    if (!r) return;
    int32_t pid = r->info.pid;
    for (int i = 0; i < FAM_MAX_TIDS; i++)
        if (g_tids[i].info.tid && g_tids[i].info.tgid == pid) memset(&g_tids[i], 0, sizeof g_tids[i]);
    memset(r, 0, sizeof *r);
}

struct fam_tid *fam_tid_find(int32_t tid)
{
    if (!g_tids || tid <= 0) return NULL;
    for (int i = 0; i < FAM_MAX_TIDS; i++)
        if (g_tids[i].info.tid == tid) return &g_tids[i];
    return NULL;
}

struct fam_tid *fam_tid_new(int32_t tid, int32_t tgid)
{
    if (!g_tids || tid <= 0) return NULL;
    struct fam_tid *r = fam_tid_find(tid);
    if (r) return r;
    for (int i = 0; i < FAM_MAX_TIDS; i++)
        if (g_tids[i].info.tid == 0) {
            memset(&g_tids[i], 0, sizeof g_tids[i]);
            g_tids[i].info.tid = tid;
            g_tids[i].info.tgid = tgid;
            g_tids[i].info.syscall_nr = -1;
            return &g_tids[i];
        }
    return NULL;
}

void fam_tid_free(struct fam_tid *r)
{
    if (r) memset(r, 0, sizeof *r);
}

int fam_count_children(int32_t ppid)
{
    int n = 0;
    for (int i = 0; g_procs && i < FAM_MAX_PROCS; i++)
        if (g_procs[i].info.pid && g_procs[i].info.ppid == ppid && !(atomic_load(&g_procs[i].flags) & RF_ZOMBIE)) n++;
    return n;
}

int fam_count_live(void)
{
    int n = 0;
    for (int i = 0; g_procs && i < FAM_MAX_PROCS; i++)
        if (g_procs[i].info.pid && !(atomic_load(&g_procs[i].flags) & RF_ZOMBIE)) n++;
    return n;
}

const char *fam_hostname(void) { return g_hdr ? g_hdr->hostname : lkd_config_get()->hostname; }
const char *fam_domainname(void) { return g_hdr ? g_hdr->domainname : "(none)"; }

/* Fork: only the local mutex can be held by another thread; the fcntl lock is never held across a
 * call that can fork and is not inherited by the child anyway. */
void fam_fork_prepare(void) { pthread_mutex_lock(&g_create_mutex); pthread_mutex_lock(&g_mutex); }
void fam_fork_parent(void) { pthread_mutex_unlock(&g_mutex); pthread_mutex_unlock(&g_create_mutex); }
void fam_fork_child(void)
{
    lkd_fork_reinit_mutex(&g_mutex, 0);
    lkd_fork_reinit_mutex(&g_create_mutex, 0);
}

/* ---- Public lookups ----------------------------------------------------------------------------- */

int lkd_proc_lookup(int32_t pid, lkd_proc_info *out)
{
    if (fam_ready() < 0) return -LX_ESRCH;
    int r = -LX_ESRCH;
    /* A process of this host process: refresh its record first (state, threads, comm). */
    lkd_task *t = lkd_task_lookup(pid);
    fam_lock();
    if (t) { task_publish_locked(t); lkd_task_put(t); }
    struct fam_proc *p = fam_proc_find(pid);
    if (p) { *out = p->info; r = 0; }
    fam_unlock();
    return r;
}

int lkd_tid_lookup(int32_t tid, lkd_tid_info *out)
{
    if (fam_ready() < 0) return -LX_ESRCH;
    int r = -LX_ESRCH;
    /* Local threads: refresh the record first so the answer is current. */
    lkd_task *t = lkd_task_lookup(tid);
    fam_lock();
    if (t) task_publish_locked(t);
    struct fam_tid *p = fam_tid_find(tid);
    if (p) { *out = p->info; r = 0; }
    fam_unlock();
    if (t) lkd_task_put(t);
    return r;
}

static int cmp_pid(const void *a, const void *b)
{
    int32_t x = (*(const lkd_proc_info *const *)a)->pid, y = (*(const lkd_proc_info *const *)b)->pid;
    return x < y ? -1 : x > y;
}

int lkd_proc_list(lkd_proc_info *out, int max)
{
    if (fam_ready() < 0 || max <= 0) return 0;
    lkd_proc_info **idx = malloc(FAM_MAX_PROCS * sizeof *idx);
    if (!idx) return 0;
    int n = 0;
    fam_lock();
    for (int i = 0; i < FAM_MAX_PROCS; i++)
        if (g_procs[i].info.pid) idx[n++] = &g_procs[i].info;
    qsort(idx, (size_t)n, sizeof *idx, cmp_pid);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = *idx[i];
    fam_unlock();
    free(idx);
    return n;
}

int lkd_tid_list(int32_t tgid, int32_t *tids, int max)
{
    if (fam_ready() < 0) return -LX_ESRCH;
    int n = 0, found = 0;
    fam_lock();
    if (fam_proc_find(tgid)) found = 1;
    for (int i = 0; i < FAM_MAX_TIDS && n < max; i++) {
        if (!g_tids[i].info.tid || g_tids[i].info.tgid != tgid) continue;
        int32_t x = g_tids[i].info.tid;
        int j = n;
        while (j > 0 && tids[j - 1] > x) { tids[j] = tids[j - 1]; j--; }
        tids[j] = x;
        n++;
    }
    fam_unlock();
    return found || n ? n : -LX_ESRCH;
}
