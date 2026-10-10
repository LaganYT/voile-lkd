/* stubs_mm.c: weak placeholders for the mm module. Owner: core (until mm overrides them).
 * lkd_mm_check_range is permissive on purpose: with lkd_guest_base == 0 unit tests use host memory as
 * guest memory and rely on the host fault recovery of lkd_user_*. Everything else is -ENOSYS. */
#include "futex.h"
#include "mm.h"

LKD_WEAK int lkd_mm_check_range(uint64_t gva, size_t len, int need_write)
{
    (void)gva; (void)len; (void)need_write;
    return 0;
}

LKD_WEAK uint64_t lkd_mm_window_size(void) { return 0; }
LKD_WEAK int lkd_mm_code_write_fault(uint64_t gva) { (void)gva; return 0; }
LKD_WEAK int lkd_mm_watch_code(uint64_t gva, uint64_t len) { (void)gva; (void)len; return 0; }
LKD_WEAK int lkd_mm_code_watched(uint64_t gva) { (void)gva; return 0; }
LKD_WEAK lkd_mm *lkd_mm_create(void) { return NULL; }
LKD_WEAK lkd_mm *lkd_mm_get(lkd_mm *mm) { return mm; }
LKD_WEAK void lkd_mm_put(lkd_mm *mm) { (void)mm; }
LKD_WEAK lkd_mm *lkd_mm_clone(lkd_mm *parent, uint64_t clone_flags) { (void)clone_flags; return parent; }
LKD_WEAK uint64_t lkd_mm_identity(const lkd_mm *mm) { return (uint64_t)(uintptr_t)mm; }
LKD_WEAK void lkd_mm_reset_for_exec(lkd_mm *mm) { (void)mm; }
LKD_WEAK int64_t lkd_mm_map(lkd_mm *mm, uint64_t addr, uint64_t len, int prot, uint32_t flags,
                            struct lkd_fdobj *file, uint64_t offset, const char *name)
{
    (void)mm; (void)addr; (void)len; (void)prot; (void)flags; (void)file; (void)offset; (void)name;
    return -LX_ENOSYS;
}
LKD_WEAK int lkd_mm_unmap(lkd_mm *mm, uint64_t addr, uint64_t len) { (void)mm; (void)addr; (void)len; return -LX_ENOSYS; }
LKD_WEAK int lkd_mm_protect(lkd_mm *mm, uint64_t addr, uint64_t len, int prot) { (void)mm; (void)addr; (void)len; (void)prot; return -LX_ENOSYS; }
LKD_WEAK int64_t lkd_mm_remap(lkd_mm *mm, uint64_t oa, uint64_t ol, uint64_t nl, uint32_t fl, uint64_t na)
{
    (void)mm; (void)oa; (void)ol; (void)nl; (void)fl; (void)na;
    return -LX_ENOSYS;
}
LKD_WEAK int64_t lkd_mm_brk(lkd_mm *mm, uint64_t new_brk) { (void)mm; (void)new_brk; return -LX_ENOSYS; }
LKD_WEAK int lkd_mm_find(lkd_mm *mm, uint64_t addr, int next, struct lkd_vma_info *out)
{
    (void)mm; (void)addr; (void)next; (void)out;
    return -LX_ENOENT;
}
LKD_WEAK int lkd_mm_find_nowait(lkd_mm *mm, uint64_t addr, struct lkd_vma_info *out)
{
    (void)mm; (void)addr; (void)out;
    return -LX_ENOENT;
}
LKD_WEAK int lkd_mm_has_writable_shared_mapping(lkd_mm *mm, struct lkd_fdobj *file) { (void)mm; (void)file; return 0; }
LKD_WEAK int lkd_mm_set_vma_name(lkd_mm *mm, uint64_t addr, uint64_t len, const char *name)
{
    (void)mm; (void)addr; (void)len; (void)name;
    return -LX_ENOSYS;
}
LKD_WEAK char *lkd_mm_render_maps(lkd_mm *mm, size_t *len_out) { (void)mm; if (len_out) *len_out = 0; return NULL; }
LKD_WEAK char *lkd_mm_render_smaps(lkd_mm *mm, size_t *len_out) { (void)mm; if (len_out) *len_out = 0; return NULL; }
LKD_WEAK int lkd_mm_stats(lkd_mm *mm, struct lkd_mm_stats *out) { (void)mm; (void)out; return -LX_ENOSYS; }
LKD_WEAK int64_t lkd_mm_remote_read(int32_t pid, uint64_t gva, void *dst, size_t n) { (void)pid; (void)gva; (void)dst; (void)n; return -LX_ENOSYS; }
LKD_WEAK int64_t lkd_mm_remote_write(int32_t pid, uint64_t gva, const void *src, size_t n) { (void)pid; (void)gva; (void)src; (void)n; return -LX_ENOSYS; }
LKD_WEAK char *lkd_mm_remote_render_maps(int32_t pid, size_t *len_out, int *err)
{
    (void)pid;
    if (len_out) *len_out = 0;
    if (err) *err = -LX_ENOSYS;
    return NULL;
}

/* Placeholder key: private to the process, never shared (wrong for MAP_SHARED futexes; mm overrides). */
LKD_WEAK int lkd_mm_futex_key(lkd_task *t, uint64_t gva, int private_hint, struct lkd_futex_key *key)
{
    (void)private_hint;
    if (gva & 3) return -LX_EINVAL;
    key->kind = LKD_FK_PRIVATE;
    key->a = t && t->proc ? (uint64_t)t->proc->pid : 0;
    key->b = gva;
    return 0;
}
LKD_WEAK void lkd_mm_set_brk_start(lkd_mm *mm, uint64_t v) { (void)mm; (void)v; }
LKD_WEAK void lkd_mm_set_stack_start(lkd_mm *mm, uint64_t v) { (void)mm; (void)v; }
LKD_WEAK void lkd_mm_set_mmap_base(lkd_mm *mm, uint64_t v) { (void)mm; (void)v; }
LKD_WEAK uint64_t lkd_mm_window_guest_lo(void) { return 0; }
LKD_WEAK uint64_t lkd_mm_window_guest_hi(void) { return 0; }
LKD_WEAK void lkd_mm_file_written(struct lkd_fdobj *file, uint64_t off, uint64_t len) { (void)file; (void)off; (void)len; }
