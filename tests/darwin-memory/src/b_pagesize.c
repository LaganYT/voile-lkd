/*
 * b_pagesize.c - fact group B: page size and 4 KiB-granular requests.
 *
 * Runs the same battery in three situations (see run_b.sh):
 *   - native arm64 process                      (16 KiB pages)
 *   - x86_64 process under Rosetta              (default)
 *   - x86_64 process under Rosetta launched with posix_spawnattr_set_4k_page_size_np
 *
 * Every case works on a private arena that is rebuilt before each case, with
 * 256 KiB of slack mapped on both sides, so a call that silently rounds to a
 * bigger range cannot touch anything that matters.  The state of each 4 KiB
 * chunk is printed as one letter:
 *   U unmapped   N mapped but no access   R read-only   W read/write
 *   lower case = the chunk's marker byte was lost (new zero-filled page)
 *
 * usage: b_pagesize [--bench]
 */
#include "common.h"
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define GiB (1ULL << 30)
#define NCH 8 /* chunks (4 KiB) printed per case */

static uint8_t *g_blk, *g_arena;
static const size_t BLK = 1 * MiB;
static char g_tmpfile[512];

/* ---- fault-guarded probes ------------------------------------------------
 * A probe runs in a forked child so that a fault just ends the child.  siglongjmp out of a
 * SIGSEGV handler is not usable here: under Rosetta the seventh abandoned signal frame hangs
 * the process (measured, see the report), and a fork-per-probe is robust on both architectures.
 */
#include <sys/wait.h>
static void child_fault(int sig) { (void)sig; _exit(99); }
static void install_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = child_fault;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
}
static int probe_child(const volatile uint8_t *p, int do_write, uint8_t wval, uint8_t *out) {
  int fds[2];
  if (pipe(fds)) return 0;
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
  if (pid == 0) {
    close(fds[0]);
    uint8_t v = 0;
    if (do_write) { *(volatile uint8_t *)p = wval; v = wval; } else v = *p;
    (void)!write(fds[1], &v, 1);
    _exit(0);
  }
  close(fds[1]);
  uint8_t got = 0;
  ssize_t n = read(fds[0], &got, 1);
  close(fds[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (n == 1 && WIFEXITED(st) && WEXITSTATUS(st) == 0) { if (out) *out = got; return 1; }
  return 0;
}
static int try_read(const volatile uint8_t *p, uint8_t *out) { return probe_child(p, 0, 0, out); }
static int try_write(volatile uint8_t *p, uint8_t v) { return probe_child(p, 1, v, NULL); }

static uint8_t marker(int chunk) { return (uint8_t)(chunk % 250 + 1); }

static void arena_reset(void) {
  void *r = mmap(g_blk, BLK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  if (r != g_blk) { printf("FATAL arena_reset mmap failed errno=%d\n", errno); exit(3); }
  for (int i = 0; i < (int)(BLK / 4096); i++) g_blk[(size_t)i * 4096] = marker((int)((g_blk + (size_t)i * 4096 - g_arena) / 4096) & 0xffff);
}

static char chunk_state(uint8_t *p, int idx) {
  uint8_t v = 0;
  dm_region r = dm_region_at_or_after((mach_vm_address_t)p);
  bool mapped = r.valid && r.start <= (mach_vm_address_t)p;
  if (!try_read(p, &v)) return mapped ? 'N' : 'U';
  bool intact = (v == marker(idx));
  bool w = try_write(p, v);
  char c = w ? 'W' : 'R';
  return intact ? c : (char)(c + 32);
}
static void states(char *out, int n) {
  for (int i = 0; i < n; i++) out[i] = chunk_state(g_arena + (size_t)i * 4096, i);
  out[n] = 0;
}

/* ---- B01: the page-size numbers ----------------------------------------- */
static void b01_numbers(void) {
  size_t l;
  int v = 0;
  INFO("B01", "arch=%s translated=%d", arch_name(), is_translated());
  INFO("B01", "vm_page_size=%lu vm_page_shift=%d vm_page_mask=0x%lx vm_kernel_page_size=%lu vm_kernel_page_shift=%d", (unsigned long)vm_page_size, vm_page_shift,
       (unsigned long)vm_page_mask, (unsigned long)vm_kernel_page_size, vm_kernel_page_shift);
  l = sizeof v; if (!sysctlbyname("hw.pagesize", &v, &l, NULL, 0)) INFO("B01", "sysctl hw.pagesize=%d", v);
  uint64_t v64 = 0; l = sizeof v64;
  if (!sysctlbyname("hw.pagesize", &v64, &l, NULL, 0)) INFO("B01", "sysctl hw.pagesize (64-bit read, %zu bytes)=%llu", l, (unsigned long long)v64);
  l = sizeof v; v = 0; if (!sysctlbyname("vm.pagesize", &v, &l, NULL, 0)) INFO("B01", "sysctl vm.pagesize=%d", v);
  INFO("B01", "getpagesize()=%d sysconf(_SC_PAGESIZE)=%ld", getpagesize(), sysconf(_SC_PAGESIZE));
  vm_size_t hps = 0;
  host_page_size(mach_host_self(), &hps);
  INFO("B01", "host_page_size()=%lu", (unsigned long)hps);
  task_vm_info_data_t ti; mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&ti, &c) == KERN_SUCCESS) INFO("B01", "TASK_VM_INFO.page_size=%d", ti.page_size);
#ifdef PAGE_SIZE
  INFO("B01", "PAGE_SIZE macro expands to %lu", (unsigned long)(PAGE_SIZE));
#endif
  INFO("B01", "compile-time: PAGE_SHIFT=%d (macro)", (int)(PAGE_SHIFT));
  vm_size_t dummy = 0;
  (void)dummy;
}

/* ---- B02: how an mmap of 4 KiB is sized and spaced -------------------------- */
static void b02_sizes(void) {
  uint8_t *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  uint8_t *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  uint8_t *c = mmap(NULL, 1, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("B02", "mmap(NULL,4096) -> %p, again -> %p, mmap(NULL,1) -> %p; spacing a->b = %lld bytes", (void *)a, (void *)b, (void *)c, (long long)(b - a));
  dm_region r = dm_region_at_or_after((mach_vm_address_t)a);
  INFO("B02", "region around the 4096-byte mapping: 0x%llx size 0x%llx", (unsigned long long)r.start, (unsigned long long)r.size);
  int ok_tail = 0;
  uint8_t v;
  if (a != MAP_FAILED && try_read(a + 4096, &v)) ok_tail = 1;
  int ok_far = (a != MAP_FAILED && try_write(a + vm_page_size - 1, 1));
  INFO("B02", "byte just past the requested 4096 is readable: %d; last byte of the first host page writable: %d (1 = silent rounding up)", ok_tail, ok_far);
  void *z = mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("B02", "mmap(NULL, 0) -> %s", z == MAP_FAILED ? errno_name(errno) : "mapped");
  mach_vm_address_t ma = 0;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &ma, 4096, VM_FLAGS_ANYWHERE);
  dm_region r2 = dm_region_at_or_after(ma);
  INFO("B02", "mach_vm_allocate(ANYWHERE, 4096) -> %s 0x%llx; region size 0x%llx", kr_name(kr), (unsigned long long)ma, (unsigned long long)r2.size);
  if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), ma, 4096);
  /* alignment of NULL-hint mappings */
  int unaligned16 = 0;
  uint8_t *m[16];
  for (int i = 0; i < 16; i++) { m[i] = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); if (m[i] != MAP_FAILED && ((uintptr_t)m[i] & 16383)) unaligned16++; }
  INFO("B02", "16 successive mmap(NULL,4096): %d of 16 are not 16 KiB aligned; last spacing %lld", unaligned16, (long long)(m[15] - m[14]));
  for (int i = 0; i < 16; i++) if (m[i] != MAP_FAILED) munmap(m[i], 4096);
  if (a != MAP_FAILED) munmap(a, 4096);
  if (b != MAP_FAILED) munmap(b, 4096);
  if (c != MAP_FAILED) munmap(c, 1);
}

/* ---- helper to report one case ---------------------------------------------- */
static void report(const char *id, const char *what, long ret, int err) {
  char st[NCH + 1];
  states(st, NCH);
  INFO(id, "%-58s ret=%ld errno=%s | chunks 0..%d: %s", what, ret, ret == -1 ? errno_name(err) : "-", NCH - 1, st);
}
static void report_k(const char *id, const char *what, kern_return_t kr) {
  char st[NCH + 1];
  states(st, NCH);
  INFO(id, "%-58s kr=%s | chunks 0..%d: %s", what, kr_name(kr), NCH - 1, st);
}
#define TRACE(what) do { if (getenv("DM_TRACE")) { fprintf(stderr, "trace: %s\n", what); } } while (0)
#define CASEK(id, what, expr)                                                  \
  do {                                                                         \
    TRACE(what);                                                               \
    arena_reset();                                                             \
    kern_return_t _k = (expr);                                                 \
    report_k(id, what, _k);                                                    \
  } while (0)
#define CASE(id, what, expr)                                                   \
  do {                                                                         \
    TRACE(what);                                                               \
    arena_reset();                                                             \
    TRACE("  reset done");                                                     \
    errno = 0;                                                                 \
    long _r = (long)(expr);                                                    \
    int _e = errno;                                                            \
    TRACE("  call done");                                                      \
    report(id, what, _r, _e);                                                  \
  } while (0)

static void b03_munmap(void) {
  CASE("B03", "munmap(arena+4096, 4096)", munmap(g_arena + 4096, 4096));
  CASE("B03", "munmap(arena+12288, 4096)  [last 4K of the 16K page]", munmap(g_arena + 12288, 4096));
  CASE("B03", "munmap(arena, 4096)", munmap(g_arena, 4096));
  CASE("B03", "munmap(arena+4096, 12288)", munmap(g_arena + 4096, 12288));
  CASE("B03", "munmap(arena+8192, 16384) [starts mid host page]", munmap(g_arena + 8192, 16384));
  CASE("B03", "munmap(arena+1, 4096)      [not 4K aligned]", munmap(g_arena + 1, 4096));
  CASE("B03", "munmap(arena, 1)", munmap(g_arena, 1));
  CASE("B03", "munmap(arena+16384, 16384) [aligned, control]", munmap(g_arena + 16384, 16384));
  CASEK("B03", "mach_vm_deallocate(arena+4096, 4096)", mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(g_arena + 4096), 4096));
  CASEK("B03", "mach_vm_deallocate(arena+4096, 12288)", mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(g_arena + 4096), 12288));
}

static void b04_mprotect(void) {
  CASE("B04", "mprotect(arena+4096, 4096, NONE)", mprotect(g_arena + 4096, 4096, PROT_NONE));
  CASE("B04", "mprotect(arena, 4096, NONE)", mprotect(g_arena, 4096, PROT_NONE));
  CASE("B04", "mprotect(arena+12288, 4096, NONE)", mprotect(g_arena + 12288, 4096, PROT_NONE));
  CASE("B04", "mprotect(arena+4096, 12288, NONE)", mprotect(g_arena + 4096, 12288, PROT_NONE));
  CASE("B04", "mprotect(arena+8192, 8192, READ) [spans 2 host pages]", mprotect(g_arena + 8192, 8192, PROT_READ));
  CASE("B04", "mprotect(arena+1, 4096, NONE)  [not 4K aligned]", mprotect(g_arena + 1, 4096, PROT_NONE));
  CASE("B04", "mprotect(arena, 1, NONE)", mprotect(g_arena, 1, PROT_NONE));
  CASE("B04", "mprotect(arena+16384, 16384, NONE) [aligned, control]", mprotect(g_arena + 16384, 16384, PROT_NONE));
  CASEK("B04", "mach_vm_protect(arena+4096, 4096, NONE)", mach_vm_protect(mach_task_self(), (mach_vm_address_t)(g_arena + 4096), 4096, FALSE, VM_PROT_NONE));
  CASEK("B04", "mach_vm_protect(arena+4096, 4096, READ)", mach_vm_protect(mach_task_self(), (mach_vm_address_t)(g_arena + 4096), 4096, FALSE, VM_PROT_READ));
  CASEK("B04", "mach_vm_protect(arena+1, 4096, NONE) [not 4K aligned]", mach_vm_protect(mach_task_self(), (mach_vm_address_t)(g_arena + 1), 4096, FALSE, VM_PROT_NONE));
  /* what mach_vm_region reports after the first case */
  arena_reset();
  mprotect(g_arena + 4096, 4096, PROT_NONE);
  char line[300]; int off = 0;
  for (int i = 0; i < 6; i++) {
    vm_prot_t mx; mach_vm_address_t rs; mach_vm_size_t rz;
    int p = dm_prot_at((mach_vm_address_t)(g_arena + (size_t)i * 4096), &mx, &rs, &rz);
    off += snprintf(line + off, sizeof line - off, " c%d:prot=%d(region 0x%llx+0x%llx)", i, p, (unsigned long long)(rs - (mach_vm_address_t)g_arena), (unsigned long long)rz);
  }
  INFO("B04", "mach_vm_region after mprotect(arena+4096,4096,NONE):%s", line);
}

static void b05_mmap_fixed(void) {
  CASE("B05", "mmap(arena+4096, 4096, RW, FIXED)", (long)mmap(g_arena + 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (long)MAP_FAILED ? -1 : 0);
  CASE("B05", "mmap(arena+4096, 16384, RW, FIXED)", (long)mmap(g_arena + 4096, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (long)MAP_FAILED ? -1 : 0);
  CASE("B05", "mmap(arena+1, 4096, RW, FIXED)  [not 4K aligned]", (long)mmap(g_arena + 1, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (long)MAP_FAILED ? -1 : 0);
  CASE("B05", "mmap(arena+8192, 4096, NONE, FIXED)", (long)mmap(g_arena + 8192, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (long)MAP_FAILED ? -1 : 0);
  CASE("B05", "mmap(arena+16384, 4096, RW, FIXED) [aligned, control]", (long)mmap(g_arena + 16384, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == (long)MAP_FAILED ? -1 : 0);
  /* mmap with a hint that is 4K- but not 16K-aligned, on free space */
  uint8_t *base = mmap(NULL, 256 * KiB, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  munmap(base, 256 * KiB);
  uint8_t *p = mmap(base + 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("B05", "mmap(hint=free 4K-aligned address base+0x1000, no FIXED) -> %s (base=%p got=%p)", p == MAP_FAILED ? errno_name(errno) : (p == base + 4096 ? "hint honoured exactly" : "hint moved/rounded"), (void *)base, (void *)p);
  if (p != MAP_FAILED) munmap(p, 4096);
  p = mmap(base + 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
  INFO("B05", "mmap(base+0x1000, 4096, FIXED) on free space -> %s (got=%p)", p == MAP_FAILED ? errno_name(errno) : "mapped", (void *)p);
  if (p != MAP_FAILED) {
    dm_region r = dm_region_at_or_after((mach_vm_address_t)p);
    INFO("B05", "  resulting region: 0x%llx size 0x%llx (start offset from base %lld)", (unsigned long long)r.start, (unsigned long long)r.size, (long long)(r.start - (mach_vm_address_t)base));
    munmap(p, 4096);
  }
}

static void b06_file(void) {
  snprintf(g_tmpfile, sizeof g_tmpfile, "%s/dm_b06_%d.bin", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
  int fd = open(g_tmpfile, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) { FAIL("B06", "cannot create temp file %s errno=%s", g_tmpfile, errno_name(errno)); return; }
  uint8_t buf[4096];
  for (int i = 0; i < 16; i++) { memset(buf, 0xA0 + i, sizeof buf); (void)!write(fd, buf, sizeof buf); }
  static const off_t offs[] = {0, 4096, 8192, 12288, 16384, 20480, 32768 + 4096};
  for (unsigned i = 0; i < sizeof offs / sizeof *offs; i++) {
    uint8_t *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, offs[i]);
    int e = errno;
    uint8_t v = 0;
    int rd = (p != MAP_FAILED) && try_read(p, &v);
    if (p == MAP_FAILED) INFO("B06", "mmap(NULL, 4096, R, PRIVATE, file, offset 0x%llx) -> %s", (unsigned long long)offs[i], errno_name(e));
    else INFO("B06", "mmap(NULL, 4096, R, PRIVATE, file, offset 0x%llx) -> mapped, first byte 0x%02x (file page %d holds 0x%02x)", (unsigned long long)offs[i], rd ? v : 0, (int)(offs[i] / 4096), 0xA0 + (int)(offs[i] / 4096));
    if (p != MAP_FAILED) munmap(p, 4096);
  }
  for (unsigned i = 1; i < 4; i++) {
    arena_reset();
    uint8_t *p = mmap(g_arena + 16384, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, (off_t)i * 4096);
    int e = errno;
    uint8_t v = 0;
    int rd = (p != MAP_FAILED) && try_read(p, &v);
    if (p == MAP_FAILED) INFO("B06", "mmap(arena+16384, 4096, R, FIXED, file, offset 0x%x) -> %s", i * 4096, errno_name(e));
    else INFO("B06", "mmap(arena+16384, 4096, R, FIXED, file, offset 0x%x) -> mapped, first byte 0x%02x", i * 4096, rd ? v : 0);
  }
  arena_reset();
  {
    uint8_t *p = mmap(g_arena + 4096, 4096, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 4096);
    INFO("B06", "mmap(arena+4096, 4096, R, FIXED, file, offset 0x1000) -> %s", p == MAP_FAILED ? errno_name(errno) : "mapped");
  }
  arena_reset();
  {
    uint8_t *p = mmap(g_arena + 16384, 16384, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
    uint8_t v0 = 0, v1 = 0, v3 = 0;
    int ok = (p != MAP_FAILED) && try_read(p, &v0) && try_read(p + 4096, &v1) && try_read(p + 12288, &v3);
    INFO("B06", "aligned-down workaround: mmap 16K of the file at offset 0 -> %s; 4K chunk bytes 0x%02x 0x%02x ... 0x%02x (a 4K-offset view lives at +delta inside the host page and its neighbours show other file data)", ok ? "ok" : "failed", v0, v1, v3);
  }
  close(fd);
  unlink(g_tmpfile);
}

static void b07_advice(void) {
  /* madvise on 4K ranges: does it affect the neighbours inside the host page? */
  static const struct { const char *n; int adv; } adv[] = {{"MADV_FREE", MADV_FREE}, {"MADV_DONTNEED", MADV_DONTNEED}, {"MADV_ZERO", MADV_ZERO}, {"MADV_FREE_REUSABLE", MADV_FREE_REUSABLE}, {"MADV_WILLNEED", MADV_WILLNEED}};
  for (unsigned i = 0; i < sizeof adv / sizeof *adv; i++) {
    arena_reset();
    memset(g_arena + 4096, 0xCD, 4096);
    memset(g_arena + 8192, 0xCD, 4096);
    int r = madvise(g_arena + 4096, 4096, adv[i].adv);
    int e = errno;
    uint8_t c0 = 0, c1 = 0, c2 = 0;
    try_read(g_arena + 4096, &c0); try_read(g_arena + 4096 + 4095, &c1); try_read(g_arena + 8192, &c2);
    INFO("B07", "madvise(arena+4096, 4096, %s) -> %d %s | byte at +4096 now 0x%02x, +8191 0x%02x, neighbour chunk 2 first byte 0x%02x (0xcd = untouched, 0x00 = zeroed)", adv[i].n, r,
         r ? errno_name(e) : "", c0, c1, c2);
  }
  arena_reset();
  unsigned char vec[16];
  memset(vec, 0xAA, sizeof vec);
  int r = mincore(g_arena, 16384, (char *)vec);
  int nset = 0; while (nset < 16 && vec[nset] != 0xAA) nset++;
  INFO("B07", "mincore(arena, 16384) -> %d; vec entries written: %d (vec[0]=0x%02x vec[1]=0x%02x vec[3]=0x%02x)", r, nset, vec[0], vec[1], vec[3]);
  memset(vec, 0xAA, sizeof vec);
  r = mincore(g_arena + 4096, 4096, (char *)vec);
  INFO("B07", "mincore(arena+4096, 4096) -> %d %s; vec[0]=0x%02x", r, r ? errno_name(errno) : "", vec[0]);
  r = msync(g_arena + 4096, 4096, MS_ASYNC);
  INFO("B07", "msync(arena+4096, 4096, MS_ASYNC) -> %d %s", r, r ? errno_name(errno) : "");
  r = mlock(g_arena + 4096, 4096);
  INFO("B07", "mlock(arena+4096, 4096) -> %d %s", r, r ? errno_name(errno) : "");
  if (!r) munlock(g_arena + 4096, 4096);
}

static void b08_mach(void) {
  /* memory entry of 4 KiB and its mapping */
  memory_object_size_t sz = 4096;
  mach_port_t ent = MACH_PORT_NULL;
  kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &sz, (memory_object_offset_t)(uintptr_t)(g_arena), VM_PROT_READ | VM_PROT_WRITE, &ent, MACH_PORT_NULL);
  INFO("B08", "mach_make_memory_entry_64(arena, size=4096) -> %s, entry size returned=%llu", kr_name(kr), (unsigned long long)sz);
  if (kr == KERN_SUCCESS) {
    mach_vm_address_t a = 0;
    kr = mach_vm_map(mach_task_self(), &a, 4096, 0, VM_FLAGS_ANYWHERE, ent, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE);
    dm_region r = dm_region_at_or_after(a);
    INFO("B08", "mach_vm_map of that 4096-byte entry (size 4096) -> %s at 0x%llx, region size 0x%llx", kr_name(kr), (unsigned long long)a, (unsigned long long)r.size);
    if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 4096);
    a = 0;
    kr = mach_vm_map(mach_task_self(), &a, 4096, 0, VM_FLAGS_ANYWHERE, ent, 4096, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE);
    INFO("B08", "mach_vm_map of the entry at offset 4096 -> %s", kr_name(kr));
    if (kr == KERN_SUCCESS) mach_vm_deallocate(mach_task_self(), a, 4096);
    mach_port_deallocate(mach_task_self(), ent);
  }
  /* mach_make_memory_entry_64 starting at a 4K offset */
  sz = 4096;
  kr = mach_make_memory_entry_64(mach_task_self(), &sz, (memory_object_offset_t)(uintptr_t)(g_arena + 4096), VM_PROT_READ | VM_PROT_WRITE, &ent, MACH_PORT_NULL);
  INFO("B08", "mach_make_memory_entry_64(arena+4096, size=4096) -> %s, size returned=%llu", kr_name(kr), (unsigned long long)sz);
  if (kr == KERN_SUCCESS) mach_port_deallocate(mach_task_self(), ent);
  /* remap of a 4K slice, with and without the data-address flags */
  static const struct { const char *n; int fl; } fl[] = {{"ANYWHERE", VM_FLAGS_ANYWHERE}, {"ANYWHERE|RETURN_DATA_ADDR", VM_FLAGS_ANYWHERE | VM_FLAGS_RETURN_DATA_ADDR}, {"ANYWHERE|RETURN_4K_DATA_ADDR", VM_FLAGS_ANYWHERE | VM_FLAGS_RETURN_4K_DATA_ADDR}};
  for (unsigned i = 0; i < sizeof fl / sizeof *fl; i++) {
    arena_reset();
    mach_vm_address_t tgt = 0;
    vm_prot_t cur, mx;
    kr = mach_vm_remap(mach_task_self(), &tgt, 4096, 0, fl[i].fl, mach_task_self(), (mach_vm_address_t)(g_arena + 4096), FALSE, &cur, &mx, VM_INHERIT_NONE);
    INFO("B08", "mach_vm_remap(src=arena+4096, size=4096, %s, copy=FALSE) -> %s target=0x%llx (target offset in page 0x%llx)", fl[i].n, kr_name(kr), (unsigned long long)tgt, (unsigned long long)(tgt & (vm_page_size - 1)));
    if (kr == KERN_SUCCESS) {
      dm_region r = dm_region_at_or_after(tgt);
      INFO("B08", "  target region starts 0x%llx size 0x%llx", (unsigned long long)r.start, (unsigned long long)r.size);
      mach_vm_deallocate(mach_task_self(), tgt & ~(mach_vm_address_t)(vm_page_size - 1), 16384);
    }
  }
  /* alias protections: does a second mapping of the same memory get independent protection? (16K granularity expected) */
  arena_reset();
  mach_vm_address_t alias = 0;
  vm_prot_t cur, mx;
  kr = mach_vm_remap(mach_task_self(), &alias, 16384, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (mach_vm_address_t)g_arena, FALSE, &cur, &mx, VM_INHERIT_NONE);
  if (kr == KERN_SUCCESS) {
    int okp = mprotect((void *)alias, 16384, PROT_READ) == 0;
    g_arena[5] = 0x42;
    uint8_t seen = 0;
    try_read((uint8_t *)alias + 5, &seen);
    int w = try_write((uint8_t *)alias + 5, 1);
    CHECK(okp && seen == 0x42 && !w && try_write(g_arena + 6, 7), "B08", "shared alias of 16K via mach_vm_remap: the alias sees writes through the original (0x%02x), has its own protection (alias write faults: %d, original still writable)", seen, !w);
    mach_vm_deallocate(mach_task_self(), alias, 16384);
  } else INFO("B08", "mach_vm_remap alias of 16K -> %s", kr_name(kr));
}

/* W^X facts that decide whether a union of protections inside a host page is available */
static void b09_wx(void) {
  uint8_t *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  int r = mprotect(p, 16384, PROT_READ | PROT_WRITE | PROT_EXEC);
  INFO("B09", "mprotect(RW anonymous page -> RWX) = %d %s", r, r ? errno_name(errno) : "");
  void *q = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("B09", "mmap(RWX anonymous, no MAP_JIT) = %s", q == MAP_FAILED ? errno_name(errno) : "mapped");
  if (q != MAP_FAILED) munmap(q, 16384);
  r = mprotect(p, 16384, PROT_READ | PROT_EXEC);
  INFO("B09", "mprotect(RW anonymous page -> RX) = %d %s", r, r ? errno_name(errno) : "");
  r = mprotect(p, 16384, PROT_READ | PROT_WRITE);
  INFO("B09", "mprotect(RX -> RW again) = %d %s", r, r ? errno_name(errno) : "");
  munmap(p, 16384);
}

static double secs(uint64_t a, uint64_t b) { return (b - a) / 1e9; }
static void b10_bench(void) {
  const int N = 20000;
  uint8_t *p = mmap(NULL, 1 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  memset(p, 1, 1 * MiB);
  uint64_t t0 = now_ns();
  for (int i = 0; i < N; i++) { mprotect(p + 4096, 4096, PROT_NONE); mprotect(p + 4096, 4096, PROT_READ | PROT_WRITE); }
  uint64_t t1 = now_ns();
  INFO("B10", "mprotect 4096-byte range NONE/RW ping-pong: %.0f ns per call (%d calls)", (t1 - t0) / (2.0 * N), 2 * N);
  t0 = now_ns();
  for (int i = 0; i < N; i++) { mprotect(p, 16384, PROT_NONE); mprotect(p, 16384, PROT_READ | PROT_WRITE); }
  t1 = now_ns();
  INFO("B10", "mprotect 16384-byte range NONE/RW ping-pong: %.0f ns per call", (t1 - t0) / (2.0 * N));
  t0 = now_ns();
  for (int i = 0; i < N; i++) { uint8_t *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); m[0] = 1; munmap(m, 4096); }
  t1 = now_ns();
  INFO("B10", "mmap(4096)+touch+munmap: %.0f ns per cycle", (double)(t1 - t0) / N);
  /* page fault cost and count */
  struct rusage ru0, ru1;
  size_t len = 64 * MiB;
  uint8_t *f = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  getrusage(RUSAGE_SELF, &ru0);
  t0 = now_ns();
  for (size_t o = 0; o < len; o += 4096) f[o] = 1;
  t1 = now_ns();
  getrusage(RUSAGE_SELF, &ru1);
  INFO("B10", "touching every 4096th byte of 64 MiB: %.1f ms, %ld minor faults (%.2f faults per MiB), %.0f ns per 4K step", secs(t0, t1) * 1e3, ru1.ru_minflt - ru0.ru_minflt,
       (ru1.ru_minflt - ru0.ru_minflt) / 64.0, (double)(t1 - t0) / (len / 4096));
  munmap(f, len);
  munmap(p, 1 * MiB);
}

int main(int argc, char **argv) {
  dm_arm_watchdog(180);
  setvbuf(stdout, NULL, _IOLBF, 0);
  install_handlers();
  bool bench = true;
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--nobench")) bench = false;
  const char *base = strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  printf("# b_pagesize variant=%s arch=%s\n", base, arch_name());
  g_blk = mmap(NULL, BLK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (g_blk == MAP_FAILED) { printf("FATAL cannot map the arena block\n"); return 3; }
  g_arena = (uint8_t *)(((uintptr_t)g_blk + 256 * KiB + 65535) & ~(uintptr_t)65535);
  b01_numbers();
  b02_sizes();
  b03_munmap();
  b04_mprotect();
  b05_mmap_fixed();
  b06_file();
  b07_advice();
  b08_mach();
  b09_wx();
  if (bench) b10_bench();
  printf("# summary pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
