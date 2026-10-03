/*
 * d_faults.c - fact group D (faults and signals): what the kernel reports for
 * each kind of fault.  Every case runs in its own forked child, so a kill by the
 * kernel is a measured result.  The handler records signo, si_code, si_addr,
 * and from the ucontext: pc, FAR, ESR, then resumes the thread by editing the pc
 * in the ucontext (skip the faulting instruction, or return to the caller).
 *
 *   F01 unmapped / PROT_NONE / read-only / no-exec / JIT-state faults
 *   F02 non-canonical addresses
 *   F03 file mappings (beyond EOF, read-only shared)
 *   F04 alignment faults (exclusives, LSE atomics, plain accesses)
 *   F05 accesses that straddle a page boundary (fault address, partial effect)
 *   F06 instruction faults (brk, udf, privileged system register)
 *   F07 floating point exception trapping, top-byte-ignore
 *   F08 stack overflow with and without sigaltstack
 */
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)
#define PG (16 * KiB)

__asm__(
    ".text\n"
    ".arch_extension lse\n"
    ".p2align 4\n"
    ".globl _cd_load64\n"
    "_cd_load64:\n"
    "  ldr x0, [x0]\n"
    "  ret\n"
    ".globl _cd_store64\n"
    "_cd_store64:\n"          /* x0 = address, x1 = value */
    "  str x1, [x0]\n"
    "  ret\n"
    ".globl _cd_store32\n"
    "_cd_store32:\n"
    "  str w1, [x0]\n"
    "  ret\n"
    ".globl _cd_jump\n"
    "_cd_jump:\n"
    "  br x0\n"
    ".globl _cd_ldaxr\n"
    "_cd_ldaxr:\n"
    "  ldaxr x0, [x0]\n"
    "  ret\n"
    ".globl _cd_ldxr32\n"
    "_cd_ldxr32:\n"
    "  ldxr w0, [x0]\n"
    "  ret\n"
    ".globl _cd_ldar\n"
    "_cd_ldar:\n"
    "  ldar x0, [x0]\n"
    "  ret\n"
    ".globl _cd_stlr\n"
    "_cd_stlr:\n"
    "  stlr x1, [x0]\n"
    "  ret\n"
    ".globl _cd_ldaddal\n"
    "_cd_ldaddal:\n"
    "  ldaddal x1, x2, [x0]\n"
    "  mov x0, x2\n"
    "  ret\n"
    ".globl _cd_casal\n"
    "_cd_casal:\n"
    "  mov x3, x1\n"
    "  casal x1, x2, [x0]\n"
    "  mov x0, x1\n"
    "  ret\n"
    ".globl _cd_swpal\n"
    "_cd_swpal:\n"
    "  swpal x1, x2, [x0]\n"
    "  mov x0, x2\n"
    "  ret\n"
    ".globl _cd_ldp\n"
    "_cd_ldp:\n"
    "  ldp x2, x3, [x0]\n"
    "  mov x0, x2\n"
    "  ret\n"
    ".globl _cd_ldr_q\n"
    "_cd_ldr_q:\n"
    "  ldr q0, [x0]\n"
    "  ret\n"
    ".globl _cd_brk\n"
    "_cd_brk:\n"
    "  brk #0\n"
    "  ret\n"
    ".globl _cd_udf\n"
    "_cd_udf:\n"
    "  .inst 0x00000000\n"
    "  ret\n"
    ".globl _cd_mrs_ttbr0\n"
    "_cd_mrs_ttbr0:\n"
    "  mrs x0, ttbr0_el1\n"
    "  ret\n"
    ".globl _cd_hvc\n"
    "_cd_hvc:\n"
    "  hvc #0\n"
    "  ret\n"
    ".globl _cd_msr_daif\n"
    "_cd_msr_daif:\n"
    "  msr daifset, #2\n"
    "  ret\n"
    ".globl _cd_fp_div_trap\n"
    "_cd_fp_div_trap:\n"          /* x0 = FPCR trap bits to OR in; performs 1.0/0.0 then restores FPCR */
    "  mrs x9, fpcr\n"
    "  orr x10, x9, x0\n"
    "  msr fpcr, x10\n"
    "  fmov d0, #1.0\n"
    "  fmov d1, xzr\n"
    "  fdiv d2, d0, d1\n"
    "  nop\n"
    "  msr fpcr, x9\n"
    "  ret\n"
    ".globl _cd_fp_inexact_trap\n"
    "_cd_fp_inexact_trap:\n"     /* 1.0/3.0 is inexact */
    "  mrs x9, fpcr\n"
    "  orr x10, x9, x0\n"
    "  msr fpcr, x10\n"
    "  fmov d0, #1.0\n"
    "  fmov d1, #3.0\n"
    "  fdiv d2, d0, d1\n"
    "  nop\n"
    "  msr fpcr, x9\n"
    "  ret\n"
    ".globl _cd_sp_misalign\n"
    "_cd_sp_misalign:\n"       /* make sp misaligned (x0 = misaligned value), load through it, restore */
    "  mov x9, sp\n"
    "  mov sp, x0\n"
    "  ldr x0, [sp]\n"
    "  mov sp, x9\n"
    "  ret\n");

extern uint64_t cd_load64(uint64_t), cd_store64(uint64_t, uint64_t), cd_store32(uint64_t, uint64_t),
    cd_jump(uint64_t), cd_ldaxr(uint64_t), cd_ldxr32(uint64_t), cd_ldar(uint64_t), cd_stlr(uint64_t, uint64_t),
    cd_ldaddal(uint64_t, uint64_t), cd_casal(uint64_t, uint64_t, uint64_t), cd_swpal(uint64_t, uint64_t),
    cd_ldp(uint64_t), cd_ldr_q(uint64_t), cd_brk(void), cd_udf(void), cd_mrs_ttbr0(void), cd_hvc(void),
    cd_msr_daif(void), cd_sp_misalign(uint64_t), cd_fp_div_trap(uint64_t), cd_fp_inexact_trap(uint64_t);

enum { ACT_SKIP = 1, ACT_RET = 2 };

typedef struct {
  volatile int count;
  volatile int sig, code;
  void *volatile addr;
  volatile uint64_t pc, far, lr;
  volatile uint32_t esr, exc;
  volatile uintptr_t hsp;
  volatile int on_alt;
} rec_t;

static rec_t R;
static volatile int g_act = ACT_SKIP;
static char *g_alt;
static size_t g_alt_size;

static void rec_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  R.count++;
  R.sig = sig;
  R.code = si->si_code;
  R.addr = si->si_addr;
  R.pc = CD_PC(uc);
  R.far = CD_FAR(uc);
  R.lr = CD_LR(uc);
  R.esr = CD_ESR(uc);
  R.exc = CD_EXC(uc);
  int local;
  R.hsp = (uintptr_t)&local;
  R.on_alt = g_alt && (char *)&local >= g_alt && (char *)&local < g_alt + g_alt_size;
  if (R.count > 8) _exit(96); /* runaway */
  switch (g_act) {
  case ACT_SKIP: CD_PC(uc) += 4; break;
  case ACT_RET: CD_PC(uc) = CD_LR(uc); break;
  default: break;
  }
}

static void install(int use_alt) {
  if (use_alt) {
    g_alt_size = 256 * KiB;
    g_alt = malloc(g_alt_size);
    stack_t ss = {.ss_sp = g_alt, .ss_size = g_alt_size, .ss_flags = 0};
    sigaltstack(&ss, NULL);
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = rec_handler;
  sa.sa_flags = SA_SIGINFO | (use_alt ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGSYS, SIGEMT};
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

static void clear_rec(void) { memset((void *)&R, 0, sizeof R); }

static void report(const char *id, const char *what, uint64_t target_addr, uint64_t insn_addr) {
  char eb[128];
  if (R.count == 0) {
    INFO(id, "%-46s -> no signal", what);
    return;
  }
  INFO(id, "%-46s -> %s code=%s(%d) si_addr=%#llx far=%#llx pc=%s lr=%s [%s] exception=%u%s", what, sig_name(R.sig),
       si_code_name(R.sig, R.code), R.code, (unsigned long long)(uintptr_t)R.addr, (unsigned long long)R.far,
       R.pc == insn_addr ? "insn" : (R.pc == target_addr ? "target" : "other"), R.lr ? "set" : "0",
       cd_esr_describe(R.esr, eb, sizeof eb), R.exc, R.count > 1 ? " (MULTIPLE SIGNALS)" : "");
  if (R.addr != (void *)(uintptr_t)target_addr)
    INFO(id, "    note: si_addr %#llx differs from the intended address %#llx (difference %lld)", (unsigned long long)(uintptr_t)R.addr,
         (unsigned long long)target_addr, (long long)((uintptr_t)R.addr - target_addr));
}

static void *page(int prot) {
  void *p = mmap(NULL, PG, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}
static void *unmapped_page(void) {
  void *p = page(PROT_READ | PROT_WRITE);
  if (p) munmap(p, PG);
  return p;
}

/* ------------------------------------------------------------------------- */
typedef int (*case_fn)(void *);
typedef struct { const char *name; case_fn fn; } case_t;

static int c_unmapped(void *a) {
  (void)a;
  install(1);
  uint64_t p = (uint64_t)(uintptr_t)unmapped_page();
  clear_rec(); g_act = ACT_SKIP; cd_load64(p);
  report("F01.unmapped", "read of an unmapped page", p, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_store64(p, 1);
  report("F01.unmapped", "write to an unmapped page", p, (uint64_t)(uintptr_t)&cd_store64);
  clear_rec(); g_act = ACT_RET; cd_jump(p);
  report("F01.unmapped", "jump to an unmapped page", p, (uint64_t)(uintptr_t)&cd_jump);
  clear_rec(); g_act = ACT_SKIP; cd_load64(0);
  report("F01.unmapped", "read of address 0 (page zero)", 0, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_load64(0x1000);
  report("F01.unmapped", "read of address 0x1000 (page zero)", 0x1000, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_load64(0xffffffffULL);
  report("F01.unmapped", "read of address 0xffffffff (page zero, last)", 0xffffffffULL, (uint64_t)(uintptr_t)&cd_load64);
  return 0;
}

static int c_prot_none(void *a) {
  (void)a;
  install(1);
  uint64_t p = (uint64_t)(uintptr_t)page(PROT_NONE);
  clear_rec(); g_act = ACT_SKIP; cd_load64(p);
  report("F01.none", "read of a PROT_NONE page", p, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_store64(p, 1);
  report("F01.none", "write to a PROT_NONE page", p, (uint64_t)(uintptr_t)&cd_store64);
  clear_rec(); g_act = ACT_RET; cd_jump(p);
  report("F01.none", "jump to a PROT_NONE page", p, (uint64_t)(uintptr_t)&cd_jump);
  return 0;
}

static int c_readonly(void *a) {
  (void)a;
  install(1);
  uint8_t *p = page(PROT_READ | PROT_WRITE);
  memset(p, 0x5a, PG);
  mprotect(p, PG, PROT_READ);
  clear_rec(); g_act = ACT_SKIP; cd_store64((uint64_t)(uintptr_t)p + 24, 1);
  report("F01.ro", "write to a PROT_READ page", (uint64_t)(uintptr_t)p + 24, (uint64_t)(uintptr_t)&cd_store64);
  uint64_t v = cd_load64((uint64_t)(uintptr_t)p + 24);
  INFO("F01.ro", "read of the same page works (%#llx)", (unsigned long long)v);
  clear_rec(); g_act = ACT_SKIP; cd_store32((uint64_t)(uintptr_t)p + PG - 4, 1);
  report("F01.ro", "32-bit write to the last word of a PROT_READ page", (uint64_t)(uintptr_t)p + PG - 4, (uint64_t)(uintptr_t)&cd_store32);
  return 0;
}

static int c_noexec(void *a) {
  (void)a;
  install(1);
  /* RW page containing a valid 'ret': jump to it */
  uint32_t *p = page(PROT_READ | PROT_WRITE);
  p[0] = A64_RET;
  sys_icache_invalidate(p, 4);
  clear_rec(); g_act = ACT_RET; cd_jump((uint64_t)(uintptr_t)p);
  report("F01.noexec", "jump to a valid 'ret' in a PROT_READ|PROT_WRITE page", (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)&cd_jump);
  /* R page with ret */
  mprotect(p, PG, PROT_READ);
  clear_rec(); g_act = ACT_RET; cd_jump((uint64_t)(uintptr_t)p);
  report("F01.noexec", "jump to a valid 'ret' in a PROT_READ page", (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)&cd_jump);
  /* RX page: write */
  uint32_t *q = page(PROT_READ | PROT_WRITE);
  q[0] = A64_RET;
  mprotect(q, PG, PROT_READ | PROT_EXEC);
  clear_rec(); g_act = ACT_SKIP; cd_store32((uint64_t)(uintptr_t)q, 0);
  report("F01.noexec", "write to a PROT_READ|PROT_EXEC page", (uint64_t)(uintptr_t)q, (uint64_t)(uintptr_t)&cd_store32);
  return 0;
}

static int c_jit_state(void *a) {
  (void)a;
  install(1);
  uint32_t *p = mmap(NULL, PG, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (p == MAP_FAILED) { INFO("F01.jit", "MAP_JIT unavailable here: %s", errno_name(errno)); return 0; }
  cd_jit_write();
  p[0] = A64_RET;
  p[1] = A64_RET;
  cd_jit_exec();
  sys_icache_invalidate(p, 8);
  clear_rec(); g_act = ACT_SKIP; cd_store32((uint64_t)(uintptr_t)p + 64, 0);
  report("F01.jit", "write to a MAP_JIT page in exec mode", (uint64_t)(uintptr_t)p + 64, (uint64_t)(uintptr_t)&cd_store32);
  cd_jit_write();
  clear_rec(); g_act = ACT_RET; cd_jump((uint64_t)(uintptr_t)p);
  report("F01.jit", "jump into a MAP_JIT page in write mode", (uint64_t)(uintptr_t)p, (uint64_t)(uintptr_t)&cd_jump);
  cd_jit_exec();
  return 0;
}

static int c_noncanonical(void *a) {
  (void)a;
  install(1);
  static const uint64_t addrs[] = {0x0000800000000000ULL, 0x0001000000000000ULL, 0x7fffffffffffULL + 1,
                                   0xffff000000000000ULL, 0xfffffff000000000ULL, 0x8000000000000000ULL,
                                   0xffffffffffffffffULL, 0x0000700000000000ULL};
  for (size_t i = 0; i < sizeof addrs / sizeof addrs[0]; i++) {
    char w[64];
    snprintf(w, sizeof w, "read of %#llx", (unsigned long long)addrs[i]);
    clear_rec(); g_act = ACT_SKIP; cd_load64(addrs[i]);
    report("F02.noncanonical", w, addrs[i], (uint64_t)(uintptr_t)&cd_load64);
  }
  clear_rec(); g_act = ACT_RET; cd_jump(0x0001000000000000ULL);
  report("F02.noncanonical", "jump to 0x0001000000000000", 0x0001000000000000ULL, (uint64_t)(uintptr_t)&cd_jump);
  return 0;
}

static int c_files(void *a) {
  (void)a;
  install(1);
  char path[PATH_MAX];
  const char *td = getenv("TMPDIR");
  snprintf(path, sizeof path, "%s/cd_f03_%d.bin", td ? td : "/private/tmp", (int)getpid());
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return 1;
  ftruncate(fd, 2 * PG);
  uint8_t *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  p[0] = 1;
  p[PG] = 2;
  ftruncate(fd, PG / 2); /* second page is beyond EOF now; first page is partly beyond EOF */
  clear_rec(); g_act = ACT_SKIP; cd_load64((uint64_t)(uintptr_t)p + PG + 8);
  report("F03.file", "read of a MAP_SHARED page entirely beyond EOF", (uint64_t)(uintptr_t)p + PG + 8, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_load64((uint64_t)(uintptr_t)p + PG / 2 + 8);
  report("F03.file", "read of the part of the first page beyond EOF", (uint64_t)(uintptr_t)p + PG / 2 + 8, (uint64_t)(uintptr_t)&cd_load64);
  clear_rec(); g_act = ACT_SKIP; cd_store64((uint64_t)(uintptr_t)p + PG + 8, 7);
  report("F03.file", "write to a MAP_SHARED page entirely beyond EOF", (uint64_t)(uintptr_t)p + PG + 8, (uint64_t)(uintptr_t)&cd_store64);
  munmap(p, 2 * PG);
  /* read-only shared mapping, write */
  ftruncate(fd, PG);
  uint8_t *q = mmap(NULL, PG, PROT_READ, MAP_SHARED, fd, 0);
  clear_rec(); g_act = ACT_SKIP; cd_store64((uint64_t)(uintptr_t)q + 8, 7);
  report("F03.file", "write to a PROT_READ MAP_SHARED file page", (uint64_t)(uintptr_t)q + 8, (uint64_t)(uintptr_t)&cd_store64);
  /* file closed behind the mapping: still fine; file deleted: still fine.  truncate to 0 while mapped then read */
  ftruncate(fd, 0);
  clear_rec(); g_act = ACT_SKIP; cd_load64((uint64_t)(uintptr_t)q + 8);
  report("F03.file", "read after the file was truncated to 0", (uint64_t)(uintptr_t)q + 8, (uint64_t)(uintptr_t)&cd_load64);
  unlink(path);
  return 0;
}

static int c_align(void *a) {
  (void)a;
  install(1);
  uint8_t *p = page(PROT_READ | PROT_WRITE);
  memset(p, 0, PG);
  uint64_t b = (uint64_t)(uintptr_t)p;
  struct { const char *n; uint64_t addr; uint64_t (*f1)(uint64_t); int kind; } t[] = {
      {"plain ldr x, +1 (inside a 16-byte window)", b + 1, cd_load64, 0},
      {"plain ldr x, +9 (crosses a 16-byte boundary)", b + 9, cd_load64, 0},
      {"plain ldr x, +60 (crosses a 64-byte line)", b + 60, cd_load64, 0},
      {"plain ldr x, +124 (crosses a 128-byte line)", b + 124, cd_load64, 0},
      {"ldp x, x, +4 (16-byte pair, offset 4)", b + 4, cd_ldp, 0},
      {"ldr q, +4", b + 4, cd_ldr_q, 0},
      {"ldar x, +1", b + 1, cd_ldar, 0},
      {"ldar x, +9", b + 9, cd_ldar, 0},
      {"ldaxr x, +1", b + 1, cd_ldaxr, 0},
      {"ldaxr x, +4", b + 4, cd_ldaxr, 0},
      {"ldaxr x, +9", b + 9, cd_ldaxr, 0},
      {"ldxr w, +1", b + 1, cd_ldxr32, 0},
      {"ldxr w, +2", b + 2, cd_ldxr32, 0},
  };
  for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
    clear_rec(); g_act = ACT_SKIP;
    t[i].f1(t[i].addr);
    report("F04.align", t[i].n, t[i].addr, 0);
  }
  struct { const char *n; uint64_t addr; int k; } at[] = {
      {"ldaddal x, +4 (inside a 16-byte window)", b + 4, 0}, {"ldaddal x, +8 (aligned 8)", b + 8, 0},
      {"ldaddal x, +9 (inside window, odd)", b + 9, 0},      {"ldaddal x, +12 (crosses 16-byte boundary)", b + 12, 0},
      {"ldaddal x, +60 (crosses 64-byte line)", b + 60, 0},  {"ldaddal x, +124 (crosses 128-byte line)", b + 124, 0},
      {"swpal x, +12 (crosses 16-byte boundary)", b + 12, 1}, {"casal x, +12 (crosses 16-byte boundary)", b + 12, 2},
      {"casal x, +4 (inside window)", b + 4, 2}};
  for (size_t i = 0; i < sizeof at / sizeof at[0]; i++) {
    clear_rec(); g_act = ACT_SKIP;
    if (at[i].k == 0) cd_ldaddal(at[i].addr, 1);
    else if (at[i].k == 1) cd_swpal(at[i].addr, 1);
    else cd_casal(at[i].addr, 0, 1);
    report("F04.align", at[i].n, at[i].addr, 0);
  }
  clear_rec(); g_act = ACT_SKIP; cd_stlr(b + 1, 1);
  report("F04.align", "stlr x, +1", b + 1, 0);
  clear_rec(); g_act = ACT_SKIP; cd_stlr(b + 9, 1);
  report("F04.align", "stlr x, +9", b + 9, 0);
  /* pc and sp alignment */
  uint32_t *c = mmap(NULL, PG, CD_RWX, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
  if (c != MAP_FAILED) {
    cd_jit_write(); c[0] = A64_RET; c[1] = A64_RET; cd_jit_exec(); sys_icache_invalidate(c, 8);
    clear_rec(); g_act = ACT_RET; cd_jump((uint64_t)(uintptr_t)c + 2);
    report("F04.align", "jump to a pc that is 2 mod 4", (uint64_t)(uintptr_t)c + 2, (uint64_t)(uintptr_t)&cd_jump);
  }
  clear_rec(); g_act = ACT_SKIP; cd_sp_misalign(b + 8 + 0x100 - 8 + 1);
  report("F04.align", "ldr [sp] with sp misaligned (sp = 8 mod 16... +1)", b + 0x101, 0);
  return 0;
}

static int c_straddle(void *a) {
  (void)a;
  install(1);
  /* two pages: first RW, second PROT_NONE / unmapped / read-only */
  for (int variant = 0; variant < 3; variant++) {
    uint8_t *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(p, 0xab, 2 * PG);
    const char *vn = variant == 0 ? "second page PROT_NONE" : (variant == 1 ? "second page unmapped" : "second page PROT_READ");
    if (variant == 0) mprotect(p + PG, PG, PROT_NONE);
    else if (variant == 1) munmap(p + PG, PG);
    else mprotect(p + PG, PG, PROT_READ);
    uint64_t edge = (uint64_t)(uintptr_t)p + PG;
    char w[96];
    clear_rec(); g_act = ACT_SKIP;
    cd_load64(edge - 4);
    snprintf(w, sizeof w, "8-byte load at page end - 4, %s", vn);
    report("F05.straddle", w, edge - 4, (uint64_t)(uintptr_t)&cd_load64);
    INFO("F05.straddle", "    (fault address relative to the page boundary: %lld bytes)", (long long)((uintptr_t)R.addr - edge));
    clear_rec(); g_act = ACT_SKIP;
    cd_ldr_q(edge - 8);
    snprintf(w, sizeof w, "16-byte vector load at page end - 8, %s", vn);
    report("F05.straddle", w, edge - 8, (uint64_t)(uintptr_t)&cd_ldr_q);
    INFO("F05.straddle", "    (fault address relative to the page boundary: %lld bytes)", (long long)((uintptr_t)R.addr - edge));
    /* stores: does the part inside the first page get written? */
    memset(p, 0xab, PG);
    clear_rec(); g_act = ACT_SKIP;
    cd_store64(edge - 4, 0x1122334455667788ULL);
    uint32_t first_part = *(volatile uint32_t *)(p + PG - 4);
    snprintf(w, sizeof w, "8-byte store at page end - 4, %s", vn);
    report("F05.straddle", w, edge - 4, (uint64_t)(uintptr_t)&cd_store64);
    INFO("F05.straddle", "    after the fault the 4 bytes in the first page are %#x (0xabababab = unchanged, 0x55667788 = partial store happened)",
         first_part);
  }
  /* unaligned LSE atomic that straddles a page boundary */
  {
    uint8_t *p = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mprotect(p + PG, PG, PROT_NONE);
    uint64_t edge = (uint64_t)(uintptr_t)p + PG;
    clear_rec(); g_act = ACT_SKIP;
    cd_ldaddal(edge - 4, 1);
    report("F05.straddle", "ldaddal x at page end - 4 (misaligned + straddle)", edge - 4, 0);
    INFO("F05.straddle", "    (fault address relative to the page boundary: %lld bytes)", (long long)((uintptr_t)R.addr - edge));
  }
  return 0;
}

static int c_insn(void *a) {
  (void)a;
  install(1);
  clear_rec(); g_act = ACT_SKIP; cd_brk();
  report("F06.insn", "brk #0", (uint64_t)(uintptr_t)&cd_brk, (uint64_t)(uintptr_t)&cd_brk);
  clear_rec(); g_act = ACT_SKIP; cd_udf();
  report("F06.insn", "udf #0 (permanently undefined)", (uint64_t)(uintptr_t)&cd_udf, (uint64_t)(uintptr_t)&cd_udf);
  clear_rec(); g_act = ACT_SKIP; cd_mrs_ttbr0();
  report("F06.insn", "mrs x0, ttbr0_el1 (privileged)", (uint64_t)(uintptr_t)&cd_mrs_ttbr0, (uint64_t)(uintptr_t)&cd_mrs_ttbr0);
  clear_rec(); g_act = ACT_SKIP; cd_hvc();
  report("F06.insn", "hvc #0", (uint64_t)(uintptr_t)&cd_hvc, (uint64_t)(uintptr_t)&cd_hvc);
  clear_rec(); g_act = ACT_SKIP; cd_msr_daif();
  report("F06.insn", "msr daifset, #2 (privileged)", (uint64_t)(uintptr_t)&cd_msr_daif, (uint64_t)(uintptr_t)&cd_msr_daif);
  return 0;
}

static int c_fp(void *a) {
  (void)a;
  install(1);
  uint64_t fpcr_before, fpcr_after;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_before));
  /* which FPCR trap-enable bits read back as written? IOE(8) DZE(9) OFE(10) UFE(11) IXE(12) IDE(15) */
  uint64_t want = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 15);
  __asm__ volatile("msr fpcr, %0" ::"r"(fpcr_before | want));
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr_after));
  __asm__ volatile("msr fpcr, %0" ::"r"(fpcr_before));
  INFO("F07.fp", "FPCR before=%#llx; after writing IOE|DZE|OFE|UFE|IXE|IDE=%#llx (bits that read back as written: %#llx)",
       (unsigned long long)fpcr_before, (unsigned long long)fpcr_after, (unsigned long long)(fpcr_after & want));
  clear_rec(); g_act = ACT_SKIP;
  cd_fp_div_trap(1u << 9);
  report("F07.fp", "1.0/0.0 with FPCR.DZE set", (uint64_t)(uintptr_t)&cd_fp_div_trap, 0);
  clear_rec(); g_act = ACT_SKIP;
  cd_fp_inexact_trap(1u << 12);
  report("F07.fp", "1.0/3.0 with FPCR.IXE set", (uint64_t)(uintptr_t)&cd_fp_inexact_trap, 0);
  clear_rec(); g_act = ACT_SKIP;
  cd_fp_div_trap(0);
  report("F07.fp", "1.0/0.0 with all FP traps disabled", (uint64_t)(uintptr_t)&cd_fp_div_trap, 0);
  /* integer division by zero */
  volatile long iz = 0, in = 5;
  clear_rec();
  volatile long q = in / iz;
  INFO("F07.fp", "integer 5 / 0 on arm64 = %ld, signals received %d (sdiv does not trap)", q, R.count);
  /* top-byte-ignore */
  uint8_t *p = page(PROT_READ | PROT_WRITE);
  *(volatile uint64_t *)p = 0x1122334455667788ULL;
  uint64_t tagged = (uint64_t)(uintptr_t)p | (0xabULL << 56);
  clear_rec(); g_act = ACT_SKIP;
  uint64_t v = cd_load64(tagged);
  if (R.count == 0) INFO("F07.tbi", "load through a pointer with top byte 0xab: no fault, value %#llx (top-byte-ignore is active for data accesses)", (unsigned long long)v);
  else report("F07.tbi", "load through a pointer with top byte 0xab", tagged, (uint64_t)(uintptr_t)&cd_load64);
  return 0;
}

static volatile uint64_t g_sink;
static uint64_t __attribute__((noinline)) recurse(uint64_t n) {
  volatile char buf[4096];
  buf[0] = (char)n;
  g_sink += buf[0];
  return recurse(n + 1) + buf[0];
}

static void so_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  char m[300], eb[128];
  int local;
  int n = snprintf(m, sizeof m, "INFO F08.stack %s code=%s(%d) si_addr=%p far=%#llx sp_at_fault=%#llx handler_local=%p on_alt=%d [%s]\n",
                   sig_name(sig), si_code_name(sig, si->si_code), si->si_code, si->si_addr, (unsigned long long)CD_FAR(uc),
                   (unsigned long long)CD_SP(uc), (void *)&local,
                   g_alt && (char *)&local >= g_alt && (char *)&local < g_alt + g_alt_size, cd_esr_describe(CD_ESR(uc), eb, sizeof eb));
  (void)!write(1, m, (size_t)n);
  _exit(0);
}

static int c_stack(void *arg) {
  int mode = (int)(intptr_t)arg; /* 0 = no altstack, 1 = altstack */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = so_handler;
  sa.sa_flags = SA_SIGINFO | (mode ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  if (mode) {
    g_alt_size = 128 * KiB;
    g_alt = malloc(g_alt_size);
    stack_t ss = {.ss_sp = g_alt, .ss_size = g_alt_size, .ss_flags = 0};
    sigaltstack(&ss, NULL);
  }
  recurse(0);
  return 3;
}

static void *stack_thread(void *arg) {
  c_stack(arg);
  return NULL;
}

static int c_stack_thread(void *arg) {
  int mode = (int)(intptr_t)arg;
  pthread_t th;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 1 * MiB);
  /* the sigaltstack is per thread: c_stack sets it up inside the thread */
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = so_handler;
  sa.sa_flags = SA_SIGINFO | (mode ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  pthread_create(&th, &at, stack_thread, arg);
  pthread_join(th, NULL);
  return 4;
}

int main(int argc, char **argv) {
  (void)argc;
  dm_arm_watchdog(200);
  setvbuf(stdout, NULL, _IOLBF, 0);
  cd_print_header(argv[0]);
  static const case_t cases[] = {
      {"unmapped", c_unmapped},     {"prot_none", c_prot_none}, {"readonly", c_readonly}, {"noexec", c_noexec},
      {"jit_state", c_jit_state},   {"noncanonical", c_noncanonical}, {"files", c_files},     {"align", c_align},
      {"straddle", c_straddle},     {"insn", c_insn},           {"fp", c_fp}};
  const char *only = argc > 1 ? argv[1] : NULL;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    if (only && !strstr(cases[i].name, only)) continue;
    int to = 0;
    int st = cd_run_child(cases[i].fn, NULL, 20000, &to);
    char sb[96];
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st) != 0)
      INFO("F00.child", "case %s ended: %s", cases[i].name, cd_status_str(st, to, sb, sizeof sb));
  }
  if (!only || strstr("stack", only)) {
    for (int mode = 0; mode < 2; mode++) {
      int to = 0;
      int st = cd_run_child(c_stack, (void *)(intptr_t)mode, 20000, &to);
      char sb[96];
      INFO("F08.stack", "main-thread stack overflow, %s: child %s", mode ? "SA_ONSTACK + sigaltstack" : "handler on the overflowing stack",
           cd_status_str(st, to, sb, sizeof sb));
      st = cd_run_child(c_stack_thread, (void *)(intptr_t)mode, 20000, &to);
      INFO("F08.stack", "pthread stack overflow (1 MiB stack), %s: child %s", mode ? "SA_ONSTACK + sigaltstack" : "handler on the overflowing stack",
           cd_status_str(st, to, sb, sizeof sb));
    }
  }
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
