/* fdrec.c: records, the host form of an open file description that has no host descriptor (fd.h). Owner: core.
 *
 * A record is a POSIX shm object of 16 KiB, unlinked as soon as it exists, so it lives exactly as long as a
 * descriptor on it (in flight in a socket buffer or held by a receiver). It is written once by the sender and only
 * read afterwards. A received POSIX shm object has no file type bits (MESURE, net/scm.c); it is a record when its
 * size and magic match and its lengths are consistent. The eventfd region has the same size and its own magic.
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lkd_internal.h"
#include "fd.h"

#define REC_SIZE 16384u
#define REC_MAGIC "LKDFREC1"

struct rec_hdr {
    char magic[8];
    uint32_t version;
    uint32_t type;
    uint32_t status_flags;
    uint32_t len;
    uint32_t reserved[2];
};
_Static_assert(sizeof(struct rec_hdr) == 32, "record header");
_Static_assert(REC_SIZE - sizeof(struct rec_hdr) == LKD_FDREC_MAX_DATA, "record data size");

static _Atomic(lkd_fdrec_import_fn) g_import[LKD_FDREC_TYPES];

void lkd_fdrec_register(uint32_t type, lkd_fdrec_import_fn fn)
{
    if (type > 0 && type < LKD_FDREC_TYPES) atomic_store(&g_import[type], fn);
}

int lkd_fdrec_export(uint32_t type, uint32_t status_flags, const void *data, size_t len, int *hfd)
{
    if (type == 0 || type >= LKD_FDREC_TYPES || len > LKD_FDREC_MAX_DATA) return -LX_EINVAL;
    static _Atomic uint32_t seq;
    char name[32];
    int fd = -1;
    for (int i = 0; i < 16 && fd < 0; i++) {
        snprintf(name, sizeof name, "/lkdrec.%d.%u", (int)getpid(), atomic_fetch_add(&seq, 1));
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno != EEXIST) return (int)LKD_HOST_ERR();
    }
    if (fd < 0) return -LX_ENFILE;
    shm_unlink(name);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(fd, REC_SIZE) < 0) { int e = (int)LKD_HOST_ERR(); close(fd); return e; }
    uint8_t *p = mmap(NULL, REC_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { int e = (int)LKD_HOST_ERR(); close(fd); return e; }
    struct rec_hdr h = { .version = 1, .type = type, .status_flags = status_flags, .len = (uint32_t)len };
    memcpy(h.magic, REC_MAGIC, 8);
    if (len) memcpy(p + sizeof h, data, len);
    memcpy(p, &h, sizeof h);
    munmap(p, REC_SIZE);
    *hfd = fd;
    return 1;
}

int lkd_fdrec_import(lkd_task *t, int host_fd, lkd_fdobj **out)
{
    struct stat st;
    if (fstat(host_fd, &st) < 0 || (st.st_mode & S_IFMT) != 0 || st.st_size != REC_SIZE) return 0;
    const uint8_t *p = mmap(NULL, REC_SIZE, PROT_READ, MAP_SHARED, host_fd, 0);
    if (p == MAP_FAILED) return 0;
    struct rec_hdr h;
    memcpy(&h, p, sizeof h);
    if (memcmp(h.magic, REC_MAGIC, 8) != 0 || h.version != 1 || h.len > LKD_FDREC_MAX_DATA) {
        munmap((void *)p, REC_SIZE);
        return 0;
    }
    int rc = -LX_EOPNOTSUPP;
    lkd_fdrec_import_fn fn = h.type > 0 && h.type < LKD_FDREC_TYPES ? atomic_load(&g_import[h.type]) : NULL;
    void *data = malloc(h.len ? h.len : 1);
    if (!data) rc = -LX_ENOMEM;
    else if (fn) {
        memcpy(data, p + sizeof h, h.len);   /* the sender may still hold the region: work on a copy */
        rc = fn(t, h.status_flags, data, h.len, out);
    }
    free(data);
    munmap((void *)p, REC_SIZE);
    if (rc < 0) lkd_log_once("fdrec: a received record of type %u could not be rebuilt (%d)", h.type, rc);
    return rc < 0 ? rc : 1;
}
