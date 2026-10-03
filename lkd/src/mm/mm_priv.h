/* mm_priv.h: private structures of the mm module. Owner: mm.
 *
 * Guest memory lives in one host window [win.host_lo, win.host_lo + win.size). The guest address g is
 * at host address lkd_guest_base + g. Two modes:
 *   production: lkd_guest_base = win.host_lo, guest addresses are [0, size) (configured by
 *               lkd_config.guest_window_base != 0, reserved in lkd_mm_init);
 *   identity:   lkd_guest_base = 0, guest addresses are the host addresses of the window (unit tests;
 *               chosen when no window base was configured, reserved by the first lkd_mm_create).
 * Guest constants that Linux defines as absolute addresses (mmap_min_addr, the MAP_32BIT range) are
 * relative to win.g_lo, the guest address of the window start (0 in production).
 *
 * Host pages are 16 KiB, guest pages 4 KiB. Invariants kept by every operation:
 *   I1 a host page that no VMA overlaps is a fresh piece of the reservation (PROT_NONE, zero, private);
 *   I2 the host protection of a host page is the union of its VMAs (r if any is not PROT_NONE, w if any
 *      is writable); host execute permission is never used (the engine translates guest code);
 *   I3 a host page overlapped by a host-shared VMA (MAP_SHARED anonymous, SysV shm, MAP_SHARED file)
 *      belongs to that mapping alone: no private VMA overlaps it. Host-shared VMAs start on a host page
 *      boundary; their tail host page may hold guest-unmapped sub-pages.
 */
#ifndef LKD_MM_PRIV_H
#define LKD_MM_PRIV_H

#include "lkd_internal.h"
#include "mm.h"
#include "fd.h"

/* Core function (src/core/core_priv.h): effective configuration. Requested in docs/requests/mm-1.md. */
const lkd_config *lkd_config_get(void);

#define GP 4096ull                       /* guest page */
#define GP_ALIGN(x) (((x) + GP - 1) & ~(GP - 1))
extern uint64_t mm_hps;                  /* host page size (16 KiB on arm64 macOS) */
#define HP_DOWN(x) ((x) & ~(mm_hps - 1))
#define HP_UP(x) (((x) + mm_hps - 1) & ~(mm_hps - 1))

/* Backing object kinds. */
enum { MB_FILE = 1, MB_SHANON = 2, MB_SYSV = 3 };

/* What a VMA maps; shared by the pieces of a split VMA and by mappings of the same open file. */
struct mm_backing {
    _Atomic int refs;
    int kind;
    struct lkd_fdobj *file;      /* MB_FILE: referenced open file description */
    int host_fd;                 /* MB_FILE: the file's host fd (owned by the fd object, not closed here) */
    uint64_t file_size;          /* MB_FILE: size at map time in file (guest) offsets */
    uint64_t hbias;              /* MB_FILE: host offset = file offset + hbias (memfd phase, request wine-7) */
    uint32_t dev_major, dev_minor;
    uint64_t ino;
    char *name;                  /* path or label shown in maps (malloc) */
    uint8_t deleted;             /* render " (deleted)" */
    uint8_t writable;            /* MB_FILE: opened for writing (VM_MAYWRITE of a shared mapping) */
    uint64_t obj_id;             /* MB_SHANON: family-unique id for futex keys */
    uint64_t obj_size;           /* MB_SHANON/MB_SYSV: size of the shared object (host pages) */
    int shm_slot;                /* MB_SYSV: slot in the SysV table */
    int shm_id;                  /* MB_SYSV: shmid */
};

/* VMA flags (vflags). */
#define VF_SHARED      0x0001u  /* MAP_SHARED: 's' in maps */
#define VF_HOSTSHARED  0x0002u  /* host-shared mapping (see I3) */
#define VF_MAYWRITE    0x0004u  /* mprotect(PROT_WRITE) allowed */
#define VF_LOCKED      0x0008u
#define VF_LOCKONFAULT 0x0010u
#define VF_DONTFORK    0x0020u
#define VF_WIPEONFORK  0x0040u
#define VF_DONTDUMP    0x0080u
#define VF_NORESERVE   0x0100u
#define VF_HUGEPAGE    0x0200u
#define VF_NOHUGEPAGE  0x0400u
#define VF_RANDOM      0x0800u
#define VF_SEQ         0x1000u
#define VF_MERGEABLE   0x2000u
#define VF_ACCOUNT_MASK (VF_SHARED | VF_HOSTSHARED | VF_MAYWRITE | VF_LOCKED | VF_LOCKONFAULT | VF_DONTFORK | \
                         VF_WIPEONFORK | VF_DONTDUMP | VF_NORESERVE | VF_HUGEPAGE | VF_NOHUGEPAGE | VF_RANDOM | \
                         VF_SEQ | VF_MERGEABLE)

/* Label kinds of anonymous VMAs. */
enum { LBL_NONE = 0, LBL_SPECIAL = 1 /* [vdso] [vvar] [vsyscall] */, LBL_STACK = 2, LBL_ANON = 3 /* [anon:x] */ };

typedef struct mm_vma {
    uint64_t start, end;         /* guest, 4 KiB aligned, end exclusive */
    uint64_t pgoff;              /* byte offset in the backing of `start` (0 for private anonymous) */
    uint32_t prot;               /* LX_PROT_READ|WRITE|EXEC */
    uint32_t vflags;
    struct mm_backing *bk;       /* NULL: private anonymous */
    char *label;                 /* malloc'ed text for LBL_*, or NULL */
    uint8_t lkind;
    /* AVL tree by start, augmented with the largest gap below a node in its subtree. */
    struct mm_vma *l, *r;
    int h;
    uint64_t prev_end;           /* end of the predecessor (window low bound for the first VMA) */
    uint64_t maxgap;
} mm_vma;

typedef struct vtree {
    mm_vma *root;
    size_t n;
    uint64_t lo;                 /* prev_end of the first VMA */
} vtree;

/* ---- vma.c: AVL tree --------------------------------------------------------------------------- */
void vt_init(vtree *t, uint64_t lo);
void vt_insert(vtree *t, mm_vma *v);
void vt_remove(vtree *t, mm_vma *v);
void vt_set_start(vtree *t, mm_vma *v, uint64_t s);   /* order must be preserved */
void vt_set_end(vtree *t, mm_vma *v, uint64_t e);
mm_vma *vt_find(const vtree *t, uint64_t addr);       /* containing addr */
mm_vma *vt_first_end_after(const vtree *t, uint64_t addr); /* first VMA with end > addr (Linux find_vma) */
mm_vma *vt_last_start_before(const vtree *t, uint64_t addr); /* last VMA with start < addr */
mm_vma *vt_next(const vtree *t, const mm_vma *v);
mm_vma *vt_prev(const vtree *t, const mm_vma *v);
mm_vma *vt_first(const vtree *t);
mm_vma *vt_last(const vtree *t);
int vt_check(const vtree *t);                         /* invariant check for tests: 0 ok */
/* Gap search. Calls fit(ctx, gap_lo, gap_hi, prev, next) for raw gaps of at least `len` bytes inside
 * [low, high), from the highest (topdown) or the lowest; stops when fit returns nonzero and returns that
 * value. The gap above the last VMA is bounded by `top`. */
typedef uint64_t (*vt_fit_fn)(void *ctx, uint64_t glo, uint64_t ghi, mm_vma *prev, mm_vma *next);
uint64_t vt_search(const vtree *t, uint64_t len, uint64_t low, uint64_t high, uint64_t top, int topdown,
                   vt_fit_fn fit, void *ctx);

/* ---- The address space ------------------------------------------------------------------------- */

#define MM_RSS_TTL_NS 100000000ll   /* lkd_mm_stats RSS cache validity: 100 ms */

struct lkd_mm {
    _Atomic int refs;
    pthread_mutex_t lock;        /* mmap_lock, LKD_RANK_MM */
    pthread_rwlock_t vlock;      /* VMA leaf lock, LKD_RANK_VMA: tree readers vs writers */
    vtree vt;
    uint64_t identity;
    uint64_t start_brk, brk;     /* guest; 0 = unknown (brk then fails like an absent heap) */
    uint64_t start_stack;
    uint64_t mmap_base;          /* top of the top-down search */
    uint32_t def_flags;          /* VF_LOCKED | VF_LOCKONFAULT from mlockall(MCL_FUTURE) */
    uint64_t locked_bytes;
    uint64_t hiwater_vm, hiwater_rss; /* VmPeak, VmHWM (lkd_mm_stats) */
    /* RSS cache of lkd_mm_stats (a mincore walk of every VMA): reused while younger than MM_RSS_TTL_NS and
     * while no mapping was added or removed (map_gen). Pages touched or reclaimed meanwhile show up at the
     * next refresh: RSS is up to MM_RSS_TTL_NS old. */
    uint64_t map_gen, rss_gen;
    int64_t rss_ns;
    uint64_t rss_anon, rss_file, rss_shmem;
};

/* Window (one per host process). */
struct mm_window {
    uint64_t host_lo, size;      /* host range */
    uint64_t g_lo, g_hi;         /* guest range */
    uint64_t min_addr;           /* guest mmap_min_addr equivalent (g_lo + 64 KiB) */
    int identity;
    int reserved;
    lkd_mm *owner;               /* the live address space (at most one) */
};
extern struct mm_window mm_win;

/* Collected JIT invalidations, issued after the locks are dropped. */
#define MM_INVAL_MAX 32
struct mm_inval { int n; int overflow; uint64_t s[MM_INVAL_MAX], e[MM_INVAL_MAX]; };
void mm_inval_add(struct mm_inval *iv, uint64_t s, uint64_t e);
void mm_inval_flush(struct mm_inval *iv);
void mm_invalidate(uint64_t s, uint64_t len); /* ops->invalidate_code */

/* ---- window.c ---------------------------------------------------------------------------------- */
int mm_window_reserve(uint64_t want_host_base, uint64_t size, int identity);
void *mm_h(uint64_t g);                                      /* host pointer of guest address */
int mm_host_reset(uint64_t g, uint64_t len);                 /* fresh reservation (I1); host page aligned */
int mm_host_protect(uint64_t g, uint64_t len, int hprot);    /* host page aligned */
int mm_host_prot_of(uint32_t lxprot);                        /* guest prot -> host prot bits */

/* ---- mm.c internals shared with the syscall files ---------------------------------------------- */
void mm_wlock(lkd_mm *mm);       /* VMA write lock (rank 90), held only around tree mutations */
void mm_wunlock(lkd_mm *mm);
void mm_lock(lkd_mm *mm);        /* mmap_lock */
void mm_unlock(lkd_mm *mm);
lkd_mm *mm_of(lkd_task *t);
struct mm_backing *mb_ref(struct mm_backing *b);
void mb_put(struct mm_backing *b);
struct mm_backing *mb_new(int kind);

/* A mapping request, after syscall-level validation. */
struct mm_mapreq {
    uint64_t addr, len;          /* len 4 KiB aligned */
    uint32_t prot;
    uint32_t lxflags;            /* LX_MAP_* */
    uint32_t vflags;             /* VF_* to set */
    struct mm_backing *bk;       /* referenced by the caller; the VMA takes its own reference */
    uint64_t pgoff;              /* byte offset in the backing */
    const char *label; uint8_t lkind;
    int fixed, noreplace;
};
int64_t mm_map_locked(lkd_mm *mm, struct mm_mapreq *rq, struct mm_inval *iv);
int mm_unmap_locked(lkd_mm *mm, uint64_t start, uint64_t end, struct mm_inval *iv);
int mm_protect_locked(lkd_mm *mm, uint64_t start, uint64_t end, uint32_t prot, struct mm_inval *iv);
int mm_host_refresh(lkd_mm *mm, uint64_t start, uint64_t end);  /* re-apply I2 on host pages */
void mm_host_cleanup(lkd_mm *mm, uint64_t start, uint64_t end); /* I1/I2 after VMAs left [start,end) */
int mm_fill(lkd_mm *mm, const mm_vma *v, uint64_t s, uint64_t e); /* zero or file content into [s,e) */
int mm_discard_range(lkd_mm *mm, const mm_vma *v, uint64_t s, uint64_t e); /* zero/refill, O(boundary) */
int mm_split(lkd_mm *mm, uint64_t addr);                         /* split the VMA containing addr at addr */
void mm_merge_around(lkd_mm *mm, uint64_t start, uint64_t end);  /* merge VMAs touching [start,end] */
int mm_range_mapped(lkd_mm *mm, uint64_t start, uint64_t end);   /* 1 if fully covered */
int mm_host_conflict(lkd_mm *mm, uint64_t s, uint64_t e, int shared, uint32_t prot,
                     const struct mm_backing *bk, uint64_t pgoff); /* I3, see mm.c */
int mm_check_range_mm(lkd_mm *mm, uint64_t gva, uint64_t len, int need_write, int force);
uint64_t mm_get_unmapped(lkd_mm *mm, uint64_t hint, uint64_t len, uint64_t pgoff, uint32_t lxflags,
                         int hostshared, int filemap, int64_t *err);
int mm_temp_access(lkd_mm *mm, uint64_t s, uint64_t e, int write); /* host RW for [s,e) (caller restores) */
void mm_vma_free(mm_vma *v);
mm_vma *mm_vma_clone(const mm_vma *v);
int mm_shm_detach_vma(const mm_vma *v);  /* shm.c: nattch-- for a SysV VMA being destroyed */
void mm_shm_attach_vma(const mm_vma *v);  /* shm.c: nattch++ for a new SysV VMA (map, split, move) */
void mm_shm_fork_child(lkd_mm *mm);      /* shm.c: nattch++ for inherited attachments */
void mm_remote_fork_child(void);         /* remote.c */
/* watch.c: host pages holding translated code (lkd_mm_watch.h) */
int mm_watch_init(void);
void mm_watch_lock(void);                /* recursive; before the VMA lock, never across guest code, */
void mm_watch_unlock(void);              /* ops->invalidate_code or a copy that may hit a watched page */
int mm_watched(uint64_t host_page_gva);
int mm_unwatch_bit(uint64_t host_page_gva);
void mm_watch_clear_all(void);
void mm_watch_fork_child(void);
void mm_unwatch_range(uint64_t p, uint64_t q);          /* host pages [p, q), any size */
uint64_t mm_next_watched(uint64_t p, uint64_t q);      /* first watched host page in [p, q), or q */
int mm_same_object(const struct mm_backing *a, const struct mm_backing *b); /* same shared object */
/* Host pages of this process that map the same bytes of a shared object as host page p, p included (at
 * most cap). VMA read lock taken inside. */
int mm_alias_pages(lkd_mm *mm, uint64_t p, uint64_t *out, int cap);
void mm_watch_alias_sync(lkd_mm *mm, uint64_t s, uint64_t e); /* new shared VMA: inherit the watch of its aliases */
int mm_unwatch_for_write(lkd_mm *mm, uint64_t gva, int need_vma_write, struct mm_inval *iv); /* iv NULL: invalidate now */
void mm_remote_stop(void);
int mm_remote_start(void);

/* Linux personality bit (include/uapi/linux/personality.h). */
#define LKD_READ_IMPLIES_EXEC 0x0400000u
/* Linux capability numbers (include/uapi/linux/capability.h). */
#define LKD_CAP_IPC_LOCK 14
#define LKD_CAP_IPC_OWNER 15
#define LKD_CAP_SYS_ADMIN 21
int mm_capable(lkd_task *t, int cap);

#endif
