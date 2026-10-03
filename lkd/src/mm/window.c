/* window.c: the host window that holds guest memory. Owner: mm.
 *
 * Production (lkd_config.guest_window_base != 0): one PROT_NONE, MAP_NORESERVE reservation of
 * guest_window_bytes (default 2^46) at the configured base, else at the first free candidate of the
 * measured free map (reports/fexcore-darwin-port.md: free 0x7d80000000..0x7ffffe000000, a 2^46
 * reservation succeeds, 2^47 fails; the prototype engine ran with a base of 8 TiB), else anywhere.
 * The reservation is tested with mach_vm_allocate(VM_FLAGS_FIXED), which fails instead of replacing an
 * existing mapping, then turned into a uniform mmap reservation.
 */
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mm_priv.h"

uint64_t mm_hps = 16384;
struct mm_window mm_win;

#define MACOS_USER_TOP 0x7ffffe000000ull /* measured, reports/fexcore-darwin-port.md */

/* "Empty" host range: no access and maximum protection none. XNU splits an anonymous mapping whose maximum protection is
 * not none into entries of ANON_CHUNK_SIZE (128 MiB): MESURE, a 2^46 PROT_NONE MAP_NORESERVE reservation is 524288
 * kernel VM map entries, the same range mapped with maximum protection none is 1. The kernel zone of map entries is
 * global; 332 runtime processes with such a window exhausted it and panicked the Mac (2026-10-02 20:21). Such a range
 * holds no data, so mm_host_protect re-creates it as fresh anonymous memory before giving it any access. */
static int empty_range(uint64_t host, uint64_t size)
{
    mach_vm_address_t a = host;
    kern_return_t k = mach_vm_map(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL,
                                  0, FALSE, VM_PROT_NONE, VM_PROT_NONE, VM_INHERIT_COPY);
    return k == KERN_SUCCESS ? 0 : -LX_ENOMEM;
}

static int try_fixed(uint64_t base, uint64_t size)
{
    if (base & (mm_hps - 1)) return -1;
    if (base + size < base || base + size > MACOS_USER_TOP) return -1;
    mach_vm_address_t a = base;
    if (mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) return -1;
    if (empty_range(base, size) != 0) {
        mach_vm_deallocate(mach_task_self(), base, size);
        return -1;
    }
    return 0;
}

int mm_window_reserve(uint64_t want, uint64_t size, int identity)
{
    if (mm_win.reserved) return 0;
    mm_hps = (uint64_t)getpagesize();
    size = HP_UP(size);
    if (size < 64 * mm_hps) return -LX_EINVAL;
    uint64_t base = 0;
    if (identity) {
        void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return LKD_HOST_ERR();
        base = (uint64_t)(uintptr_t)p;
    } else {
        static const uint64_t cand[] = { 0, 0x80000000000ull /* 8 TiB */, 0x100000000000ull, 0x200000000000ull,
                                         0x7d80000000ull };
        int ok = 0;
        for (unsigned i = 0; i < sizeof cand / sizeof cand[0] && !ok; i++) {
            uint64_t b = i == 0 ? want : cand[i];
            if (b && try_fixed(b, size) == 0) { base = b; ok = 1; }
        }
        if (!ok && want) {
            /* Diagnostic: what occupies the wanted range (a host mapping placed there before the reservation). */
            mach_vm_address_t r = want;
            for (int n = 0; n < 8; n++) {
                mach_vm_size_t rs = 0; vm_region_basic_info_data_64_t inf; mach_msg_type_number_t c = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t o;
                if (mach_vm_region(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&inf, &c, &o) != KERN_SUCCESS || r >= want + size) break;
                lkd_log("mm: wanted guest window range is occupied at 0x%llx size 0x%llx prot %d", (unsigned long long)r, (unsigned long long)rs, inf.protection);
                r += rs;
            }
        }
        if (!ok) {
            mach_vm_address_t a = 0;
            /* anywhere, 1 GiB aligned */
            if (mach_vm_map(mach_task_self(), &a, size, (1ull << 30) - 1, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0,
                            FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY) != KERN_SUCCESS)
                return -LX_ENOMEM;
            if (empty_range(a, size) != 0) return -LX_ENOMEM;
            base = a;
        }
        if (want && base != want) lkd_log("mm: guest window wanted at 0x%llx, placed at 0x%llx",
                                          (unsigned long long)want, (unsigned long long)base);
    }
    mm_win.host_lo = base;
    mm_win.size = size;
    mm_win.identity = identity;
    lkd_guest_base = identity ? 0 : base;
    mm_win.g_lo = identity ? base : 0;
    mm_win.g_hi = mm_win.g_lo + size;
    mm_win.min_addr = mm_win.g_lo + 0x10000; /* vm.mmap_min_addr = 65536 (Debian default) */
    mm_win.reserved = 1;
    return mm_watch_init();
}

void *mm_h(uint64_t g) { return lkd_g2h(g); }

int mm_host_reset(uint64_t g, uint64_t len)
{
    if (!len) return 0;
    if (mm_win.identity) {
        /* Test mode (identity window inside an ordinary host mapping): keep the plain mapping. */
        void *p = mmap(mm_h(g), len, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? (int)LKD_HOST_ERR() : 0;
    }
    return empty_range((uint64_t)(uintptr_t)mm_h(g), len);
}

/* Give access to [g, g+len). Parts that are still "empty" (maximum protection none, see empty_range) become fresh
 * anonymous zero memory first; parts that hold data are only mprotect'ed. Walked region by region so that a range mixing
 * both never loses data. */
int mm_host_protect(uint64_t g, uint64_t len, int hprot)
{
    if (!len) return 0;
    if (mprotect(mm_h(g), len, hprot) == 0) return 0;
    if (errno != EACCES && errno != ENOTSUP) return (int)LKD_HOST_ERR();
    uint64_t a = (uint64_t)(uintptr_t)mm_h(g), end = a + len;
    while (a < end) {
        mach_vm_address_t ra = a;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return -LX_ENOMEM;
        if (ra > a) return -LX_ENOMEM; /* a hole inside the window: never expected */
        uint64_t re = ra + rs < end ? ra + rs : end;
        if (info.max_protection == VM_PROT_NONE) {
            void *p = mmap((void *)(uintptr_t)a, re - a, hprot, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
            if (p == MAP_FAILED) return (int)LKD_HOST_ERR();
        } else if (mprotect((void *)(uintptr_t)a, re - a, hprot) < 0) {
            return (int)LKD_HOST_ERR();
        }
        a = re;
    }
    return 0;
}

/* x86 page tables cannot express write-only or execute-only: any access right implies read. */
int mm_host_prot_of(uint32_t p)
{
    int h = 0;
    if (p & (LX_PROT_READ | LX_PROT_WRITE | LX_PROT_EXEC)) h |= PROT_READ;
    if (p & LX_PROT_WRITE) h |= PROT_WRITE;
    return h;
}
