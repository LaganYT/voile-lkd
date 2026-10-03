/*
 * c_jit_basic.c - fact group C (JIT), single-threaded facts.
 *
 *   C01 environment (code signing flags, cache geometry, features)
 *   C02 MAP_JIT basics and the two states of the per-thread write protection
 *   C03 instruction cache invalidation (is it needed, which sequences work)
 *   C04 RWX and RW->RX mappings without MAP_JIT
 *   C05 largest MAP_JIT reservation
 *   C06 number of MAP_JIT regions
 *   C07 mprotect on MAP_JIT pages
 *   C08 MAP_FIXED, address hints and mach_vm_remap with MAP_JIT
 *   C09 madvise on MAP_JIT pages (code cache reset)
 *   C10 transfers of control between JIT regions and host code
 *
 * Every risky step runs in a forked child with a time limit, so a kill by the
 * kernel (code signing) shows up as a measured result instead of ending the run.
 */
#include <fcntl.h>
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define GiB (1ULL << 30)
#define TiB (1ULL << 40)

typedef uint64_t (*fn0_t)(void);

/* ------------------------------------------------------------------------- */
/* Templates assembled by the toolchain and copied into JIT memory.           */
/* ------------------------------------------------------------------------- */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _cd_tpl_call_host\n"
    "_cd_tpl_call_host:\n"          /* uint64_t f(a0=x0,a1=x1): calls the host function in the literal */
    "  str x30, [sp, #-16]!\n"
    "  ldr x16, 1f\n"
    "  blr x16\n"
    "  ldr x30, [sp], #16\n"
    "  ret\n"
    ".p2align 3\n"
    "1:\n"
    ".globl _cd_tpl_call_host_lit\n"
    "_cd_tpl_call_host_lit:\n"
    "  .quad 0\n"
    ".globl _cd_tpl_call_host_end\n"
    "_cd_tpl_call_host_end:\n"

    ".p2align 4\n"
    ".globl _cd_tpl_bl_to\n"
    "_cd_tpl_bl_to:\n"              /* calls another JIT function with bl (offset patched after copy) */
    "  str x30, [sp, #-16]!\n"
    ".globl _cd_tpl_bl_to_insn\n"
    "_cd_tpl_bl_to_insn:\n"
    "  bl _cd_tpl_bl_to\n"          /* placeholder, replaced by the C code */
    "  add x0, x0, #1\n"
    "  ldr x30, [sp], #16\n"
    "  ret\n"
    ".globl _cd_tpl_bl_to_end\n"
    "_cd_tpl_bl_to_end:\n"

    ".p2align 4\n"
    ".globl _cd_tpl_far_tail\n"
    "_cd_tpl_far_tail:\n"           /* tail call through a literal: ldr x16, lit ; br x16 */
    "  ldr x16, 1f\n"
    "  br x16\n"
    ".p2align 3\n"
    "1:\n"
    ".globl _cd_tpl_far_tail_lit\n"
    "_cd_tpl_far_tail_lit:\n"
    "  .quad 0\n"
    ".globl _cd_tpl_far_tail_end\n"
    "_cd_tpl_far_tail_end:\n"

    ".p2align 4\n"
    ".globl _cd_tpl_ret77\n"
    "_cd_tpl_ret77:\n"
    "  mov x0, #77\n"
    "  ret\n"
    ".globl _cd_tpl_ret77_end\n"
    "_cd_tpl_ret77_end:\n"

    ".p2align 4\n"
    ".globl _cd_tpl_addone\n"
    "_cd_tpl_addone:\n"              /* x0 = x0 + 1 */
    "  add x0, x0, #1\n"
    "  ret\n"
    ".globl _cd_tpl_addone_end\n"
    "_cd_tpl_addone_end:\n");

extern char cd_tpl_call_host[], cd_tpl_call_host_lit[], cd_tpl_call_host_end[];
extern char cd_tpl_bl_to[], cd_tpl_bl_to_insn[], cd_tpl_bl_to_end[];
extern char cd_tpl_far_tail[], cd_tpl_far_tail_lit[], cd_tpl_far_tail_end[];
extern char cd_tpl_ret77[], cd_tpl_ret77_end[];
extern char cd_tpl_addone[], cd_tpl_addone_end[];

static uint64_t host_add(uint64_t a, uint64_t b) { return a * 1000 + b; }

/* Copies [start,end) into dst (the caller holds the write state) and returns end of copy. */
static size_t cd_copy_tpl(void *dst, const char *start, const char *end) {
  size_t n = (size_t)(end - start);
  memcpy(dst, start, n);
  return n;
}

/* ------------------------------------------------------------------------- */
/* C01                                                                        */
/* ------------------------------------------------------------------------- */
static void c01_environment(const char *prog) {
  cd_print_header(prog);
  cd_install_probe_handlers();
  int ctr_sig = 0;
  uint64_t ctr = cd_read_ctr_el0_checked(&ctr_sig);
  if (ctr_sig)
    INFO("C01.ctr_el0", "mrs CTR_EL0 from user space traps with %s (cache geometry comes from sysctl instead: hw.cachelinesize=%zu)",
         sig_name(ctr_sig), cd_cache_line());
  else
    INFO("C01.ctr_el0", "CTR_EL0=%#llx IminLine=%u bytes DminLine=%u bytes ERG=%llu CWG=%llu IDC=%llu DIC=%llu",
         (unsigned long long)ctr, 4u << (ctr & 0xf), 4u << ((ctr >> 16) & 0xf), (unsigned long long)((ctr >> 20) & 0xf),
         (unsigned long long)((ctr >> 24) & 0xf), (unsigned long long)((ctr >> 28) & 1),
         (unsigned long long)((ctr >> 29) & 1));
  INFO("C01.cntfrq", "CNTFRQ_EL0=%llu Hz (timer tick = %.2f ns)", (unsigned long long)cd_cntfrq(),
       1e9 / (double)cd_cntfrq());
  static const char *names[] = {"hw.cachelinesize", "hw.l1icachesize", "hw.l1dcachesize", "hw.l2cachesize",
                                "hw.optional.arm.FEAT_LSE", "hw.optional.arm.FEAT_LSE2", "hw.optional.arm.FEAT_BTI",
                                "hw.optional.arm.FEAT_PAuth", "hw.optional.arm.FEAT_FlagM",
                                "hw.optional.arm.FEAT_FlagM2", "hw.optional.arm.FEAT_FCMA",
                                "hw.optional.arm.FEAT_JSCVT", "hw.optional.arm.FEAT_SME",
                                "hw.optional.arm.FEAT_SME2", "hw.optional.arm.FEAT_SB",
                                "hw.optional.arm.FEAT_AFP", "hw.optional.arm.FEAT_RPRES",
                                "hw.optional.arm.FEAT_MTE", "hw.optional.arm.FEAT_PAN"};
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    long long v = cd_sysctl_ll(names[i]);
    if (v < 0) INFO("C01.sysctl", "%s = absent", names[i]);
    else INFO("C01.sysctl", "%s = %lld", names[i], v);
  }
}

/* ------------------------------------------------------------------------- */
/* C02                                                                        */
/* ------------------------------------------------------------------------- */
static void c02_basic(void) {
  int err = 0;
  void *p = cd_jit_map(64 * KiB, CD_RWX, 0, &err);
  if (!p) {
    FAIL("C02.mapjit", "mmap(NULL, 64 KiB, RWX, MAP_PRIVATE|MAP_ANON|MAP_JIT) failed: %s", errno_name(err));
    return;
  }
  PASS("C02.mapjit", "mmap(NULL, 64 KiB, RWX, MAP_PRIVATE|MAP_ANON|MAP_JIT) = %p", p);
  dm_region r = dm_region_at_or_after((mach_vm_address_t)p);
  char b1[4], b2[4];
  INFO("C02.region", "mach_vm_region: start=%#llx size=%#llx prot=%s maxprot=%s share_mode=%d",
       (unsigned long long)r.start, (unsigned long long)r.size, prot_str(r.prot, b1), prot_str(r.maxprot, b2),
       r.share_mode);

  cd_install_probe_handlers();
  uint32_t *c = p;
  cd_probe_result pr;
  char sb[160];
  int sig;

  /* Default state of the main thread, before any toggle. */
  sig = cd_probe_store32(&c[0], A64_RET, &pr);
  INFO("C02.default-state", "main thread, before any call to pthread_jit_write_protect_np: store to JIT page = %s",
       cd_probe_str(sig, &pr, sb, sizeof sb));
  int default_writable = (sig == 0);
  if (default_writable) {
    sig = cd_probe_call(c, NULL, &pr);
    INFO("C02.default-state", "main thread default (writable): call into the same page = %s",
         cd_probe_str(sig, &pr, sb, sizeof sb));
  } else {
    /* Install a stub in write mode, then check execution in the default state. */
    cd_jit_write();
    cd_emit_ret_const(c, 42);
    cd_jit_exec();
    sys_icache_invalidate(c, 8);
    uint64_t out = 0;
    sig = cd_probe_call(c, &out, &pr);
    INFO("C02.default-state", "main thread default (not writable): call stub = %s (returned %llu)",
         cd_probe_str(sig, &pr, sb, sizeof sb), (unsigned long long)out);
  }
  /* Restore the state we are about to rely on: explicit toggles from now on. */

  /* The four combinations (state x access). */
  struct { const char *name; int w; } st[2] = {{"write-mode (pthread_jit_write_protect_np(0))", 1},
                                                {"exec-mode  (pthread_jit_write_protect_np(1))", 0}};
  for (int i = 0; i < 2; i++) {
    /* make sure a valid stub exists */
    cd_jit_write();
    cd_emit_ret_const(&c[16], 7);
    cd_jit_exec();
    sys_icache_invalidate(&c[16], 8);
    if (st[i].w) cd_jit_write(); else cd_jit_exec();
    sig = cd_probe_store32(&c[32], 0x11223344u, &pr);
    INFO("C02.states", "%s: store = %s", st[i].name, cd_probe_str(sig, &pr, sb, sizeof sb));
    if (st[i].w) cd_jit_write(); else cd_jit_exec();
    uint32_t v = 0;
    sig = cd_probe_load32(&c[32], &v, &pr);
    INFO("C02.states", "%s: load  = %s", st[i].name, cd_probe_str(sig, &pr, sb, sizeof sb));
    if (st[i].w) cd_jit_write(); else cd_jit_exec();
    uint64_t out = 0;
    sig = cd_probe_call(&c[16], &out, &pr);
    INFO("C02.states", "%s: call  = %s (returned %llu)", st[i].name, cd_probe_str(sig, &pr, sb, sizeof sb),
         (unsigned long long)out);
  }
  cd_jit_exec();

  /* Canonical write / execute cycle. */
  cd_jit_write();
  cd_emit_ret_const(&c[64], 1234);
  cd_jit_exec();
  sys_icache_invalidate(&c[64], 8);
  uint64_t out = ((fn0_t)&c[64])();
  CHECK(out == 1234, "C02.cycle", "write mode, store 'movz x0,#1234; ret', exec mode, sys_icache_invalidate, call -> %llu",
        (unsigned long long)out);

  /* Mixed prot flags on the MAP_JIT mmap. */
  static const struct { const char *name; int prot; } prots[] = {
      {"PROT_NONE", PROT_NONE}, {"RW", PROT_READ | PROT_WRITE}, {"RX", PROT_READ | PROT_EXEC}, {"R", PROT_READ},
      {"X only", PROT_EXEC}, {"RWX", CD_RWX}};
  for (size_t i = 0; i < sizeof prots / sizeof prots[0]; i++) {
    int e2 = 0;
    void *q = cd_jit_map(16 * KiB, prots[i].prot, 0, &e2);
    if (q) {
      dm_region rr = dm_region_at_or_after((mach_vm_address_t)q);
      INFO("C02.mmap-prot", "MAP_JIT with initial prot %-9s: ok, region prot=%s maxprot=%s", prots[i].name,
           prot_str(rr.prot, b1), prot_str(rr.maxprot, b2));
      munmap(q, 16 * KiB);
    } else {
      INFO("C02.mmap-prot", "MAP_JIT with initial prot %-9s: %s", prots[i].name, errno_name(e2));
    }
  }
  /* MAP_JIT combined with flags that the kernel may reject. */
  {
    int e2 = 0;
    void *q = mmap(NULL, 16 * KiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_NORESERVE, -1, 0);
    e2 = (q == MAP_FAILED) ? errno : 0;
    INFO("C02.mmap-flags", "MAP_JIT|MAP_NORESERVE: %s", q == MAP_FAILED ? errno_name(e2) : "ok");
    if (q != MAP_FAILED) munmap(q, 16 * KiB);
    q = mmap(NULL, 16 * KiB, CD_RWX, MAP_SHARED | MAP_ANON | MAP_JIT, -1, 0);
    e2 = (q == MAP_FAILED) ? errno : 0;
    INFO("C02.mmap-flags", "MAP_JIT|MAP_SHARED: %s", q == MAP_FAILED ? errno_name(e2) : "ok");
    if (q != MAP_FAILED) munmap(q, 16 * KiB);
    q = mmap(NULL, 16 * KiB, CD_RWX, MAP_PRIVATE | MAP_JIT, -1, 0);
    e2 = (q == MAP_FAILED) ? errno : 0;
    INFO("C02.mmap-flags", "MAP_JIT without MAP_ANON (fd=-1): %s", q == MAP_FAILED ? errno_name(e2) : "ok");
    if (q != MAP_FAILED) munmap(q, 16 * KiB);
    /* size not a multiple of the page size */
    q = mmap(NULL, 4 * KiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (q != MAP_FAILED) {
      dm_region rr = dm_region_at_or_after((mach_vm_address_t)q);
      INFO("C02.mmap-flags", "MAP_JIT of 4 KiB: ok, kernel region size=%#llx (host page = 16 KiB)",
           (unsigned long long)rr.size);
      munmap(q, 4 * KiB);
    } else {
      INFO("C02.mmap-flags", "MAP_JIT of 4 KiB: %s", errno_name(errno));
    }
  }
  munmap(p, 64 * KiB);
}

/* ------------------------------------------------------------------------- */
/* C03: instruction cache                                                     */
/* ------------------------------------------------------------------------- */
static inline void manual_dc_cvau(void *p, size_t n) {
  size_t line = cd_cache_line();
  uintptr_t a = (uintptr_t)p & ~(line - 1), e = (uintptr_t)p + n;
  for (; a < e; a += line) __asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
}
static inline void manual_ic_ivau(void *p, size_t n) {
  size_t line = cd_cache_line();
  uintptr_t a = (uintptr_t)p & ~(line - 1), e = (uintptr_t)p + n;
  for (; a < e; a += line) __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
}
#define DSB_ISH() __asm__ volatile("dsb ish" ::: "memory")
#define ISB() __asm__ volatile("isb" ::: "memory")

typedef void (*inval_fn)(void *, size_t);
static void inv_none(void *p, size_t n) { (void)p; (void)n; }
static void inv_sys(void *p, size_t n) { sys_icache_invalidate(p, n); }
static void inv_builtin(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static void inv_dsb_isb(void *p, size_t n) { (void)p; (void)n; DSB_ISH(); ISB(); }
static void inv_dc_only(void *p, size_t n) { manual_dc_cvau(p, n); DSB_ISH(); ISB(); }
static void inv_ic_only(void *p, size_t n) { manual_ic_ivau(p, n); DSB_ISH(); ISB(); }
static void inv_full_manual(void *p, size_t n) {
  manual_dc_cvau(p, n);
  DSB_ISH();
  manual_ic_ivau(p, n);
  DSB_ISH();
  ISB();
}

static int child_icache(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  int err = 0;
  uint32_t *c = cd_jit_map(64 * KiB, CD_RWX, 0, &err);
  if (!c) { FAIL("C03.setup", "mmap MAP_JIT failed: %s", errno_name(err)); return 1; }
  static const struct { const char *name; inval_fn f; } v[] = {
      {"none", inv_none},
      {"sys_icache_invalidate", inv_sys},
      {"__builtin___clear_cache", inv_builtin},
      {"dsb ish + isb only", inv_dsb_isb},
      {"dc cvau + dsb + isb (no ic)", inv_dc_only},
      {"ic ivau + dsb + isb (no dc)", inv_ic_only},
      {"dc cvau; dsb; ic ivau; dsb; isb", inv_full_manual}};
  const int N = 200000;
  for (size_t k = 0; k < sizeof v / sizeof v[0]; k++) {
    {
      /* Does the sequence run at EL0 at all? */
      sigjmp_buf jb0;
      int sg = 0;
      memset(&t_probe_res, 0, sizeof t_probe_res);
      if (sigsetjmp(jb0, 1) == 0) {
        t_probe_jb = &jb0;
        v[k].f(c, 8);
        t_probe_jb = NULL;
      } else {
        t_probe_jb = NULL;
        sg = t_probe_res.sig;
      }
      if (sg) {
        INFO("C03.same-thread", "%-34s: traps with %s at EL0, skipped", v[k].name, sig_name(sg));
        continue;
      }
    }
    /* Same thread rewrites the instruction word and calls it right after.  The slot
     * is reused so a stale instruction cache line would give the previous value. */
    long stale = 0, faults = 0;
    int first_fault_sig = 0;
    uint32_t *slot = &c[(k * 64) % 4096];
    for (int i = 0; i < N; i++) {
      uint32_t val = 1 + (uint32_t)(i & 0x7ff);
      cd_jit_write();
      slot[0] = A64_MOVZ_X(0, val);
      slot[1] = A64_RET;
      cd_jit_exec();
      v[k].f(slot, 8);
      uint64_t out = 0;
      int sig = cd_probe_call(slot, &out, NULL);
      if (sig) { faults++; if (!first_fault_sig) first_fault_sig = sig; }
      else if (out != val) stale++;
    }
    cd_jit_exec();
    if (faults || stale)
      FAIL("C03.same-thread", "%-34s: %ld stale and %ld faulting executions (first fault %s) over %d rewrites", v[k].name, stale,
           faults, sig_name(first_fault_sig), N);
    else PASS("C03.same-thread", "%-34s: 0 stale executions over %d rewrites of the same instruction word", v[k].name, N);
  }
  /* Is the invalidation instruction usable while the thread is in exec mode, and in write mode? */
  for (int w = 0; w < 2; w++) {
    if (w) cd_jit_write(); else cd_jit_exec();
    int sig = 0;
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1) == 0) {
      t_probe_jb = &jb;
      sys_icache_invalidate(c, 64);
      t_probe_jb = NULL;
    } else {
      t_probe_jb = NULL;
      sig = t_probe_res.sig;
    }
    cd_jit_exec();
    INFO("C03.inval-state", "sys_icache_invalidate while in %s: %s", w ? "write mode" : "exec mode",
         sig ? sig_name(sig) : "ok");
  }
  /* Invalidating a range that is not mapped. */
  {
    int sig = 0;
    sigjmp_buf jb;
    void *hole = mmap(NULL, 16 * KiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    munmap(hole, 16 * KiB);
    if (sigsetjmp(jb, 1) == 0) {
      t_probe_jb = &jb;
      sys_icache_invalidate(hole, 64);
      t_probe_jb = NULL;
    } else {
      t_probe_jb = NULL;
      sig = t_probe_res.sig;
    }
    INFO("C03.inval-unmapped", "sys_icache_invalidate on an unmapped range: %s", sig ? sig_name(sig) : "ok, silently ignored");
  }
  return 0;
}

static void c03_icache(void) {
  int to = 0;
  int st = cd_run_child(child_icache, NULL, 60000, &to);
  char sb[96];
  INFO("C03.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C04: RWX and W^X dances without MAP_JIT                                    */
/* ------------------------------------------------------------------------- */
static int child_nojit(void *arg) {
  int mode = (int)(intptr_t)arg;
  uint64_t out = 0;
  size_t sz = 16 * KiB;
  void *p;
  switch (mode) {
  case 0:
    p = mmap(NULL, sz, CD_RWX, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { INFO("C04.rwx-anon", "mmap(RWX, MAP_ANON|MAP_PRIVATE) without MAP_JIT: %s", errno_name(errno)); return 10; }
    INFO("C04.rwx-anon", "mmap(RWX, MAP_ANON|MAP_PRIVATE) without MAP_JIT: ok at %p", p);
    cd_emit_ret_const(p, 5);
    sys_icache_invalidate(p, 8);
    out = ((fn0_t)p)();
    INFO("C04.rwx-anon", "wrote a stub and called it: returned %llu", (unsigned long long)out);
    return out == 5 ? 0 : 11;
  case 1:
    p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 12;
    cd_emit_ret_const(p, 6);
    sys_icache_invalidate(p, 8);
    if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0) { INFO("C04.rw-to-rx", "mmap(RW), write stub, mprotect(RX): %s", errno_name(errno)); return 10; }
    INFO("C04.rw-to-rx", "mmap(RW), write stub, mprotect(RX): ok");
    out = ((fn0_t)p)();
    INFO("C04.rw-to-rx", "called the stub: returned %llu", (unsigned long long)out);
    return out == 6 ? 0 : 11;
  case 2:
    p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 12;
    if (mprotect(p, sz, CD_RWX) != 0) { INFO("C04.rw-to-rwx", "mmap(RW), mprotect(RWX): %s", errno_name(errno)); return 10; }
    INFO("C04.rw-to-rwx", "mmap(RW), mprotect(RWX): ok");
    cd_emit_ret_const(p, 8);
    sys_icache_invalidate(p, 8);
    out = ((fn0_t)p)();
    INFO("C04.rw-to-rwx", "wrote and called: returned %llu", (unsigned long long)out);
    return out == 8 ? 0 : 11;
  case 3:
    p = mmap(NULL, sz, PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { INFO("C04.x-only", "mmap(PROT_EXEC) anon: %s", errno_name(errno)); return 10; }
    INFO("C04.x-only", "mmap(PROT_EXEC) anon: ok at %p", p);
    return 0;
  case 4: {
    /* file-backed executable mapping of an unsigned file written just now (AOT code cache case) */
    char path[PATH_MAX];
    const char *td = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/cd_c04_%d.bin", td ? td : "/private/tmp", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return 12;
    uint32_t code[4096];
    memset(code, 0, sizeof code);
    cd_emit_ret_const(code, 9);
    (void)!write(fd, code, sizeof code);
    p = mmap(NULL, 16 * KiB, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
      INFO("C04.file-x", "mmap(PROT_READ|PROT_EXEC, MAP_PRIVATE, unsigned temp file): %s", errno_name(errno));
      unlink(path);
      return 10;
    }
    INFO("C04.file-x", "mmap(PROT_READ|PROT_EXEC, MAP_PRIVATE, unsigned temp file): ok at %p", p);
    out = ((fn0_t)p)();
    INFO("C04.file-x", "called the stub in the file mapping: returned %llu", (unsigned long long)out);
    unlink(path);
    return out == 9 ? 0 : 11;
  }
  case 5: {
    /* mmap RX anon (no MAP_JIT) then try to make it writable and executable again */
    p = mmap(NULL, sz, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { INFO("C04.rx-dance", "mmap(RX) anon: %s", errno_name(errno)); return 10; }
    if (mprotect(p, sz, PROT_READ | PROT_WRITE) != 0) { INFO("C04.rx-dance", "mmap(RX) then mprotect(RW): %s", errno_name(errno)); return 10; }
    cd_emit_ret_const(p, 4);
    sys_icache_invalidate(p, 8);
    if (mprotect(p, sz, PROT_READ | PROT_EXEC) != 0) { INFO("C04.rx-dance", "mprotect back to RX: %s", errno_name(errno)); return 10; }
    out = ((fn0_t)p)();
    INFO("C04.rx-dance", "mmap(RX), mprotect(RW), write, mprotect(RX), call: returned %llu", (unsigned long long)out);
    return out == 4 ? 0 : 11;
  }
  default: return 99;
  }
}

static void c04_nojit(void) {
  static const char *names[] = {"mmap RWX anon without MAP_JIT", "mmap RW, write, mprotect RX",
                                "mmap RW, mprotect RWX, write, call", "mmap PROT_EXEC anon",
                                "unsigned file mapped PROT_EXEC", "mmap RX, mprotect RW, write, mprotect RX"};
  for (int m = 0; m < 6; m++) {
    int to = 0;
    int st = cd_run_child(child_nojit, (void *)(intptr_t)m, 15000, &to);
    char sb[96];
    INFO("C04.result", "%-44s -> child %s", names[m], cd_status_str(st, to, sb, sizeof sb));
  }
}

/* ------------------------------------------------------------------------- */
/* C05: reservation sizes                                                     */
/* ------------------------------------------------------------------------- */
static int child_sizes(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  static const unsigned long long sizes[] = {16 * KiB, 1 * MiB, 64 * MiB, 256 * MiB, 1 * GiB, 4 * GiB, 8 * GiB,
                                             16 * GiB, 64 * GiB, 256 * GiB, 1 * TiB, 4 * TiB, 16 * TiB, 64 * TiB,
                                             100 * TiB, 127 * TiB, 128 * TiB};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    uint64_t t0 = now_ns();
    int err = 0;
    char *p = cd_jit_map(sizes[i], CD_RWX, 0, &err);
    uint64_t t1 = now_ns();
    if (!p) {
      INFO("C05.size", "MAP_JIT RWX %9llu MiB: %s (mmap took %.1f us)", sizes[i] / MiB, errno_name(err),
           (t1 - t0) / 1e3);
      continue;
    }
    /* Use the first and last page. */
    uint32_t *first = (uint32_t *)p, *last = (uint32_t *)(p + sizes[i] - 16 * KiB);
    cd_jit_write();
    cd_emit_ret_const(first, 11);
    cd_emit_ret_const(last, 22);
    cd_jit_exec();
    sys_icache_invalidate(first, 8);
    sys_icache_invalidate(last, 8);
    uint64_t r1 = ((fn0_t)first)(), r2 = ((fn0_t)last)();
    dm_region rg = dm_region_at_or_after((mach_vm_address_t)p);
    uint64_t t2 = now_ns();
    INFO("C05.size", "MAP_JIT RWX %9llu MiB: ok at %p, first/last page stubs return %llu/%llu, region size %llu MiB, mmap %.1f us, total %.1f us",
         sizes[i] / MiB, (void *)p, (unsigned long long)r1, (unsigned long long)r2, (unsigned long long)(rg.size / MiB),
         (t1 - t0) / 1e3, (t2 - t0) / 1e3);
    munmap(p, sizes[i]);
  }
  /* Same, with PROT_NONE reservation then mprotect to RWX. */
  {
    int err = 0;
    char *p = cd_jit_map(4 * GiB, PROT_NONE, 0, &err);
    if (!p) INFO("C05.reserve-none", "MAP_JIT PROT_NONE 4 GiB: %s", errno_name(err));
    else {
      int rc = mprotect(p, 64 * MiB, CD_RWX);
      INFO("C05.reserve-none", "MAP_JIT PROT_NONE 4 GiB at %p, mprotect(first 64 MiB, RWX) = %s", (void *)p,
           rc ? errno_name(errno) : "ok");
      if (rc == 0) {
        uint32_t *c = (uint32_t *)p;
        cd_jit_write();
        cd_emit_ret_const(c, 31);
        cd_jit_exec();
        sys_icache_invalidate(c, 8);
        INFO("C05.reserve-none", "stub in the committed part returns %llu", (unsigned long long)((fn0_t)c)());
        int sig = cd_probe_store32((volatile uint32_t *)(p + 128 * MiB), 1, NULL);
        INFO("C05.reserve-none", "store beyond the committed part (PROT_NONE): %s", sig ? sig_name(sig) : "ok?!");
      }
      munmap(p, 4 * GiB);
    }
  }
  return 0;
}

static void c05_sizes(void) {
  int to = 0;
  int st = cd_run_child(child_sizes, NULL, 60000, &to);
  char sb[96];
  INFO("C05.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C06: number of regions                                                     */
/* ------------------------------------------------------------------------- */
static int child_counts(void *arg) {
  (void)arg;
  struct { size_t size; size_t limit; const char *name; } cfg[] = {
      {16 * KiB, 100000, "16 KiB"}, {1 * MiB, 20000, "1 MiB"}, {1 * GiB, 4096, "1 GiB"}};
  for (size_t k = 0; k < 3; k++) {
    size_t cap = cfg[k].limit;
    void **ptrs = calloc(cap, sizeof(void *));
    size_t n = 0;
    int lasterr = 0;
    uint64_t t0 = now_ns();
    for (; n < cap; n++) {
      int err = 0;
      void *p = cd_jit_map(cfg[k].size, CD_RWX, 0, &err);
      if (!p) { lasterr = err; break; }
      ptrs[n] = p;
    }
    uint64_t t1 = now_ns();
    /* Count how many distinct vm entries the regions occupy (adjacent ones may merge). */
    size_t entries = 0;
    mach_vm_address_t a = 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < n; i++) {
      if ((uintptr_t)ptrs[i] < lo) lo = (uintptr_t)ptrs[i];
      if ((uintptr_t)ptrs[i] + cfg[k].size > hi) hi = (uintptr_t)ptrs[i] + cfg[k].size;
    }
    if (n) {
      a = lo;
      while (a < hi && entries < 2000000) {
        dm_region r = dm_region_at_or_after(a);
        if (!r.valid || r.start >= hi) break;
        entries++;
        a = r.start + r.size;
      }
    }
    INFO("C06.count", "MAP_JIT regions of %-6s: %zu allocated before %s (%.2f us per mmap), address span %llu MiB, %zu mach_vm_region entries",
         cfg[k].name, n, n == cap ? "reaching the test limit" : errno_name(lasterr), n ? (t1 - t0) / 1e3 / n : 0.0,
         (unsigned long long)((hi - lo) / MiB), entries);
    /* Use the last region to prove it is usable. */
    if (n) {
      uint32_t *c = ptrs[n - 1];
      cd_jit_write();
      cd_emit_ret_const(c, 5);
      cd_jit_exec();
      sys_icache_invalidate(c, 8);
      INFO("C06.count", "last region of that series usable: stub returns %llu", (unsigned long long)((fn0_t)c)());
    }
    uint64_t t2 = now_ns();
    for (size_t i = 0; i < n; i++) munmap(ptrs[i], cfg[k].size);
    uint64_t t3 = now_ns();
    INFO("C06.count", "  unmapping them all took %.1f ms (%.2f us each); series took %.1f ms", (t3 - t2) / 1e6,
         n ? (t3 - t2) / 1e3 / n : 0.0, (t2 - t0) / 1e6);
    free(ptrs);
  }
  return 0;
}

static void c06_counts(void) {
  int to = 0;
  int st = cd_run_child(child_counts, NULL, 120000, &to);
  char sb[96];
  INFO("C06.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C07: mprotect on MAP_JIT pages                                             */
/* ------------------------------------------------------------------------- */
static void describe_region(const char *id, const char *what, void *p) {
  dm_region r = dm_region_at_or_after((mach_vm_address_t)p);
  char b1[4], b2[4];
  if (r.valid && r.start <= (mach_vm_address_t)p)
    INFO(id, "%s: region %#llx+%#llx prot=%s maxprot=%s", what, (unsigned long long)r.start, (unsigned long long)r.size,
         prot_str(r.prot, b1), prot_str(r.maxprot, b2));
  else INFO(id, "%s: not mapped", what);
}

static int child_mprotect(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  const size_t pg = 16 * KiB;
  int err = 0;
  char *p = cd_jit_map(4 * pg, CD_RWX, 0, &err);
  if (!p) { FAIL("C07.setup", "mmap MAP_JIT failed: %s", errno_name(err)); return 1; }
  cd_probe_result pr;
  char sb[160];
  int sig;
  describe_region("C07.initial", "fresh MAP_JIT region", p);

  /* Matrix: initial protection (rows) x mprotect target (columns) on a fresh MAP_JIT page. */
  {
    static const struct { const char *n; int p; } pr_[] = {{"---", PROT_NONE}, {"r--", PROT_READ}, {"rw-", PROT_READ | PROT_WRITE},
                                                           {"r-x", PROT_READ | PROT_EXEC}, {"rwx", CD_RWX}};
    for (int i = 0; i < 5; i++) {
      char line[200];
      int o = snprintf(line, sizeof line, "MAP_JIT created %s, mprotect to:", pr_[i].n);
      for (int j = 0; j < 5; j++) {
        void *q = cd_jit_map(pg, pr_[i].p, 0, NULL);
        if (!q) { o += snprintf(line + o, sizeof line - o, " %s=mmapfail", pr_[j].n); continue; }
        int r2 = mprotect(q, pg, pr_[j].p);
        o += snprintf(line + o, sizeof line - o, " %s=%s", pr_[j].n, r2 ? errno_name(errno) : "ok");
        munmap(q, pg);
      }
      INFO("C07.matrix", "%s", line);
    }
    /* Same matrix on a plain anonymous RW/RX mapping (no MAP_JIT), for comparison. */
    for (int i = 0; i < 5; i++) {
      char line[200];
      int o = snprintf(line, sizeof line, "plain anon created %s, mprotect to:", pr_[i].n);
      for (int j = 0; j < 5; j++) {
        void *q = mmap(NULL, pg, pr_[i].p, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (q == MAP_FAILED) { o += snprintf(line + o, sizeof line - o, " %s=mmap-%s", pr_[j].n, errno_name(errno)); continue; }
        int r2 = mprotect(q, pg, pr_[j].p);
        o += snprintf(line + o, sizeof line - o, " %s=%s", pr_[j].n, r2 ? errno_name(errno) : "ok");
        munmap(q, pg);
      }
      INFO("C07.matrix", "%s", line);
    }
  }

  /* Two hops: a MAP_JIT page created with a lower protection, promoted to RWX, then lowered again. */
  {
    static const struct { const char *n; int p; } hop[] = {{"---", PROT_NONE}, {"rw-", PROT_READ | PROT_WRITE}};
    for (int i = 0; i < 2; i++) {
      char line[220];
      void *q = cd_jit_map(4 * pg, hop[i].p, 0, NULL);
      if (!q) continue;
      int o = snprintf(line, sizeof line, "MAP_JIT created %s: mprotect(page0,rwx)=", hop[i].n);
      int r1 = mprotect(q, pg, CD_RWX);
      o += snprintf(line + o, sizeof line - o, "%s", r1 ? errno_name(errno) : "ok");
      int r2 = mprotect(q, pg, PROT_NONE);
      o += snprintf(line + o, sizeof line - o, " then ---=%s", r2 ? errno_name(errno) : "ok");
      int r3 = mprotect(q, pg, PROT_READ | PROT_WRITE);
      o += snprintf(line + o, sizeof line - o, " then rw-=%s", r3 ? errno_name(errno) : "ok");
      int r4 = mprotect(q, pg, CD_RWX);
      o += snprintf(line + o, sizeof line - o, " then rwx=%s", r4 ? errno_name(errno) : "ok");
      /* is the page usable as JIT after the hops? */
      if (r4 == 0 || r1 == 0) {
        cd_jit_write();
        int sg = cd_probe_store32((volatile uint32_t *)q, A64_MOVZ_X(0, 9), NULL);
        if (!sg) ((volatile uint32_t *)q)[1] = A64_RET;
        cd_jit_exec();
        sys_icache_invalidate(q, 8);
        uint64_t out = 0;
        int sg2 = sg ? sg : cd_probe_call(q, &out, NULL);
        o += snprintf(line + o, sizeof line - o, "; write+exec afterwards %s out=%llu", sg2 ? sig_name(sg2) : "works",
                      (unsigned long long)out);
      }
      INFO("C07.twohop", "%s", line);
      /* sub-range promotion inside a larger reservation */
      char *res = q;
      int a1 = mprotect(res + pg, pg, CD_RWX);
      int a2 = mprotect(res + 2 * pg, pg, PROT_READ | PROT_WRITE);
      dm_region rr = dm_region_at_or_after((mach_vm_address_t)res);
      char b1[4], b2[4];
      INFO("C07.twohop", "  4-page reservation created %s: page1->rwx=%s page2->rw-=%s; first region now %#llx+%#llx prot=%s max=%s",
           hop[i].n, a1 ? errno_name(errno) : "ok", a2 ? errno_name(errno) : "ok", (unsigned long long)rr.start,
           (unsigned long long)rr.size, prot_str(rr.prot, b1), prot_str(rr.maxprot, b2));
      munmap(q, 4 * pg);
    }
  }

  /* page 0: RWX -> RW */
  int rc = mprotect(p, pg, PROT_READ | PROT_WRITE);
  INFO("C07.rwx-to-rw", "mprotect(page0, RW) = %s", rc ? errno_name(errno) : "ok");
  describe_region("C07.rwx-to-rw", "after", p);
  if (rc == 0) {
    cd_jit_exec();
    sig = cd_probe_store32((volatile uint32_t *)p, A64_RET, &pr);
    INFO("C07.rwx-to-rw", "store to page0 while thread is in exec mode: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
    rc = mprotect(p, pg, PROT_READ | PROT_EXEC);
    INFO("C07.rw-to-rx", "mprotect(page0, RX) = %s", rc ? errno_name(errno) : "ok");
    describe_region("C07.rw-to-rx", "after", p);
    if (rc == 0) {
      uint64_t out = 0;
      sig = cd_probe_call(p, &out, &pr);
      INFO("C07.rw-to-rx", "call stub in page0 (RX, written while RW) = %s out=%llu", cd_probe_str(sig, &pr, sb, sizeof sb),
           (unsigned long long)out);
    }
    rc = mprotect(p, pg, CD_RWX);
    INFO("C07.back-to-rwx", "mprotect(page0, RWX) = %s", rc ? errno_name(errno) : "ok");
    describe_region("C07.back-to-rwx", "after", p);
  }

  /* page 1: PROT_NONE and back */
  rc = mprotect(p + pg, pg, PROT_NONE);
  INFO("C07.none", "mprotect(page1, PROT_NONE) = %s", rc ? errno_name(errno) : "ok");
  describe_region("C07.none", "after", p + pg);
  if (rc == 0) {
    sig = cd_probe_load32((volatile uint32_t *)(p + pg), NULL, &pr);
    INFO("C07.none", "load from the PROT_NONE page: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
    rc = mprotect(p + pg, pg, CD_RWX);
    INFO("C07.none", "mprotect(page1, RWX) again = %s", rc ? errno_name(errno) : "ok");
    describe_region("C07.none", "after", p + pg);
    cd_jit_write();
    cd_emit_ret_const((uint32_t *)(p + pg), 3);
    cd_jit_exec();
    sys_icache_invalidate(p + pg, 8);
    uint64_t out = 0;
    sig = cd_probe_call(p + pg, &out, &pr);
    INFO("C07.none", "after PROT_NONE -> RWX round trip, write-mode store + exec-mode call: %s out=%llu",
         cd_probe_str(sig, &pr, sb, sizeof sb), (unsigned long long)out);
  }

  /* page 2: read-only (the SMC case applied to a JIT page) */
  cd_jit_write();
  cd_emit_ret_const((uint32_t *)(p + 2 * pg), 21);
  cd_jit_exec();
  sys_icache_invalidate(p + 2 * pg, 8);
  rc = mprotect(p + 2 * pg, pg, PROT_READ);
  INFO("C07.ro", "mprotect(page2, R) = %s", rc ? errno_name(errno) : "ok");
  if (rc == 0) {
    cd_jit_write();
    sig = cd_probe_store32((volatile uint32_t *)(p + 2 * pg), 0, &pr);
    INFO("C07.ro", "store to the R page in write mode: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
    cd_jit_exec();
    uint64_t out = 0;
    sig = cd_probe_call(p + 2 * pg, &out, &pr);
    INFO("C07.ro", "call stub in the R page in exec mode: %s out=%llu", cd_probe_str(sig, &pr, sb, sizeof sb),
         (unsigned long long)out);
    rc = mprotect(p + 2 * pg, pg, CD_RWX);
    INFO("C07.ro", "mprotect(page2, RWX) = %s", rc ? errno_name(errno) : "ok");
    cd_jit_write();
    sig = cd_probe_store32((volatile uint32_t *)(p + 2 * pg), A64_MOVZ_X(0, 22), &pr);
    cd_jit_exec();
    sys_icache_invalidate(p + 2 * pg, 8);
    out = 0;
    int sig2 = cd_probe_call(p + 2 * pg, &out, &pr);
    INFO("C07.ro", "restored RWX: write-mode store=%s, exec-mode call out=%llu (sig=%d)", sig ? sig_name(sig) : "ok",
         (unsigned long long)out, sig2);
  }

  /* page 3: RX only: not writable even in write mode */
  rc = mprotect(p + 3 * pg, pg, PROT_READ | PROT_EXEC);
  INFO("C07.rx", "mprotect(page3, RX) = %s", rc ? errno_name(errno) : "ok");
  if (rc == 0) {
    cd_jit_write();
    sig = cd_probe_store32((volatile uint32_t *)(p + 3 * pg), A64_RET, &pr);
    INFO("C07.rx", "store to the RX page in write mode: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
    cd_jit_exec();
    describe_region("C07.rx", "after", p + 3 * pg);
    /* the dance on an RX page: RX -> RW -> RX */
    rc = mprotect(p + 3 * pg, pg, PROT_READ | PROT_WRITE);
    INFO("C07.rx", "mprotect(page3, RW) = %s", rc ? errno_name(errno) : "ok");
    if (rc == 0) {
      cd_emit_ret_const((uint32_t *)(p + 3 * pg), 99);
      rc = mprotect(p + 3 * pg, pg, PROT_READ | PROT_EXEC);
      sys_icache_invalidate(p + 3 * pg, 8);
      uint64_t out = 0;
      sig = cd_probe_call(p + 3 * pg, &out, &pr);
      INFO("C07.rx", "RX -> RW -> write -> RX dance: mprotect=%s, call = %s out=%llu", rc ? errno_name(errno) : "ok",
           cd_probe_str(sig, &pr, sb, sizeof sb), (unsigned long long)out);
    }
  }
  /* vm_protect with set_maximum */
  {
    kern_return_t kr = mach_vm_protect(mach_task_self(), (mach_vm_address_t)p, pg, TRUE, VM_PROT_READ | VM_PROT_EXECUTE);
    INFO("C07.max", "mach_vm_protect(page0, set_maximum, R|X) = %s", kr_name(kr));
    describe_region("C07.max", "after", p);
    kr = mach_vm_protect(mach_task_self(), (mach_vm_address_t)p, pg, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE);
    INFO("C07.max", "mach_vm_protect(page0, current, RWX) after lowering the maximum = %s", kr_name(kr));
  }
  return 0;
}

static void c07_mprotect(void) {
  int to = 0;
  int st = cd_run_child(child_mprotect, NULL, 60000, &to);
  char sb[96];
  INFO("C07.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C08: MAP_FIXED, hints, remap                                               */
/* ------------------------------------------------------------------------- */
static int child_fixed(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  const size_t pg = 16 * KiB;
  cd_probe_result pr;
  char sb[160];
  int sig;

  /* 1. MAP_FIXED|MAP_JIT inside a normal PROT_NONE reservation. */
  char *res = mmap(NULL, 1 * GiB, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (res == MAP_FAILED) { FAIL("C08.setup", "reservation failed"); return 1; }
  void *q = mmap(res + 64 * MiB, 64 * KiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_FIXED, -1, 0);
  INFO("C08.fixed-jit", "MAP_FIXED|MAP_JIT inside a PROT_NONE reservation: %s", q == MAP_FAILED ? errno_name(errno) : "ok");
  if (q != MAP_FAILED) {
    cd_jit_write();
    cd_emit_ret_const(q, 66);
    cd_jit_exec();
    sys_icache_invalidate(q, 8);
    INFO("C08.fixed-jit", "  stub in that fixed region returns %llu", (unsigned long long)((fn0_t)q)());
  }
  munmap(res, 1 * GiB);

  /* 2. Hint address honoured? */
  {
    uintptr_t text = (uintptr_t)&child_fixed;
    uintptr_t hint = (text + 64 * MiB) & ~(uintptr_t)(16 * KiB - 1);
    void *h = mmap((void *)hint, 1 * MiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (h == MAP_FAILED) INFO("C08.hint", "MAP_JIT with hint %#lx (text+64 MiB): %s", hint, errno_name(errno));
    else {
      long dist = (long)((uintptr_t)h - text);
      INFO("C08.hint", "MAP_JIT with hint %#lx (text+64 MiB): got %p, distance from this function %ld MiB (bl reaches +-128 MiB)", hint,
           h, dist / (long)MiB);
      munmap(h, 1 * MiB);
    }
    /* A free hint far away (above 4 GiB). */
    void *h2 = mmap((void *)0x600000000ULL, 1 * MiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    INFO("C08.hint", "MAP_JIT with hint 0x600000000: %s %p", h2 == MAP_FAILED ? errno_name(errno) : "got", h2 == MAP_FAILED ? NULL : h2);
    if (h2 != MAP_FAILED) munmap(h2, 1 * MiB);
    /* Hint below 4 GiB (the pagezero) and in the 32-bit range */
    void *h3 = mmap((void *)0x40000000ULL, 1 * MiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    INFO("C08.hint", "MAP_JIT with hint 0x40000000 (1 GiB, inside the 4 GiB page zero): %s %p", h3 == MAP_FAILED ? errno_name(errno) : "got",
         h3 == MAP_FAILED ? NULL : h3);
    if (h3 != MAP_FAILED) munmap(h3, 1 * MiB);
  }

  /* 2b. Hint honoured for free addresses?  Compare MAP_JIT with a plain anonymous mmap. */
  {
    uintptr_t base = ((uintptr_t)&child_fixed & ~(uintptr_t)(1 * GiB - 1));
    static const unsigned long long hints[] = {0x120000000ULL, 0x180000000ULL, 0x200000000ULL, 0x300000000ULL,
                                               0x1000000000ULL, 0x2000000000ULL, 0x5000000000ULL, 0x6000000000ULL,
                                               0x7000000000ULL, 0x10000000000ULL};
    (void)base;
    for (size_t i = 0; i < sizeof hints / sizeof hints[0]; i++) {
      void *plain = mmap((void *)hints[i], 1 * MiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      void *plain_ok = plain;
      if (plain != MAP_FAILED) munmap(plain, 1 * MiB);
      void *j = mmap((void *)hints[i], 1 * MiB, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
      INFO("C08.hint-matrix", "hint %#llx: plain mmap got %p (%s), MAP_JIT got %p (%s)", hints[i],
           plain_ok == MAP_FAILED ? NULL : plain_ok, plain_ok == (void *)hints[i] ? "honoured" : "moved",
           j == MAP_FAILED ? NULL : j, j == (void *)hints[i] ? "honoured" : "moved");
      if (j != MAP_FAILED) munmap(j, 1 * MiB);
    }
  }

  /* 3. munmap a hole in the middle of a JIT region and re-request it. */
  {
    char *a = cd_jit_map(8 * pg, CD_RWX, 0, NULL);
    if (a) {
      munmap(a + 2 * pg, 2 * pg);
      void *b = mmap(a + 2 * pg, 2 * pg, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
      INFO("C08.hole", "munmap(2 pages) inside a JIT region then mmap(hint=hole, MAP_JIT): %s (%s)",
           b == MAP_FAILED ? errno_name(errno) : "ok", b == (void *)(a + 2 * pg) ? "same address" : "other address");
      if (b != MAP_FAILED && b != (void *)(a + 2 * pg)) munmap(b, 2 * pg);
      munmap(a, 8 * pg);
    }
  }

  /* 4. MAP_FIXED|MAP_ANON (no JIT) over part of a JIT region. */
  {
    char *a = cd_jit_map(8 * pg, CD_RWX, 0, NULL);
    if (a) {
      void *b = mmap(a + 2 * pg, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      INFO("C08.overlay", "MAP_FIXED|MAP_ANON RW (no MAP_JIT) over 2 pages of a JIT region: %s", b == MAP_FAILED ? errno_name(errno) : "ok");
      if (b != MAP_FAILED) {
        describe_region("C08.overlay", "overlaid pages", a + 2 * pg);
        cd_jit_exec();
        sig = cd_probe_store32((volatile uint32_t *)(a + 2 * pg), A64_RET, &pr);
        INFO("C08.overlay", "  store into the overlaid pages in exec mode: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
        cd_jit_write();
        sig = cd_probe_store32((volatile uint32_t *)(a + 2 * pg), A64_RET, &pr);
        INFO("C08.overlay", "  store into the overlaid pages in write mode: %s", cd_probe_str(sig, &pr, sb, sizeof sb));
        cd_jit_exec();
        int rc = mprotect(a + 2 * pg, 2 * pg, CD_RWX);
        INFO("C08.overlay", "  mprotect(overlaid pages, RWX) = %s", rc ? errno_name(errno) : "ok");
        void *c = mmap(a + 2 * pg, 2 * pg, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT | MAP_FIXED, -1, 0);
        INFO("C08.overlay", "  MAP_FIXED|MAP_JIT over them again: %s", c == MAP_FAILED ? errno_name(errno) : "ok");
      }
      munmap(a, 8 * pg);
    }
  }

  /* 5. mach_vm_remap of a JIT region (dual mapping). */
  {
    char *a = cd_jit_map(4 * pg, CD_RWX, 0, NULL);
    if (a) {
      cd_jit_write();
      cd_emit_ret_const((uint32_t *)a, 55);
      cd_jit_exec();
      sys_icache_invalidate(a, 8);
      mach_vm_address_t alias = 0;
      vm_prot_t cur = 0, max = 0;
      kern_return_t kr = mach_vm_remap(mach_task_self(), &alias, 4 * pg, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                       (mach_vm_address_t)a, FALSE, &cur, &max, VM_INHERIT_NONE);
      char b1[4], b2[4];
      INFO("C08.remap", "mach_vm_remap(JIT region -> alias, copy=FALSE) = %s alias=%#llx cur=%s max=%s", kr_name(kr),
           (unsigned long long)alias, prot_str(cur, b1), prot_str(max, b2));
      if (kr == KERN_SUCCESS) {
        describe_region("C08.remap", "alias", (void *)alias);
        kr = mach_vm_protect(mach_task_self(), alias, 4 * pg, FALSE, VM_PROT_READ | VM_PROT_WRITE);
        INFO("C08.remap", "mach_vm_protect(alias, RW) = %s", kr_name(kr));
        if (kr == KERN_SUCCESS) {
          sig = cd_probe_store32((volatile uint32_t *)(alias + 64), A64_MOVZ_X(0, 56), &pr);
          INFO("C08.remap", "store through the RW alias (thread in exec mode): %s", cd_probe_str(sig, &pr, sb, sizeof sb));
          if (sig == 0) {
            cd_emit_ret_const((uint32_t *)(alias + 64) - 0, 56);
            uint32_t seen = *(volatile uint32_t *)(a + 64);
            INFO("C08.remap", "the original region sees %#x at +64 (expected %#x if the pages are shared)", seen, A64_MOVZ_X(0, 56));
            sys_icache_invalidate(a + 64, 8);
            uint64_t out = 0;
            int sg = cd_probe_call(a + 64, &out, &pr);
            INFO("C08.remap", "call through the original JIT region: %s out=%llu", cd_probe_str(sg, &pr, sb, sizeof sb), (unsigned long long)out);
          }
        }
      }
    }
  }
  return 0;
}

static void c08_fixed(void) {
  int to = 0;
  int st = cd_run_child(child_fixed, NULL, 60000, &to);
  char sb[96];
  INFO("C08.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C09: madvise on JIT pages                                                  */
/* ------------------------------------------------------------------------- */
static int child_madvise(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  const size_t pg = 16 * KiB, total = 1 * MiB;
  static const struct { const char *name; int adv; } adv[] = {
      {"MADV_ZERO", MADV_ZERO}, {"MADV_DONTNEED", MADV_DONTNEED}, {"MADV_FREE", MADV_FREE},
      {"MADV_FREE_REUSABLE", MADV_FREE_REUSABLE}, {"MADV_PAGEOUT", MADV_PAGEOUT}};
  for (size_t k = 0; k < sizeof adv / sizeof adv[0]; k++) {
    int err = 0;
    uint8_t *p = cd_jit_map(total, CD_RWX, 0, &err);
    if (!p) { FAIL("C09.setup", "mmap failed"); return 1; }
    cd_jit_write();
    memset(p, 0xa5, total);
    cd_jit_exec();
    uint64_t t0 = now_ns();
    int rc = madvise(p, total, adv[k].adv);
    uint64_t t1 = now_ns();
    int e = errno;
    size_t nz = 0;
    for (size_t i = 0; i < total; i += pg) nz += (p[i] != 0);
    /* Is the memory still usable as JIT memory? */
    cd_jit_write();
    int sig = 0;
    cd_probe_result pr;
    sig = cd_probe_store32((volatile uint32_t *)p, A64_MOVZ_X(0, 5), &pr);
    cd_jit_write();
    if (sig == 0) *(volatile uint32_t *)(p + 4) = A64_RET;
    cd_jit_exec();
    sys_icache_invalidate(p, 8);
    uint64_t out = 0;
    int sig2 = sig ? sig : cd_probe_call(p, &out, &pr);
    INFO("C09.madvise", "%-20s on 1 MiB written with 0xa5: rc=%s, pages still nonzero=%zu/%zu, took %.1f us, afterwards write+exec %s out=%llu",
         adv[k].name, rc ? errno_name(e) : "0", nz, total / pg, (t1 - t0) / 1e3, sig2 ? sig_name(sig2) : "works",
         (unsigned long long)out);
    if (k == 3) {
      rc = madvise(p, total, MADV_FREE_REUSE);
      INFO("C09.madvise", "  MADV_FREE_REUSE after REUSABLE: rc=%s", rc ? errno_name(errno) : "0");
    }
    munmap(p, total);
  }
  /* Comparator: the same advice on a plain RW anonymous mapping. */
  for (size_t k = 0; k < sizeof adv / sizeof adv[0]; k++) {
    uint8_t *p = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(p, 0xa5, total);
    uint64_t t0 = now_ns();
    int rc = madvise(p, total, adv[k].adv);
    uint64_t t1 = now_ns();
    int e = errno;
    size_t nz = 0;
    for (size_t i = 0; i < total; i += pg) nz += (p[i] != 0);
    INFO("C09.plain-anon", "%-20s on a plain RW anonymous 1 MiB: rc=%s, pages still nonzero=%zu/%zu, took %.1f us", adv[k].name,
         rc ? errno_name(e) : "0", nz, total / pg, (t1 - t0) / 1e3);
    munmap(p, total);
  }
  /* mach_vm_behavior_set zero, and mach_vm_deallocate of a range then re-mmap with MAP_JIT at the same hint */
  {
    uint8_t *p = cd_jit_map(total, CD_RWX, 0, NULL);
    cd_jit_write();
    memset(p, 0xa5, total);
    cd_jit_exec();
    munmap(p, total);
    void *q = mmap(p, total, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    INFO("C09.remap-same", "munmap then mmap(hint=same address, MAP_JIT): %s, %s", q == MAP_FAILED ? errno_name(errno) : "ok",
         q == (void *)p ? "same address" : "other address");
    if (q != MAP_FAILED) {
      size_t nz = 0;
      for (size_t i = 0; i < total; i += pg) nz += (((uint8_t *)q)[i] != 0);
      INFO("C09.remap-same", "  fresh mapping nonzero pages=%zu (must be 0)", nz);
      munmap(q, total);
    }
  }
  return 0;
}

static void c09_madvise(void) {
  int to = 0;
  int st = cd_run_child(child_madvise, NULL, 60000, &to);
  char sb[96];
  INFO("C09.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

/* ------------------------------------------------------------------------- */
/* C10: control transfers                                                     */
/* ------------------------------------------------------------------------- */
static int child_calls(void *arg) {
  (void)arg;
  cd_install_probe_handlers();
  const size_t pg = 16 * KiB;
  uint8_t *a = cd_jit_map(pg, CD_RWX, 0, NULL);
  uint8_t *b = cd_jit_map(pg, CD_RWX, 0, NULL);
  if (!a || !b) { FAIL("C10.setup", "mmap MAP_JIT failed"); return 1; }
  long dist = (long)(b - a);
  INFO("C10.layout", "region A=%p region B=%p distance %ld MiB", (void *)a, (void *)b, dist / (long)MiB);

  /* a. JIT -> host function */
  cd_jit_write();
  size_t n = cd_copy_tpl(a, cd_tpl_call_host, cd_tpl_call_host_end);
  size_t off = (size_t)(cd_tpl_call_host_lit - cd_tpl_call_host);
  uint64_t hf = (uint64_t)(uintptr_t)&host_add;
  memcpy(a + off, &hf, 8);
  cd_jit_exec();
  sys_icache_invalidate(a, n);
  uint64_t r = ((uint64_t(*)(uint64_t, uint64_t))a)(3, 4);
  CHECK(r == host_add(3, 4), "C10.jit-to-host", "JIT code calls a host C function through 'ldr x16,lit; blr x16' and returns: %llu", (unsigned long long)r);

  /* b. JIT A -> JIT B with bl (B within range) */
  uint8_t *bl_a = a + 256, *bl_b = b;
  cd_jit_write();
  n = cd_copy_tpl(bl_a, cd_tpl_bl_to, cd_tpl_bl_to_end);
  cd_copy_tpl(bl_b, cd_tpl_ret77, cd_tpl_ret77_end);
  long bl_off = (long)(bl_b - (bl_a + (cd_tpl_bl_to_insn - cd_tpl_bl_to)));
  int in_range = (bl_off > -(128L * 1024 * 1024) && bl_off < 128L * 1024 * 1024);
  if (in_range) {
    uint32_t insn = A64_BL(bl_off);
    memcpy(bl_a + (cd_tpl_bl_to_insn - cd_tpl_bl_to), &insn, 4);
  }
  cd_jit_exec();
  sys_icache_invalidate(bl_a, n);
  sys_icache_invalidate(bl_b, 16);
  if (in_range) {
    uint64_t rr = ((fn0_t)bl_a)();
    CHECK(rr == 78, "C10.bl-between-regions", "bl from region A to region B (offset %ld bytes) and back: returned %llu (77+1 expected)", bl_off, (unsigned long long)rr);
  } else {
    INFO("C10.bl-between-regions", "regions are %ld bytes apart: bl cannot reach, skipped", bl_off);
  }

  /* c. far tail call through a literal to a region 8 GiB away */
  uint8_t *far = mmap((void *)((uintptr_t)a + 8 * GiB), pg, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (far == MAP_FAILED) far = cd_jit_map(pg, CD_RWX, 0, NULL);
  if (far) {
    cd_jit_write();
    n = cd_copy_tpl(far, cd_tpl_ret77, cd_tpl_ret77_end);
    uint8_t *ft = a + 1024;
    size_t m = cd_copy_tpl(ft, cd_tpl_far_tail, cd_tpl_far_tail_end);
    uint64_t target = (uint64_t)(uintptr_t)far;
    memcpy(ft + (cd_tpl_far_tail_lit - cd_tpl_far_tail), &target, 8);
    cd_jit_exec();
    sys_icache_invalidate(far, 16);
    sys_icache_invalidate(ft, m);
    uint64_t rr = ((fn0_t)ft)();
    CHECK(rr == 77, "C10.far-tail", "ldr x16,lit; br x16 from A into a region %ld GiB away: returned %llu", (long)(((long)(far - a)) / (long)GiB), (unsigned long long)rr);
  }

  /* d. host -> JIT -> host -> JIT nesting, callee-saved registers preserved */
  {
    cd_jit_write();
    uint8_t *add1 = a + 2048;
    cd_copy_tpl(add1, cd_tpl_addone, cd_tpl_addone_end);
    cd_jit_exec();
    sys_icache_invalidate(add1, 16);
    uint64_t acc = 0;
    for (int i = 0; i < 1000; i++) acc = ((uint64_t(*)(uint64_t))add1)(acc);
    CHECK(acc == 1000, "C10.loop-calls", "1000 host->JIT calls through a function pointer: %llu", (unsigned long long)acc);
  }

  /* e. a branch landing in the middle of a stub without a BTI landing pad */
  {
    cd_jit_write();
    uint32_t *s = (uint32_t *)(a + 4096);
    s[0] = A64_MOVZ_X(0, 1);
    s[1] = A64_MOVZ_X(0, 2); /* land here via br */
    s[2] = A64_RET;
    cd_jit_exec();
    sys_icache_invalidate(s, 12);
    uint64_t out = 0;
    cd_probe_result pr;
    char sb[160];
    int sig = cd_probe_call(&s[1], &out, &pr);
    INFO("C10.bti", "indirect call (blr) to a JIT address that has no BTI landing pad: %s out=%llu", cd_probe_str(sig, &pr, sb, sizeof sb), (unsigned long long)out);
  }
  return 0;
}

static void c10_calls(void) {
  int to = 0;
  int st = cd_run_child(child_calls, NULL, 60000, &to);
  char sb[96];
  INFO("C10.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
}

int main(int argc, char **argv) {
  (void)argc;
  dm_arm_watchdog(200);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
#define RUN(tag, fn) do { if (!only || strstr(only, tag)) fn; } while (0)
  c01_environment(argv[0]);
  RUN("C02", c02_basic());
  RUN("C03", c03_icache());
  RUN("C04", c04_nojit());
  RUN("C05", c05_sizes());
  RUN("C06", c06_counts());
  RUN("C07", c07_mprotect());
  RUN("C08", c08_fixed());
  RUN("C09", c09_madvise());
  RUN("C10", c10_calls());
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
