/*
 * a_addrspace.c - fact group A: address space of a macOS arm64 (or Rosetta
 * x86_64) process.
 *
 * Build several variants of this one file with different linker -pagezero_size
 * values, entitlements and hardened-runtime flags (see build.sh).  Every probe
 * that could replace an existing mapping uses mach_vm_allocate(VM_FLAGS_FIXED)
 * (which refuses to overwrite) or checks the range is free first; nothing here
 * can destroy the program's own mappings.
 *
 * usage: a_addrspace [--map] [--brief]
 *   --map    dump the whole top-level region list (with submap and tag info)
 *   --brief  print one BRIEF line (image base, min/max address) and exit
 */
#include "common.h"
#include <crt_externs.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <sys/resource.h>

#ifndef MAP_32BIT
#define MAP_32BIT 0x8000
#endif

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define GiB (1ULL << 30)
#define TiB (1ULL << 40)

/* csops(2) is not in the public SDK headers; the numbers below are from XNU. */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define DM_CS_OPS_STATUS 0
#define DM_CS_OPS_ENTITLEMENTS_BLOB 7

/* Values collected for the SUMMARY line. */
static struct {
  mach_vm_address_t exe_base, min_address, max_address;
  mach_vm_address_t lowest_alloc, highest_alloc_end;
  unsigned long long largest_resv_gib;
  unsigned long long guest_total_mib, guest_reservable_mib, guest_occupied_mib, guest_blocked_low_mib, guest_blocked_high_mib, guest_blocked_other_mib;
  int fixed_ok[5];
} S;

static kern_return_t try_fixed(mach_vm_address_t addr, mach_vm_size_t size) {
  mach_vm_address_t a = addr;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED);
  if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, size);
  return kr;
}

/* Run-length encoded scan of try_fixed over [from, to) with the given step. */
static void scan_rle(const char *id, const char *what, mach_vm_address_t from,
                     mach_vm_address_t to, mach_vm_size_t step, mach_vm_size_t probe) {
  kern_return_t cur = (kern_return_t)-12345;
  mach_vm_address_t seg = from;
  int nseg = 0;
  mach_vm_address_t first_ok = 0;
  bool have_ok = false;
  for (mach_vm_address_t a = from; a < to; a += step) {
    kern_return_t kr = try_fixed(a, probe);
    if (kr == KERN_SUCCESS && !have_ok) { have_ok = true; first_ok = a; }
    if (kr != cur) {
      if (cur != (kern_return_t)-12345 && nseg < 60)
        INFO(id, "%s [0x%llx, 0x%llx) -> %s", what, (unsigned long long)seg, (unsigned long long)a, kr_name(cur));
      if (cur != (kern_return_t)-12345) nseg++;
      cur = kr;
      seg = a;
    }
  }
  if (cur != (kern_return_t)-12345 && nseg < 60)
    INFO(id, "%s [0x%llx, 0x%llx) -> %s", what, (unsigned long long)seg, (unsigned long long)to, kr_name(cur));
  if (have_ok)
    INFO(id, "%s first allocatable address in scan = 0x%llx", what, (unsigned long long)first_ok);
  else
    INFO(id, "%s no allocatable address in [0x%llx, 0x%llx)", what, (unsigned long long)from, (unsigned long long)to);
}

static void a01_env(void) {
  char buf[128];
  size_t l = sizeof buf;
  INFO("A01", "arch=%s translated=%d pid=%d", arch_name(), is_translated(), getpid());
  l = sizeof buf; if (!sysctlbyname("kern.osproductversion", buf, &l, NULL, 0)) INFO("A01", "kern.osproductversion=%s", buf);
  l = sizeof buf; if (!sysctlbyname("kern.osversion", buf, &l, NULL, 0)) INFO("A01", "kern.osversion=%s", buf);
  l = sizeof buf; if (!sysctlbyname("machdep.cpu.brand_string", buf, &l, NULL, 0)) INFO("A01", "cpu=%s", buf);
  uint64_t mem = 0; l = sizeof mem;
  if (!sysctlbyname("hw.memsize", &mem, &l, NULL, 0)) INFO("A01", "hw.memsize=%llu (%.1f GiB)", (unsigned long long)mem, mem / (double)GiB);
  INFO("A01", "sizeof(void*)=%zu vm_page_size=%lu vm_kernel_page_size=%lu", sizeof(void *),
       (unsigned long)vm_page_size, (unsigned long)vm_kernel_page_size);
}

/* Code-signing state and entitlements as seen by the running process. */
static void a01b_codesign(void) {
  uint32_t flags = 0;
  int r = csops(getpid(), DM_CS_OPS_STATUS, &flags, sizeof flags);
  if (r != 0) { INFO("A01B", "csops(CS_OPS_STATUS) failed errno=%s", errno_name(errno)); return; }
  static const struct { uint32_t bit; const char *n; } names[] = {
      {0x1, "VALID"}, {0x2, "ADHOC"}, {0x4, "GET_TASK_ALLOW"}, {0x10, "FORCED_LV"}, {0x100, "HARD"}, {0x200, "KILL"},
      {0x800, "RESTRICT"}, {0x1000, "ENFORCEMENT"}, {0x2000, "REQUIRE_LV"}, {0x10000, "RUNTIME"}, {0x20000, "LINKER_SIGNED"},
      {0x4000000, "PLATFORM_BINARY"}};
  char line[300]; int off = 0;
  for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
    if (flags & names[i].bit) off += snprintf(line + off, sizeof line - off, " %s", names[i].n);
  INFO("A01B", "cs_flags=0x%x:%s hardened_runtime=%d", flags, line, !!(flags & 0x10000));
  uint8_t blob[16384];
  memset(blob, 0, sizeof blob);
  r = csops(getpid(), DM_CS_OPS_ENTITLEMENTS_BLOB, blob, sizeof blob);
  if (r != 0) { INFO("A01B", "entitlements blob: none visible (csops errno=%s)", errno_name(errno)); return; }
  const char *keys[] = {"com.apple.developer.kernel.extended-virtual-addressing", "com.apple.developer.kernel.increased-memory-limit",
                        "com.apple.security.cs.allow-jit", "com.apple.security.cs.allow-unsigned-executable-memory",
                        "com.apple.security.cs.disable-library-validation"};
  for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++)
    INFO("A01B", "entitlement %s visible to the process: %d", keys[i], memmem(blob, sizeof blob, keys[i], strlen(keys[i])) != NULL);
}

static void a02_macho(void) {
  const struct mach_header_64 *mh = (const struct mach_header_64 *)_NSGetMachExecuteHeader();
  const uint8_t *p = (const uint8_t *)(mh + 1);
  uint64_t text_vmaddr = 0, pagezero = 0;
  bool has_pagezero = false;
  for (uint32_t i = 0; i < mh->ncmds; i++) {
    const struct load_command *lc = (const struct load_command *)p;
    if (lc->cmd == LC_SEGMENT_64) {
      const struct segment_command_64 *sc = (const struct segment_command_64 *)lc;
      char name[17] = {0};
      memcpy(name, sc->segname, 16);
      if (!strcmp(name, "__PAGEZERO")) { has_pagezero = true; pagezero = sc->vmsize; }
      if (!strcmp(name, "__TEXT")) text_vmaddr = sc->vmaddr;
      INFO("A02", "segment %-12s vmaddr=0x%llx vmsize=0x%llx fileoff=0x%llx filesize=0x%llx maxprot=%d initprot=%d",
           name, (unsigned long long)sc->vmaddr, (unsigned long long)sc->vmsize,
           (unsigned long long)sc->fileoff, (unsigned long long)sc->filesize, sc->maxprot, sc->initprot);
    } else if (lc->cmd == LC_BUILD_VERSION) {
      const struct build_version_command *bv = (const struct build_version_command *)lc;
      INFO("A02", "LC_BUILD_VERSION platform=%u minos=%u.%u.%u sdk=%u.%u.%u", bv->platform,
           bv->minos >> 16, (bv->minos >> 8) & 0xff, bv->minos & 0xff, bv->sdk >> 16,
           (bv->sdk >> 8) & 0xff, bv->sdk & 0xff);
    }
    p += lc->cmdsize;
  }
  INFO("A02", "mach_header flags=0x%x cputype=0x%x cpusubtype=0x%x (MH_PIE=%d)", mh->flags, mh->cputype, mh->cpusubtype, !!(mh->flags & 0x200000));
  S.exe_base = (mach_vm_address_t)mh;
  INFO("A02", "linked __PAGEZERO present=%d size=0x%llx, __TEXT linked at 0x%llx, loaded at 0x%llx, ASLR slide = 0x%llx (%.2f MiB)",
       has_pagezero, (unsigned long long)pagezero, (unsigned long long)text_vmaddr, (unsigned long long)(uintptr_t)mh,
       (unsigned long long)((uintptr_t)mh - text_vmaddr), ((uintptr_t)mh - text_vmaddr) / (double)MiB);
}

static void a03_taskinfo(void) {
  task_vm_info_data_t ti;
  mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
  kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&ti, &c);
  if (kr != KERN_SUCCESS) { FAIL("A03", "task_info(TASK_VM_INFO) kr=%s", kr_name(kr)); return; }
  S.min_address = ti.min_address;
  S.max_address = ti.max_address;
  INFO("A03", "TASK_VM_INFO page_size=%d min_address=0x%llx max_address=0x%llx virtual_size=%llu region_count=%d",
       ti.page_size, (unsigned long long)ti.min_address, (unsigned long long)ti.max_address,
       (unsigned long long)ti.virtual_size, ti.region_count);
  INFO("A03", "TASK_VM_INFO phys_footprint=%llu limit_bytes_remaining=%llu (footprint limit signal; increased-memory-limit would raise it)",
       (unsigned long long)ti.phys_footprint, (unsigned long long)ti.limit_bytes_remaining);
  INFO("A03", "header constant MACH_VM_MAX_ADDRESS=0x%llx MACH_VM_MIN_ADDRESS=0x%llx", (unsigned long long)MACH_VM_MAX_ADDRESS,
       (unsigned long long)MACH_VM_MIN_ADDRESS);
  INFO("A03", "min_address - image base = 0x%llx (0 means the slid __PAGEZERO ends exactly at __TEXT)", (unsigned long long)(ti.min_address - S.exe_base));
  CHECK(ti.max_address >= 0x7ffe00000000ULL, "A03", "task max_address (0x%llx) is at least 47-bit minus 32 MiB", (unsigned long long)ti.max_address);
  struct rlimit rl;
  if (!getrlimit(RLIMIT_AS, &rl)) INFO("A03", "RLIMIT_AS cur=%llu max=%llu", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
  if (!getrlimit(RLIMIT_DATA, &rl)) INFO("A03", "RLIMIT_DATA cur=%llu max=%llu", (unsigned long long)rl.rlim_cur, (unsigned long long)rl.rlim_max);
}

/* Top-level region walk with submap and tag information. */
static void a04_regions(bool full) {
  mach_vm_address_t addr = 0;
  int n = 0, nsub = 0;
  mach_vm_address_t prev_end = 0;
  struct { mach_vm_address_t start; mach_vm_size_t size; } gaps[8];
  int ngaps = 0;
  mach_vm_size_t total = 0, submap_total = 0;
  mach_vm_address_t lowest = 0, highest_end = 0;
  bool first = true;
  for (;;) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt);
    if (kr != KERN_SUCCESS) break;
    char p1[4], p2[4];
    if (first) { lowest = a; first = false; }
    if (a > prev_end) {
      mach_vm_size_t g = a - prev_end;
      int pos = -1;
      if (ngaps < 8) pos = ngaps++;
      else { int mi = 0; for (int i = 1; i < 8; i++) if (gaps[i].size < gaps[mi].size) mi = i; if (gaps[mi].size < g) pos = mi; }
      if (pos >= 0) { gaps[pos].start = prev_end; gaps[pos].size = g; }
    }
    if (info.is_submap) { nsub++; submap_total += sz; }
    if (full || n < 14 || info.is_submap || sz >= 16 * GiB)
      INFO("A04", "region[%d] 0x%llx-0x%llx size=0x%llx prot=%s max=%s share=%d submap=%d tag=%u depth=%u", n, (unsigned long long)a,
           (unsigned long long)(a + sz), (unsigned long long)sz, prot_str(info.protection, p1), prot_str(info.max_protection, p2),
           info.share_mode, info.is_submap, info.user_tag, depth);
    total += sz;
    prev_end = a + sz;
    highest_end = prev_end;
    addr = prev_end;
    n++;
    if (n > 100000) break;
  }
  INFO("A04", "%d top-level regions (%d submaps covering %llu MiB), mapped total %llu MiB, lowest region start 0x%llx, highest region end 0x%llx", n, nsub,
       (unsigned long long)(submap_total / MiB), (unsigned long long)(total / MiB), (unsigned long long)lowest, (unsigned long long)highest_end);
  S.highest_alloc_end = highest_end;
  if (highest_end < MACH_VM_MAX_ADDRESS) {
    mach_vm_size_t g = MACH_VM_MAX_ADDRESS - highest_end;
    int pos = -1;
    if (ngaps < 8) pos = ngaps++;
    else { int mi = 0; for (int i = 1; i < 8; i++) if (gaps[i].size < gaps[mi].size) mi = i; if (gaps[mi].size < g) pos = mi; }
    if (pos >= 0) { gaps[pos].start = highest_end; gaps[pos].size = g; }
  }
  for (int i = 0; i < ngaps; i++)
    for (int j = i + 1; j < ngaps; j++)
      if (gaps[j].size > gaps[i].size) { __typeof__(gaps[0]) t = gaps[i]; gaps[i] = gaps[j]; gaps[j] = t; }
  for (int i = 0; i < ngaps; i++)
    INFO("A04", "largest free gap #%d: 0x%llx-0x%llx = %.3f GiB (%.3f TiB)", i, (unsigned long long)gaps[i].start,
         (unsigned long long)(gaps[i].start + gaps[i].size), gaps[i].size / (double)GiB, gaps[i].size / (double)TiB);
}

static void a05_low(void) {
  static const mach_vm_address_t pts[] = {0x0, 0x1000, 0x4000, 0x8000, 0x10000, 0x20000, 0x100000, 0x7ffe0000ULL,
                                          0x80000000ULL, 0xFFFFC000ULL, 0x100000000ULL, 0x140000000ULL};
  for (unsigned i = 0; i < sizeof pts / sizeof *pts; i++) {
    kern_return_t kr = try_fixed(pts[i], vm_page_size);
    INFO("A05", "mach_vm_allocate(FIXED, 0x%llx, %lu) -> %s (%d)", (unsigned long long)pts[i], (unsigned long)vm_page_size, kr_name(kr), kr);
  }
  /* mmap(MAP_FIXED) at 0 and at 0x1000: only when mach_vm_allocate says it is free */
  for (unsigned i = 0; i < 2; i++) {
    mach_vm_address_t a = i ? 0x1000 : 0;
    if (try_fixed(a, vm_page_size) == KERN_SUCCESS) {
      void *p = mmap((void *)a, vm_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      int e = errno;
      if (p != MAP_FAILED) { INFO("A05", "mmap(MAP_FIXED, 0x%llx) succeeded", (unsigned long long)a); munmap(p, vm_page_size); }
      else INFO("A05", "mmap(MAP_FIXED, 0x%llx) failed errno=%s", (unsigned long long)a, errno_name(e));
    } else {
      INFO("A05", "mmap(MAP_FIXED, 0x%llx) not attempted: mach_vm_allocate says not free/mappable", (unsigned long long)a);
    }
  }
  /* the same address through mmap itself, on addresses that mach_vm_allocate refused */
  static const mach_vm_address_t mm[] = {0x10000, 0x7ffe0000ULL, 0x100000000ULL};
  for (unsigned i = 0; i < sizeof mm / sizeof *mm; i++) {
    if (!dm_range_is_free(mm[i], vm_page_size)) continue;
    void *p = mmap((void *)mm[i], vm_page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    int e = errno;
    INFO("A05", "mmap(0x%llx, PROT_NONE, MAP_FIXED|MAP_NORESERVE) -> %s", (unsigned long long)mm[i], p == MAP_FAILED ? errno_name(e) : "mapped");
    if (p != MAP_FAILED) munmap(p, vm_page_size);
  }
  scan_rle("A05", "low-scan", 0, 6 * GiB, vm_page_size, vm_page_size);
}

typedef struct { const char *name; mach_vm_address_t addr; } fixed_pt;

static void a06_fixed(void) {
  static const fixed_pt pts[] = {
      {"0x10000 (Windows lowest alloc)", 0x10000ULL},
      {"0x7ffe0000 (KUSER_SHARED_DATA)", 0x7ffe0000ULL},
      {"0x140000000 (x64 image base)", 0x140000000ULL},
      {"0x7ffffffe0000 (Win64 top)", 0x7ffffffe0000ULL},
      {"0x7fffffff0000 (Win64 top+64K)", 0x7fffffff0000ULL},
  };
  static const mach_vm_size_t sizes[] = {16 * KiB, 1 * MiB};
  for (unsigned i = 0; i < sizeof pts / sizeof *pts; i++) {
    for (unsigned s = 0; s < 2; s++) {
      mach_vm_address_t a = pts[i].addr;
      mach_vm_size_t sz = sizes[s];
      bool crosses_top = (a + sz) > 0x800000000000ULL;
      bool freeb = dm_range_is_free(a, sz);
      kern_return_t kr_alloc = try_fixed(a, sz);
      char line[400];
      int off = snprintf(line, sizeof line, "%s size=%llu KiB free=%d mach_vm_allocate(FIXED)=%s", pts[i].name,
                         (unsigned long long)(sz / KiB), freeb, kr_name(kr_alloc));
      if (freeb) {
        for (int k = 0; k < 2; k++) {
          int prot = k ? (PROT_READ | PROT_WRITE) : PROT_NONE;
          void *p = mmap((void *)a, sz, prot, MAP_PRIVATE | MAP_ANON | MAP_FIXED | (k ? 0 : MAP_NORESERVE), -1, 0);
          int e = errno;
          if (p == MAP_FAILED) off += snprintf(line + off, sizeof line - off, " mmap(%s,FIXED)=%s", k ? "RW" : "NONE", errno_name(e));
          else {
            int ok = 1;
            if (k) { volatile uint64_t *q = (volatile uint64_t *)p; *q = 0x1122334455667788ULL; ok = (*q == 0x1122334455667788ULL); }
            off += snprintf(line + off, sizeof line - off, " mmap(%s,FIXED)=ok%s%s", k ? "RW" : "NONE", (p == (void *)a) ? "" : "(MOVED)", k ? (ok ? "+rw" : "+RWFAIL") : "");
            munmap(p, sz);
          }
        }
        mach_vm_address_t ma = a;
        kern_return_t kr = mach_vm_map(mach_task_self(), &ma, sz, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
        off += snprintf(line + off, sizeof line - off, " mach_vm_map(FIXED)=%s", kr_name(kr));
        if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), ma, sz);
      }
      if (crosses_top) off += snprintf(line + off, sizeof line - off, " [range crosses 2^47]");
      (void)off;
      bool mappable = (kr_alloc == KERN_SUCCESS);
      if (s == 0) S.fixed_ok[i] = mappable;
      if (mappable) PASS("A06", "%s", line); else FAIL("A06", "%s", line);
    }
  }
}

static void a07_hints(void) {
  static const mach_vm_address_t hints[] = {0x10000ULL, 0x7ffe0000ULL, 0x140000000ULL, 0x140001000ULL, 0x200000000ULL, 0x1000000000ULL,
                                            0x600000000000ULL, 0x7ffffffe0000ULL, 0x7fffffff0000ULL, 0x800000000000ULL,
                                            0x1000000000000ULL};
  for (unsigned i = 0; i < sizeof hints / sizeof *hints; i++) {
    void *p = mmap((void *)hints[i], 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    int e = errno;
    if (p == MAP_FAILED) INFO("A07", "mmap(hint=0x%llx) failed errno=%s", (unsigned long long)hints[i], errno_name(e));
    else {
      INFO("A07", "mmap(hint=0x%llx) -> %p %s", (unsigned long long)hints[i], p, (p == (void *)hints[i]) ? "(hint honoured)" : "(hint NOT honoured)");
      munmap(p, 16 * KiB);
    }
    mach_vm_address_t a = hints[i];
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, 16 * KiB, VM_FLAGS_ANYWHERE);
    if (kr == KERN_SUCCESS) { INFO("A07", "mach_vm_allocate(ANYWHERE, start=0x%llx) -> 0x%llx (the start value is ignored by ANYWHERE unless it is a lower bound)", (unsigned long long)hints[i], (unsigned long long)a); mach_vm_deallocate(mach_task_self(), a, 16 * KiB); }
    else INFO("A07", "mach_vm_allocate(ANYWHERE, start=0x%llx) -> %s", (unsigned long long)hints[i], kr_name(kr));
  }
  void *live = mmap(NULL, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  void *again = mmap(live, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("A07", "mmap(hint=occupied %p) -> %p (%s)", live, again, again == live ? "SAME ADDRESS (replaced?)" : "different address");
  if (again != MAP_FAILED && again != live) munmap(again, 16 * KiB);
  munmap(live, 16 * KiB);
  void *q[5];
  char line[300]; int off = 0;
  for (int i = 0; i < 5; i++) { q[i] = mmap(NULL, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); off += snprintf(line + off, sizeof line - off, " %p", q[i]); }
  INFO("A07", "five successive mmap(NULL,16K):%s", line);
  for (int i = 0; i < 5; i++) munmap(q[i], 16 * KiB);
  /* VM_FLAGS_RANDOM_ADDR */
  {
    mach_vm_address_t r[3]; char l2[200]; int o2 = 0;
    for (int i = 0; i < 3; i++) { r[i] = 0; kern_return_t kr = mach_vm_allocate(mach_task_self(), &r[i], 16 * KiB, VM_FLAGS_ANYWHERE | VM_FLAGS_RANDOM_ADDR); o2 += snprintf(l2 + o2, sizeof l2 - o2, " %s:0x%llx", kr_name(kr), (unsigned long long)r[i]); }
    INFO("A07", "three mach_vm_allocate(ANYWHERE|RANDOM_ADDR):%s", l2);
    for (int i = 0; i < 3; i++) if (r[i]) mach_vm_deallocate(mach_task_self(), r[i], 16 * KiB);
  }
  mach_vm_address_t ma = 0;
  kern_return_t kr = mach_vm_map(mach_task_self(), &ma, 1 * MiB, (mach_vm_offset_t)(64 * KiB - 1), VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
  if (kr == KERN_SUCCESS) { CHECK((ma & (64 * KiB - 1)) == 0, "A07", "mach_vm_map(ANYWHERE, mask=0xffff) -> 0x%llx (64 KiB aligned: %d)", (unsigned long long)ma, (int)((ma & (64 * KiB - 1)) == 0)); mach_vm_deallocate(mach_task_self(), ma, 1 * MiB); }
  else FAIL("A07", "mach_vm_map(ANYWHERE, mask=0xffff) -> %s", kr_name(kr));
  ma = 0;
  kr = mach_vm_map(mach_task_self(), &ma, 1 * MiB, (mach_vm_offset_t)(4 * GiB - 1), VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
  if (kr == KERN_SUCCESS) { INFO("A07", "mach_vm_map(ANYWHERE, mask=4GiB-1) -> 0x%llx (4 GiB aligned: %d)", (unsigned long long)ma, (int)((ma & (4 * GiB - 1)) == 0)); mach_vm_deallocate(mach_task_self(), ma, 1 * MiB); }
  else INFO("A07", "mach_vm_map(ANYWHERE, mask=4GiB-1) -> %s", kr_name(kr));
}

static void *try_reserve(mach_vm_size_t size, int prot, int extra_flags, int *err) {
  void *p = mmap(NULL, size, prot, MAP_PRIVATE | MAP_ANON | extra_flags, -1, 0);
  *err = (p == MAP_FAILED) ? errno : 0;
  return p;
}

static void a08_reserve(void) {
  static const struct { const char *n; mach_vm_size_t s; } sz[] = {
      {"128 GiB", 128 * GiB}, {"1 TiB", 1 * TiB}, {"100 TiB", 100 * TiB}, {"120 TiB", 120 * TiB}, {"127 TiB", 127 * TiB}};
  for (unsigned i = 0; i < sizeof sz / sizeof *sz; i++) {
    int e;
    void *p = try_reserve(sz[i].s, PROT_NONE, MAP_NORESERVE, &e);
    if (p != MAP_FAILED) { PASS("A08", "mmap(NULL, %s, PROT_NONE, MAP_NORESERVE) -> %p", sz[i].n, p); munmap(p, sz[i].s); }
    else FAIL("A08", "mmap(NULL, %s, PROT_NONE, MAP_NORESERVE) failed errno=%s", sz[i].n, errno_name(e));
  }
  int e; void *p;
  p = try_reserve(1 * TiB, PROT_NONE, 0, &e);
  if (p != MAP_FAILED) { INFO("A08", "mmap(NULL, 1 TiB, PROT_NONE) without NORESERVE -> %p", p); munmap(p, 1 * TiB); } else INFO("A08", "mmap(NULL, 1 TiB, PROT_NONE) without NORESERVE failed errno=%s", errno_name(e));
  p = try_reserve(1 * TiB, PROT_READ | PROT_WRITE, 0, &e);
  if (p != MAP_FAILED) { INFO("A08", "mmap(NULL, 1 TiB, RW) untouched -> %p", p); munmap(p, 1 * TiB); } else INFO("A08", "mmap(NULL, 1 TiB, RW) failed errno=%s", errno_name(e));
  {
    mach_vm_address_t a = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, 1 * TiB, VM_FLAGS_ANYWHERE);
    INFO("A08", "mach_vm_allocate(ANYWHERE, 1 TiB) -> %s addr=0x%llx", kr_name(kr), (unsigned long long)a);
    if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 1 * TiB);
    a = 0;
    kr = mach_vm_map(mach_task_self(), &a, 100 * TiB, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT);
    INFO("A08", "mach_vm_map(ANYWHERE, 100 TiB, VM_PROT_NONE) -> %s addr=0x%llx", kr_name(kr), (unsigned long long)a);
    if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 100 * TiB);
  }
  mach_vm_size_t lo = 1 * GiB, hi = 128 * TiB;
  while (hi - lo > 1 * GiB) {
    mach_vm_size_t mid = ((lo + hi) / 2) & ~(1 * GiB - 1);
    if (mid <= lo) break;
    p = try_reserve(mid, PROT_NONE, MAP_NORESERVE, &e);
    if (p != MAP_FAILED) { munmap(p, mid); lo = mid; } else hi = mid;
  }
  p = try_reserve(lo, PROT_NONE, MAP_NORESERVE, &e);
  S.largest_resv_gib = lo / GiB;
  INFO("A08", "largest single PROT_NONE|MAP_NORESERVE reservation (1 GiB granularity) = %llu GiB (%.3f TiB) at %p", (unsigned long long)(lo / GiB), lo / (double)TiB, p);
  if (p != MAP_FAILED) {
    char *base = (char *)p;
    void *c = mmap(base + 1 * GiB, 64 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    int ce = errno;
    if (c == (void *)(base + 1 * GiB)) {
      memset(c, 0xAB, 64 * MiB);
      int r1 = mprotect(c, 64 * MiB, PROT_NONE);
      int r2 = mprotect(c, 64 * MiB, PROT_READ | PROT_WRITE);
      int ok = (((unsigned char *)c)[12345] == 0xAB);
      int r3 = madvise(c, 64 * MiB, MADV_FREE);
      CHECK(r1 == 0 && r2 == 0 && ok, "A08", "carve 64 MiB RW inside the reservation: touch, mprotect NONE and back keeps contents (madvise(MADV_FREE)=%d)", r3);
      int r4 = munmap(base + 1 * GiB + 16 * MiB, 16 * MiB);
      INFO("A08", "munmap of a 16 MiB hole inside the reservation -> %d (%s)", r4, r4 ? errno_name(errno) : "ok");
    } else FAIL("A08", "MAP_FIXED carve inside reservation failed errno=%s", errno_name(ce));
    munmap(p, lo);
  }
}

static void a09_47bit(void) {
  static const struct { const char *n; mach_vm_address_t a; } pts[] = {
      {"MACH_VM_MAX_ADDRESS-16K", 0x7FFFFDFFC000ULL}, {"MACH_VM_MAX_ADDRESS", 0x7FFFFE000000ULL},
      {"MACH_VM_MAX_ADDRESS+16K", 0x7FFFFE004000ULL}, {"2^47-128K (0x7ffffffe0000)", 0x7FFFFFFE0000ULL},
      {"2^47-64K (0x7fffffff0000)", 0x7FFFFFFF0000ULL}, {"2^47-16K", 0x7FFFFFFFC000ULL},
      {"2^47", 0x800000000000ULL}, {"2^47+16K", 0x800000004000ULL}, {"2^48-16K", 0xFFFFFFFFC000ULL},
      {"2^52", 0x10000000000000ULL}};
  for (unsigned i = 0; i < sizeof pts / sizeof *pts; i++) {
    kern_return_t kr = try_fixed(pts[i].a, 16 * KiB);
    INFO("A09", "mach_vm_allocate(FIXED, %s = 0x%llx) -> %s", pts[i].n, (unsigned long long)pts[i].a, kr_name(kr));
  }
  scan_rle("A09", "top-scan", 0x7FFFFDF00000ULL, 0x800000000000ULL, 16 * KiB, 16 * KiB);
  mach_vm_address_t hi = 0;
  for (mach_vm_address_t a = 0x7FFFFFFFC000ULL; a >= 0x7FFFFD000000ULL; a -= 16 * KiB) {
    if (try_fixed(a, 16 * KiB) == KERN_SUCCESS) { hi = a; break; }
  }
  INFO("A09", "highest allocatable 16 KiB page starts at 0x%llx, so the highest mappable byte is 0x%llx", (unsigned long long)hi, (unsigned long long)(hi + 16 * KiB - 1));
  CHECK(hi >= 0x7FFFFFFE0000ULL, "A09", "Windows x64 top-of-user-space 0x7ffffffe0000 is mappable (needs highest page >= 0x7ffffffe0000, got 0x%llx)", (unsigned long long)hi);
}

static void a10_carveout(void) {
  scan_rle("A10", "gpu-carveout-1GiB-steps", 32 * GiB, 512 * GiB, 1 * GiB, 16 * KiB);
}

static void a11_map32(void) {
  void *p = mmap(NULL, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_32BIT, -1, 0);
  int e = errno;
  if (p == MAP_FAILED) FAIL("A11", "mmap(NULL, MAP_32BIT) failed errno=%s", errno_name(e));
  else { CHECK((uintptr_t)p < 4 * GiB, "A11", "mmap(NULL, MAP_32BIT) -> %p (below 4 GiB: %d)", p, (int)((uintptr_t)p < 4 * GiB)); munmap(p, 16 * KiB); }
  mach_vm_address_t a = 0;
  kern_return_t kr = mach_vm_map(mach_task_self(), &a, 16 * KiB, 0, VM_FLAGS_ANYWHERE, MACH_PORT_NULL, 0, FALSE, VM_PROT_DEFAULT, VM_PROT_ALL, VM_INHERIT_DEFAULT);
  INFO("A11", "mach_vm_map(ANYWHERE, start=0) -> %s 0x%llx", kr_name(kr), (unsigned long long)a);
  if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 16 * KiB);
  a = 0;
  kr = mach_vm_allocate(mach_task_self(), &a, 16 * KiB, VM_FLAGS_ANYWHERE | VM_FLAGS_4GB_CHUNK);
  INFO("A11", "mach_vm_allocate(ANYWHERE|4GB_CHUNK) -> %s 0x%llx", kr_name(kr), (unsigned long long)a);
  if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 16 * KiB);
}

static void a12_overwrite(void) {
  char *p = mmap(NULL, 64 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 0x5A, 64 * KiB);
  char *q = mmap(p + 16 * KiB, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  CHECK(q == p + 16 * KiB && p[16 * KiB + 5] == 0 && p[5] == 0x5A, "A12", "mmap(MAP_FIXED) over a live mapping replaces it silently and zero-fills (fill byte now %d)", p[16 * KiB + 5]);
  mach_vm_address_t a = (mach_vm_address_t)(p + 32 * KiB);
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, 16 * KiB, VM_FLAGS_FIXED);
  CHECK(kr != KERN_SUCCESS, "A12", "mach_vm_allocate(FIXED) over a live mapping is refused: %s", kr_name(kr));
  a = (mach_vm_address_t)(p + 32 * KiB);
  kr = mach_vm_allocate(mach_task_self(), &a, 16 * KiB, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
  CHECK(kr == KERN_SUCCESS && p[32 * KiB + 5] == 0, "A12", "mach_vm_allocate(FIXED|OVERWRITE) replaces: %s", kr_name(kr));
  int r = munmap(p + 16 * KiB, 16 * KiB);
  INFO("A12", "munmap middle 16 KiB of 64 KiB mapping -> %d; regions now: first=%d last-prot=%d", r, dm_prot_at((mach_vm_address_t)p, NULL, NULL, NULL), dm_prot_at((mach_vm_address_t)(p + 48 * KiB), NULL, NULL, NULL));
  munmap(p, 64 * KiB);
}

/*
 * A13: can the guest range of a 64-bit Wine process, 0x10000 .. 0x7fffffff0000,
 * be reserved?  Walk the free holes of the address space inside that range,
 * find which part of each hole mach_vm_allocate accepts, then actually reserve
 * everything that is reservable with PROT_NONE|MAP_NORESERVE and release it.
 */
static mach_vm_address_t lowest_ok_in(mach_vm_address_t lo, mach_vm_address_t hi) {
  /* smallest address a in [lo, hi) with a 16 KiB probe succeeding, 16 KiB granularity; 0 if none */
  if (try_fixed(lo, 16 * KiB) == KERN_SUCCESS) return lo;
  if (hi - 16 * KiB <= lo || try_fixed(hi - 16 * KiB, 16 * KiB) != KERN_SUCCESS) return 0;
  mach_vm_address_t l = lo, h = hi - 16 * KiB; /* l fails, h succeeds */
  while (h - l > 16 * KiB) {
    mach_vm_address_t m = l + (((h - l) / 2) & ~(16 * KiB - 1));
    if (m == l) break;
    if (try_fixed(m, 16 * KiB) == KERN_SUCCESS) h = m; else l = m;
  }
  return h;
}
static mach_vm_address_t highest_end_ok_in(mach_vm_address_t lo, mach_vm_address_t hi) {
  /* largest e <= hi such that [e-16K, e) probe succeeds; 0 if none */
  if (try_fixed(hi - 16 * KiB, 16 * KiB) == KERN_SUCCESS) return hi;
  if (try_fixed(lo, 16 * KiB) != KERN_SUCCESS) return 0;
  mach_vm_address_t l = lo, h = hi - 16 * KiB; /* l succeeds, h fails */
  while (h - l > 16 * KiB) {
    mach_vm_address_t m = l + (((h - l) / 2) & ~(16 * KiB - 1));
    if (m == l) break;
    if (try_fixed(m, 16 * KiB) == KERN_SUCCESS) l = m; else h = m;
  }
  return l + 16 * KiB;
}

static void a13_guest_range(void) {
  const mach_vm_address_t LO = 0x10000ULL, HI = 0x7fffffff0000ULL;
  struct { mach_vm_address_t s, e; } resv[256];
  int nresv = 0;
  mach_vm_size_t occupied = 0, blocked_low = 0, blocked_high = 0, blocked_other = 0, reservable = 0;
  mach_vm_address_t cursor = LO;
  /* iterate regions that intersect the guest range */
  while (cursor < HI) {
    dm_region r = dm_region_at_or_after(cursor);
    mach_vm_address_t hole_end = (!r.valid || r.start >= HI) ? HI : r.start;
    if (hole_end > cursor) {
      mach_vm_address_t hs = cursor, he = hole_end;
      mach_vm_address_t rs = lowest_ok_in(hs, he);
      if (!rs) {
        /* nothing in this hole is allocatable */
        if (he <= S.min_address) blocked_low += he - hs; else if (hs >= S.max_address) blocked_high += he - hs; else blocked_other += he - hs;
      } else {
        mach_vm_address_t re = highest_end_ok_in(rs, he);
        if (rs > hs) { if (rs <= S.min_address) blocked_low += rs - hs; else blocked_other += rs - hs; }
        if (re < he) { if (re >= S.max_address) blocked_high += he - re; else blocked_other += he - re; }
        if (re > rs && nresv < 256) {
          resv[nresv].s = rs; resv[nresv].e = re; nresv++;
          reservable += re - rs;
        }
      }
    }
    if (!r.valid || r.start >= HI) break;
    mach_vm_address_t re2 = r.start + r.size;
    mach_vm_address_t os = r.start < LO ? LO : r.start, oe = re2 > HI ? HI : re2;
    if (oe > os) occupied += oe - os;
    cursor = re2;
  }
  S.guest_total_mib = (HI - LO) / MiB;
  S.guest_reservable_mib = reservable / MiB;
  S.guest_occupied_mib = occupied / MiB;
  S.guest_blocked_low_mib = blocked_low / MiB;
  S.guest_blocked_high_mib = blocked_high / MiB;
  S.guest_blocked_other_mib = blocked_other / MiB;
  INFO("A13", "guest range 0x%llx-0x%llx = %.3f TiB (%.0f GiB)", (unsigned long long)LO, (unsigned long long)HI, (HI - LO) / (double)TiB, (HI - LO) / (double)GiB);
  INFO("A13", "free and allocatable now: %.3f TiB in %d holes; occupied by existing mappings: %.3f GiB; unmappable below the slid __PAGEZERO: %.3f GiB; unmappable above max_address: %.3f MiB; unmappable elsewhere (reserved by the kernel): %.3f GiB",
       reservable / (double)TiB, nresv, occupied / (double)GiB, blocked_low / (double)GiB, blocked_high / (double)MiB, blocked_other / (double)GiB);
  for (int i = 0; i < nresv && i < 24; i++)
    INFO("A13", "reservable hole #%d: 0x%llx-0x%llx (%.3f GiB)", i, (unsigned long long)resv[i].s, (unsigned long long)resv[i].e, (resv[i].e - resv[i].s) / (double)GiB);
  /* actually reserve them all */
  int okc = 0, failc = 0;
  for (int i = 0; i < nresv; i++) {
    void *p = mmap((void *)resv[i].s, resv[i].e - resv[i].s, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    if (p == (void *)resv[i].s) okc++; else failc++;
  }
  CHECK(failc == 0 && okc > 0, "A13", "reserved %d of %d free holes with mmap(MAP_FIXED|PROT_NONE|MAP_NORESERVE), %d failed", okc, nresv, failc);
  /* verify the reservation is contiguous where holes are adjacent, then carve RW and the PE-image style addresses */
  {
    mach_vm_address_t tests[] = {0x140000000ULL, 0x7ffffd000000ULL};
    for (unsigned i = 0; i < 2; i++) {
      void *c = mmap((void *)tests[i], 64 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      INFO("A13", "after reserving everything, carve RW 64 KiB at 0x%llx -> %s", (unsigned long long)tests[i], c == (void *)tests[i] ? "ok" : errno_name(errno));
    }
  }
  for (int i = 0; i < nresv; i++) munmap((void *)resv[i].s, resv[i].e - resv[i].s);
  INFO("A13", "reservation released");
}


/*
 * A14: the big PROT_NONE regions with VM_MEMORY_GUARD (tag 31) that the kernel or dyld
 * installs at exec (one of 384 GiB at 0x1000000000 on this machine).  Can a process reclaim
 * their address space?  Runs last because it is destructive for this process.
 */
static void a14_guard_regions(void) {
  mach_vm_address_t addr = 0;
  int tried = 0;
  for (;;) {
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (mach_vm_region_recurse(mach_task_self(), &a, &sz, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) break;
    addr = a + sz;
    if (info.is_submap || info.user_tag != 31 || info.max_protection != 0 || sz < 1 * GiB) continue;
    tried++;
    INFO("A14", "guard region 0x%llx-0x%llx (%.1f GiB) prot=%d max=%d tag=%u share=%d", (unsigned long long)a, (unsigned long long)(a + sz), sz / (double)GiB,
         info.protection, info.max_protection, info.user_tag, info.share_mode);
    mach_vm_address_t t = a;
    kern_return_t kr = mach_vm_protect(mach_task_self(), t, 16 * KiB, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    INFO("A14", "mach_vm_protect(RW) on the first 16 KiB of the guard region -> %s", kr_name(kr));
    void *m = mmap((void *)t, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    int e = errno;
    INFO("A14", "mmap(MAP_FIXED, RW, 16 KiB) over the start of the guard region -> %s", m == MAP_FAILED ? errno_name(e) : "replaced");
    if (m != MAP_FAILED) {
      volatile uint8_t *q = (volatile uint8_t *)m; *q = 0x77;
      INFO("A14", "the replaced page is usable: wrote 0x77 read 0x%02x", *q);
      munmap(m, 16 * KiB);
    }
    kr = mach_vm_deallocate(mach_task_self(), t + 16 * KiB, 16 * KiB);
    INFO("A14", "mach_vm_deallocate of 16 KiB inside the guard region -> %s", kr_name(kr));
    if (kr == KERN_SUCCESS) {
      void *h = mmap((void *)(t + 16 * KiB), 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      INFO("A14", "mmap(MAP_FIXED, RW) into the 16 KiB hole punched in the guard region -> %s", h == MAP_FAILED ? errno_name(errno) : "ok (usable memory inside a former guard region)");
      if (h != MAP_FAILED) munmap(h, 16 * KiB);
    }
    /* take the whole remaining region away with mach_vm_deallocate, then use the space */
    kr = mach_vm_deallocate(mach_task_self(), t, sz);
    INFO("A14", "mach_vm_deallocate of the whole %.0f GiB guard range -> %s", sz / (double)GiB, kr_name(kr));
    if (kr == KERN_SUCCESS) {
      bool free_now = dm_range_is_free(t, sz);
      void *big = mmap((void *)t, sz, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
      int e2 = errno;
      INFO("A14", "range free after deallocate: %d; then mmap(MAP_FIXED, PROT_NONE|NORESERVE) over the whole range -> %s", free_now, big == MAP_FAILED ? errno_name(e2) : "ok (address space reclaimed)");
      if (big != MAP_FAILED) {
        void *c = mmap((void *)(t + sz / 2), 64 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        INFO("A14", "carve RW 64 KiB in the middle of the reclaimed range -> %s", c == MAP_FAILED ? errno_name(errno) : "ok");
        munmap(big, sz);
      }
    }
  }
  if (!tried) INFO("A14", "no PROT_NONE guard region of at least 1 GiB with tag 31 found");
}

int main(int argc, char **argv) {
  dm_arm_watchdog(240);
  bool full = false, brief = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--map")) full = true;
    if (!strcmp(argv[i], "--brief")) brief = true;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (brief) {
    task_vm_info_data_t ti; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&ti, &c);
    printf("BRIEF image_base=0x%llx min_address=0x%llx max_address=0x%llx page_size=%d\n", (unsigned long long)(uintptr_t)_NSGetMachExecuteHeader(),
           (unsigned long long)ti.min_address, (unsigned long long)ti.max_address, ti.page_size);
    return 0;
  }
  const char *base = strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  printf("# a_addrspace variant=%s arch=%s\n", base, arch_name());
  a01_env();
  a01b_codesign();
  a02_macho();
  a03_taskinfo();
  a04_regions(full);
  a05_low();
  a06_fixed();
  a07_hints();
  a08_reserve();
  a09_47bit();
  a10_carveout();
  a11_map32();
  a12_overwrite();
  a13_guest_range();
  a14_guard_regions();
  printf("# summary pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  printf("SUMMARY variant=%s arch=%s image_base=0x%llx min_address=0x%llx max_address=0x%llx largest_reservation_gib=%llu "
         "fixed_0x10000=%d fixed_0x7ffe0000=%d fixed_0x140000000=%d fixed_0x7ffffffe0000=%d fixed_0x7fffffff0000=%d "
         "guest_total_mib=%llu guest_reservable_mib=%llu guest_occupied_mib=%llu guest_blocked_low_mib=%llu guest_blocked_other_mib=%llu guest_blocked_high_mib=%llu\n",
         base, arch_name(), (unsigned long long)S.exe_base, (unsigned long long)S.min_address, (unsigned long long)S.max_address, S.largest_resv_gib,
         S.fixed_ok[0], S.fixed_ok[1], S.fixed_ok[2], S.fixed_ok[3], S.fixed_ok[4], S.guest_total_mib, S.guest_reservable_mib, S.guest_occupied_mib,
         S.guest_blocked_low_mib, S.guest_blocked_other_mib, S.guest_blocked_high_mib);
  return 0;
}
