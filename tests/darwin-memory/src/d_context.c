/*
 * d_context.c - fact group D (faults and signals): the arm64 signal context.
 *
 *   X01 layout of siginfo_t, ucontext_t and the arm64 mcontext (sizes, offsets)
 *   X02 geometry of a signal frame on the normal stack and on a sigaltstack
 *   X03 reading the whole register file from the handler (x0..x28, fp, lr, sp, pc,
 *       cpsr, v0..v31, fpsr, fpcr)
 *   X04 writing the whole register file from the handler and resuming at another pc
 *       with another sp
 *   X05 x18 (the reserved platform register): does it survive signals, syscalls
 *       and preemption?
 *   X06 sigaltstack API facts
 */
#include "cd_common.h"

#define KiB (1ULL << 10)
#define MiB (1ULL << 20)

/* ------------------------------------------------------------------------- */
/* Assembly: trigger with a known register file, and the resume target.       */
/* ------------------------------------------------------------------------- */
uint64_t cd_ctx_pat[32];          /* patterns for x0..x30 (index 28 unused: x28 = capture buffer) */
uint64_t cd_ctx_save[24];         /* callee-saved registers, sp, lr, x18, d8-d15 */
uint8_t cd_ctx_q_pat[32][16] __attribute__((aligned(16)));
uint64_t cd_ctx_nzcv_pat, cd_ctx_fpsr_pat, cd_ctx_fpcr_pat;
uint64_t *cd_ctx_save_ptr;

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl _cd_ctx_trigger\n"
    "_cd_ctx_trigger:\n"                  /* x0 = capture buffer */
    "  adrp x9, _cd_ctx_save@PAGE\n"
    "  add x9, x9, _cd_ctx_save@PAGEOFF\n"
    "  stp x19, x20, [x9, #0]\n"
    "  stp x21, x22, [x9, #16]\n"
    "  stp x23, x24, [x9, #32]\n"
    "  stp x25, x26, [x9, #48]\n"
    "  stp x27, x28, [x9, #64]\n"
    "  stp x29, x30, [x9, #80]\n"
    "  mov x10, sp\n"
    "  str x10, [x9, #96]\n"
    "  stp d8, d9, [x9, #104]\n"
    "  stp d10, d11, [x9, #120]\n"
    "  stp d12, d13, [x9, #136]\n"
    "  stp d14, d15, [x9, #152]\n"
    "  str x18, [x9, #168]\n"
    "  mov x28, x0\n"                      /* capture buffer, kept in x28 */
    /* vector registers */
    "  adrp x9, _cd_ctx_q_pat@PAGE\n"
    "  add x9, x9, _cd_ctx_q_pat@PAGEOFF\n"
    "  ldp q0, q1, [x9, #0]\n"
    "  ldp q2, q3, [x9, #32]\n"
    "  ldp q4, q5, [x9, #64]\n"
    "  ldp q6, q7, [x9, #96]\n"
    "  ldp q8, q9, [x9, #128]\n"
    "  ldp q10, q11, [x9, #160]\n"
    "  ldp q12, q13, [x9, #192]\n"
    "  ldp q14, q15, [x9, #224]\n"
    "  ldp q16, q17, [x9, #256]\n"
    "  ldp q18, q19, [x9, #288]\n"
    "  ldp q20, q21, [x9, #320]\n"
    "  ldp q22, q23, [x9, #352]\n"
    "  ldp q24, q25, [x9, #384]\n"
    "  ldp q26, q27, [x9, #416]\n"
    "  ldp q28, q29, [x9, #448]\n"
    "  ldp q30, q31, [x9, #480]\n"
    /* flags and FP control */
    "  adrp x9, _cd_ctx_nzcv_pat@PAGE\n"
    "  ldr x10, [x9, _cd_ctx_nzcv_pat@PAGEOFF]\n"
    "  msr nzcv, x10\n"
    "  adrp x9, _cd_ctx_fpsr_pat@PAGE\n"
    "  ldr x10, [x9, _cd_ctx_fpsr_pat@PAGEOFF]\n"
    "  msr fpsr, x10\n"
    "  adrp x9, _cd_ctx_fpcr_pat@PAGE\n"
    "  ldr x10, [x9, _cd_ctx_fpcr_pat@PAGEOFF]\n"
    "  msr fpcr, x10\n"
    /* general registers */
    "  adrp x9, _cd_ctx_pat@PAGE\n"
    "  add x9, x9, _cd_ctx_pat@PAGEOFF\n"
    "  ldp x0, x1, [x9, #0]\n"
    "  ldp x2, x3, [x9, #16]\n"
    "  ldp x4, x5, [x9, #32]\n"
    "  ldp x6, x7, [x9, #48]\n"
    "  ldp x10, x11, [x9, #80]\n"
    "  ldp x12, x13, [x9, #96]\n"
    "  ldp x14, x15, [x9, #112]\n"
    "  ldp x16, x17, [x9, #128]\n"
    "  ldp x18, x19, [x9, #144]\n"
    "  ldp x20, x21, [x9, #160]\n"
    "  ldp x22, x23, [x9, #176]\n"
    "  ldp x24, x25, [x9, #192]\n"
    "  ldp x26, x27, [x9, #208]\n"
    "  ldp x29, x30, [x9, #232]\n"
    "  ldr x8, [x9, #64]\n"
    "  ldr x9, [x9, #72]\n"
    "  brk #0\n"
    "  brk #0\n"                           /* never reached: the handler redirects the pc */

    ".p2align 4\n"
    ".globl _cd_ctx_resume\n"
    "_cd_ctx_resume:\n"                   /* x28 = capture buffer; everything else = values written by the handler */
    "  stp x0, x1, [x28, #0]\n"
    "  stp x2, x3, [x28, #16]\n"
    "  stp x4, x5, [x28, #32]\n"
    "  stp x6, x7, [x28, #48]\n"
    "  stp x8, x9, [x28, #64]\n"
    "  stp x10, x11, [x28, #80]\n"
    "  stp x12, x13, [x28, #96]\n"
    "  stp x14, x15, [x28, #112]\n"
    "  stp x16, x17, [x28, #128]\n"
    "  stp x18, x19, [x28, #144]\n"
    "  stp x20, x21, [x28, #160]\n"
    "  stp x22, x23, [x28, #176]\n"
    "  stp x24, x25, [x28, #192]\n"
    "  stp x26, x27, [x28, #208]\n"
    "  stp x29, x30, [x28, #224]\n"
    "  str x28, [x28, #240]\n"
    "  mov x0, sp\n"
    "  str x0, [x28, #248]\n"
    "  mrs x0, nzcv\n"
    "  str x0, [x28, #256]\n"
    "  mrs x0, fpsr\n"
    "  str x0, [x28, #264]\n"
    "  mrs x0, fpcr\n"
    "  str x0, [x28, #272]\n"
    "  stp q0, q1, [x28, #288]\n"
    "  stp q2, q3, [x28, #320]\n"
    "  stp q4, q5, [x28, #352]\n"
    "  stp q6, q7, [x28, #384]\n"
    "  stp q8, q9, [x28, #416]\n"
    "  stp q10, q11, [x28, #448]\n"
    "  stp q12, q13, [x28, #480]\n"
    "  stp q14, q15, [x28, #512]\n"
    "  stp q16, q17, [x28, #544]\n"
    "  stp q18, q19, [x28, #576]\n"
    "  stp q20, q21, [x28, #608]\n"
    "  stp q22, q23, [x28, #640]\n"
    "  stp q24, q25, [x28, #672]\n"
    "  stp q26, q27, [x28, #704]\n"
    "  stp q28, q29, [x28, #736]\n"
    "  stp q30, q31, [x28, #768]\n"
    /* restore what the C caller expects */
    "  adrp x0, _cd_ctx_save@PAGE\n"
    "  add x0, x0, _cd_ctx_save@PAGEOFF\n"
    "  ldp x19, x20, [x0, #0]\n"
    "  ldp x21, x22, [x0, #16]\n"
    "  ldp x23, x24, [x0, #32]\n"
    "  ldp x25, x26, [x0, #48]\n"
    "  ldp x27, x28, [x0, #64]\n"
    "  ldp x29, x30, [x0, #80]\n"
    "  ldr x9, [x0, #96]\n"
    "  mov sp, x9\n"
    "  ldp d8, d9, [x0, #104]\n"
    "  ldp d10, d11, [x0, #120]\n"
    "  ldp d12, d13, [x0, #136]\n"
    "  ldp d14, d15, [x0, #152]\n"
    "  ldr x18, [x0, #168]\n"
    "  msr fpcr, xzr\n"
    "  msr fpsr, xzr\n"
    "  mov x0, #0\n"
    "  ret\n"

    /* x18 persistence: x0 = iterations, x1 = pattern; returns the number of iterations at which x18 != pattern */
    ".p2align 4\n"
    ".globl _cd_x18_spin\n"
    "_cd_x18_spin:\n"
    "  mov x9, x18\n"
    "  mov x18, x1\n"
    "  mov x2, #0\n"
    "1:\n"
    "  cmp x18, x1\n"
    "  b.eq 2f\n"
    "  add x2, x2, #1\n"
    "  mov x18, x1\n"
    "2:\n"
    "  subs x0, x0, #1\n"
    "  b.ne 1b\n"
    "  mov x0, x2\n"
    "  mov x18, x9\n"
    "  ret\n"

    /* x18 across the syscall path: getppid via svc #0x80 (x0 = iterations, x1 = pattern) */
    ".p2align 4\n"
    ".globl _cd_x18_syscall\n"
    "_cd_x18_syscall:\n"
    "  stp x19, x20, [sp, #-48]!\n"
    "  stp x21, x22, [sp, #16]\n"
    "  str x18, [sp, #32]\n"
    "  mov x19, x0\n"
    "  mov x20, x1\n"
    "  mov x21, #0\n"
    "  mov x18, x20\n"
    "1:\n"
    "  mov x16, #39\n"                     /* SYS_getppid */
    "  svc #0x80\n"
    "  cmp x18, x20\n"
    "  b.eq 2f\n"
    "  add x21, x21, #1\n"
    "  mov x18, x20\n"
    "2:\n"
    "  subs x19, x19, #1\n"
    "  b.ne 1b\n"
    "  mov x0, x21\n"
    "  ldr x18, [sp, #32]\n"
    "  ldp x21, x22, [sp, #16]\n"
    "  ldp x19, x20, [sp], #48\n"
    "  ret\n");

extern uint64_t cd_ctx_trigger(void *capture);
extern void cd_ctx_resume(void);
extern uint64_t cd_x18_spin(uint64_t iters, uint64_t pattern);
extern uint64_t cd_x18_syscall(uint64_t iters, uint64_t pattern);

#define PX(i) (0xA500000000000000ULL | ((uint64_t)(i) << 48) | 0x0000123456789a00ULL | (uint64_t)(i))
#define PXW(i) (0x5A00000000000000ULL | ((uint64_t)(i) << 48) | 0x0000fedcba987600ULL | (uint64_t)(i))
#define NZCV_READ 0xA0000000ULL
#define NZCV_WRITE 0x50000000ULL
#define FPSR_READ 0x08000013ULL
#define FPSR_WRITE 0x08000008ULL
#define FPCR_READ 0x01400000ULL
#define FPCR_WRITE 0x00800000ULL

typedef struct {
  uint64_t x[31];
  uint64_t sp, nzcv, fpsr, fpcr;
  uint8_t q[32][16];
} regfile_t;

static uint8_t g_capture[1024] __attribute__((aligned(16)));
static uint8_t *g_resume_stack;

static void qpat(uint8_t q[16], int i, int write) {
  uint64_t lo = (write ? 0x6600000000000000ULL : 0xB500000000000000ULL) | ((uint64_t)i << 48) | 0x11;
  uint64_t hi = (write ? 0x7700000000000000ULL : 0xC500000000000000ULL) | ((uint64_t)i << 48) | 0x22;
  memcpy(q, &lo, 8);
  memcpy(q + 8, &hi, 8);
}

static volatile int g_read_ok_x, g_read_bad_x, g_read_bad_v, g_read_cpsr_ok, g_read_fpsr_ok, g_read_fpcr_ok,
    g_read_sp_ok;
static volatile uint64_t g_read_cpsr, g_read_fpsr, g_read_fpcr, g_read_sp, g_read_pc, g_read_bad_x_mask;

static void ctx_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig;
  (void)si;
  /* READ test */
  int okx = 0, badx = 0;
  uint64_t badmask = 0;
  for (int i = 0; i < 29; i++) {
    uint64_t want = (i == 28) ? (uint64_t)(uintptr_t)g_capture : PX(i);
    if (CD_X(uc, i) == want) okx++; else { badx++; badmask |= 1ULL << i; }
  }
  if (CD_FP(uc) == PX(29)) okx++; else { badx++; badmask |= 1ULL << 29; }
  if (CD_LR(uc) == PX(30)) okx++; else { badx++; badmask |= 1ULL << 30; }
  int badv = 0;
  for (int i = 0; i < 32; i++) {
    uint8_t want[16];
    qpat(want, i, 0);
    if (memcmp(&CD_V(uc, i), want, 16) != 0) badv++;
  }
  g_read_ok_x = okx;
  g_read_bad_x = badx;
  g_read_bad_x_mask = badmask;
  g_read_bad_v = badv;
  g_read_cpsr = CD_CPSR(uc);
  g_read_cpsr_ok = (CD_CPSR(uc) & 0xF0000000u) == NZCV_READ;
  g_read_fpsr = CD_FPSR(uc);
  g_read_fpsr_ok = CD_FPSR(uc) == FPSR_READ;
  g_read_fpcr = CD_FPCR(uc);
  g_read_fpcr_ok = CD_FPCR(uc) == FPCR_READ;
  g_read_sp = CD_SP(uc);
  g_read_sp_ok = CD_SP(uc) == cd_ctx_save[12];
  g_read_pc = CD_PC(uc);
  /* WRITE test */
  for (int i = 0; i < 28; i++) CD_X(uc, i) = PXW(i);
  CD_X(uc, 28) = (uint64_t)(uintptr_t)g_capture;
  CD_FP(uc) = PXW(29);
  CD_LR(uc) = PXW(30);
  for (int i = 0; i < 32; i++) {
    uint8_t w[16];
    qpat(w, i, 1);
    memcpy(&CD_V(uc, i), w, 16);
  }
  CD_CPSR(uc) = (CD_CPSR(uc) & ~0xF0000000u) | (uint32_t)NZCV_WRITE;
  CD_FPSR(uc) = (uint32_t)FPSR_WRITE;
  CD_FPCR(uc) = (uint32_t)FPCR_WRITE;
  CD_SP(uc) = (uint64_t)(uintptr_t)(g_resume_stack + 64 * KiB - 64);
  CD_PC(uc) = (uint64_t)(uintptr_t)&cd_ctx_resume;
}

static int x_context_rw(int use_alt) {
  g_resume_stack = malloc(64 * KiB);
  char *alt = NULL;
  if (use_alt) {
    alt = malloc(128 * KiB);
    stack_t ss = {.ss_sp = alt, .ss_size = 128 * KiB, .ss_flags = 0};
    sigaltstack(&ss, NULL);
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = ctx_handler;
  sa.sa_flags = SA_SIGINFO | (use_alt ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTRAP, &sa, NULL);
  for (int i = 0; i < 31; i++) cd_ctx_pat[i] = (i == 28) ? 0 : PX(i);
  for (int i = 0; i < 32; i++) qpat(cd_ctx_q_pat[i], i, 0);
  cd_ctx_nzcv_pat = NZCV_READ;
  cd_ctx_fpsr_pat = FPSR_READ;
  cd_ctx_fpcr_pat = FPCR_READ;
  memset(g_capture, 0, sizeof g_capture);
  cd_ctx_trigger(g_capture);
  /* READ results */
  const char *how = use_alt ? "on a sigaltstack" : "on the thread stack";
  CHECK(g_read_bad_x == 0, "X03.read-gpr", "%s: handler sees x0..x28, fp, lr exactly as set before 'brk': %d of 31 match (mismatch mask %#llx)", how,
        g_read_ok_x, (unsigned long long)g_read_bad_x_mask);
  CHECK(g_read_bad_v == 0, "X03.read-vec", "%s: handler sees v0..v31 exactly as set before 'brk': %d of 32 differ", how, g_read_bad_v);
  CHECK(g_read_cpsr_ok, "X03.read-cpsr", "%s: NZCV in cpsr = %#llx (expected top nibble %#llx)", how, (unsigned long long)g_read_cpsr, (unsigned long long)NZCV_READ);
  CHECK(g_read_fpsr_ok, "X03.read-fpsr", "%s: fpsr = %#llx (expected %#llx)", how, (unsigned long long)g_read_fpsr, (unsigned long long)FPSR_READ);
  CHECK(g_read_fpcr_ok, "X03.read-fpcr", "%s: fpcr = %#llx (expected %#llx)", how, (unsigned long long)g_read_fpcr, (unsigned long long)FPCR_READ);
  CHECK(g_read_sp_ok, "X03.read-sp", "%s: sp in the context = %#llx, sp at the 'brk' = %#llx", how, (unsigned long long)g_read_sp, (unsigned long long)cd_ctx_save[12]);
  /* WRITE results */
  uint64_t *cap = (uint64_t *)g_capture;
  int wbad = 0;
  uint64_t wmask = 0;
  for (int i = 0; i < 28; i++) if (cap[i] != PXW(i)) { wbad++; wmask |= 1ULL << i; }
  if (cap[28] != PXW(29)) { wbad++; wmask |= 1ULL << 29; }
  if (cap[29] != PXW(30)) { wbad++; wmask |= 1ULL << 30; }
  CHECK(wbad == 0, "X04.write-gpr", "%s: after sigreturn, x0..x27, fp, lr hold the values written by the handler: %d mismatches (mask %#llx)", how, wbad, (unsigned long long)wmask);
  int wbv = 0;
  for (int i = 0; i < 32; i++) {
    uint8_t w[16];
    qpat(w, i, 1);
    if (memcmp(g_capture + 288 + 16 * i, w, 16) != 0) wbv++;
  }
  CHECK(wbv == 0, "X04.write-vec", "%s: after sigreturn, v0..v31 hold the values written by the handler: %d differ", how, wbv);
  CHECK(cap[30] == (uint64_t)(uintptr_t)g_capture, "X04.write-x28", "%s: x28 as written by the handler (this also proves the resume code ran at the pc written by the handler)", how);
  CHECK(cap[31] == (uint64_t)(uintptr_t)(g_resume_stack + 64 * KiB - 64), "X04.write-sp", "%s: sp changed to another stack by the handler: sp after sigreturn = %#llx (wrote %#llx)", how,
        (unsigned long long)cap[31], (unsigned long long)(uintptr_t)(g_resume_stack + 64 * KiB - 64));
  CHECK((cap[32] & 0xF0000000u) == NZCV_WRITE, "X04.write-nzcv", "%s: NZCV after sigreturn = %#llx (wrote %#llx)", how, (unsigned long long)cap[32], (unsigned long long)NZCV_WRITE);
  CHECK(cap[33] == FPSR_WRITE, "X04.write-fpsr", "%s: fpsr after sigreturn = %#llx (wrote %#llx)", how, (unsigned long long)cap[33], (unsigned long long)FPSR_WRITE);
  CHECK(cap[34] == FPCR_WRITE, "X04.write-fpcr", "%s: fpcr after sigreturn = %#llx (wrote %#llx)", how, (unsigned long long)cap[34], (unsigned long long)FPCR_WRITE);
  INFO("X04.x18", "%s: x18 as written by the handler = %#llx, seen after sigreturn = %#llx (%s)", how, (unsigned long long)PXW(18),
       (unsigned long long)cap[18], cap[18] == PXW(18) ? "preserved" : "NOT preserved");
  if (use_alt) {
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
  }
  return 0;
}

static int child_ctx0(void *a) { (void)a; return x_context_rw(0); }
static int child_ctx1(void *a) { (void)a; return x_context_rw(1); }

/* ------------------------------------------------------------------------- */
/* X01, X02: layout and frame geometry                                        */
/* ------------------------------------------------------------------------- */
static volatile uintptr_t g_geo[12];
static volatile int g_geo_done;
static uintptr_t g_alt_lo, g_alt_hi;

static void geo_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig;
  int local;
  g_geo[0] = (uintptr_t)si;
  g_geo[1] = (uintptr_t)uc;
  g_geo[2] = (uintptr_t)uc->uc_mcontext;
  g_geo[3] = (uintptr_t)&local;
  g_geo[4] = (uintptr_t)uc->uc_mcsize;
  g_geo[5] = (uintptr_t)uc->uc_stack.ss_sp;
  g_geo[6] = (uintptr_t)uc->uc_stack.ss_size;
  g_geo[7] = (uintptr_t)uc->uc_stack.ss_flags;
  g_geo[8] = (uintptr_t)uc->uc_onstack;
  g_geo[9] = (uintptr_t)uc->uc_link;
  g_geo[10] = CD_SP(uc);
  stack_t cur;
  sigaltstack(NULL, &cur);
  g_geo[11] = (uintptr_t)cur.ss_flags;
  g_geo_done = 1;
}

static int child_geometry(void *arg) {
  int use_alt = (int)(intptr_t)arg;
  char *alt = NULL;
  size_t asz = 128 * KiB;
  if (use_alt) {
    alt = malloc(asz);
    stack_t ss = {.ss_sp = alt, .ss_size = asz, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    g_alt_lo = (uintptr_t)alt;
    g_alt_hi = (uintptr_t)alt + asz;
  }
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = geo_handler;
  sa.sa_flags = SA_SIGINFO | (use_alt ? SA_ONSTACK : 0);
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  uintptr_t sp_before;
  __asm__ volatile("mov %0, sp" : "=r"(sp_before));
  raise(SIGUSR1);
  uintptr_t lo = g_geo[0];
  if (g_geo[1] < lo) lo = g_geo[1];
  if (g_geo[2] < lo) lo = g_geo[2];
  if (g_geo[3] < lo) lo = g_geo[3];
  uintptr_t top = use_alt ? g_alt_hi : g_geo[10];
  INFO("X02.frame", "%s: siginfo at top-%lld, ucontext at top-%lld, mcontext at top-%lld (ucontext + %lld), handler local at top-%lld; kernel-pushed frame size (top - lowest of si/uc/mc) = %lld bytes; uc_mcsize = %lu",
       use_alt ? "sigaltstack" : "thread stack", (long long)(top - g_geo[0]), (long long)(top - g_geo[1]), (long long)(top - g_geo[2]),
       (long long)(g_geo[2] - g_geo[1]), (long long)(top - g_geo[3]), (long long)(top - lo), (unsigned long)g_geo[4]);
  if (!use_alt)
    INFO("X02.frame", "thread stack: sp of the interrupted code %#lx (raise() runs in libc, so this is the sp inside raise), lowest frame address %#lx, distance %ld (includes any red zone skip)",
         (unsigned long)g_geo[10], (unsigned long)lo, (long)(g_geo[10] - lo));
  INFO("X02.uc_stack", "uc_stack.ss_sp=%#lx ss_size=%lu ss_flags=%lu, uc_onstack=%lu, uc_link=%#lx; sigaltstack(NULL,&cur) inside the handler: ss_flags=%lu (SS_ONSTACK=%d SS_DISABLE=%d)",
       (unsigned long)g_geo[5], (unsigned long)g_geo[6], (unsigned long)g_geo[7], (unsigned long)g_geo[8], (unsigned long)g_geo[9],
       (unsigned long)g_geo[11], SS_ONSTACK, SS_DISABLE);
  return 0;
}

static void x_layout(void) {
  INFO("X01.sizes", "sizeof(siginfo_t)=%zu sizeof(ucontext_t) as seen with this header (no _XOPEN_SOURCE, mcontext embedded only with it)=%zu sizeof(struct __darwin_mcontext64)=%zu sizeof(stack_t)=%zu sizeof(sigset_t)=%zu sizeof(sigset_t)*8 bits",
       sizeof(siginfo_t), sizeof(ucontext_t), sizeof(struct __darwin_mcontext64), sizeof(stack_t), sizeof(sigset_t));
  INFO("X01.ucontext", "offsetof(ucontext_t): uc_onstack=%zu uc_sigmask=%zu uc_stack=%zu uc_link=%zu uc_mcsize=%zu uc_mcontext=%zu",
       offsetof(ucontext_t, uc_onstack), offsetof(ucontext_t, uc_sigmask), offsetof(ucontext_t, uc_stack), offsetof(ucontext_t, uc_link),
       offsetof(ucontext_t, uc_mcsize), offsetof(ucontext_t, uc_mcontext));
  struct __darwin_mcontext64 *m = NULL;
  INFO("X01.mcontext", "offsetof(struct __darwin_mcontext64): __es=%zu (__far=%zu __esr=%zu __exception=%zu) __ss=%zu __ns=%zu",
       (size_t)&m->__es, (size_t)&m->__es.__far, (size_t)&m->__es.__esr, (size_t)&m->__es.__exception, (size_t)&m->__ss, (size_t)&m->__ns);
  INFO("X01.thread_state", "within __ss (offset from mcontext start): x0=%zu x28=%zu fp=%zu lr=%zu sp=%zu pc=%zu cpsr=%zu pad=%zu; sizeof(__ss)=%zu",
       (size_t)&m->__ss.__x[0], (size_t)&m->__ss.__x[28], (size_t)&m->__ss.__fp, (size_t)&m->__ss.__lr, (size_t)&m->__ss.__sp,
       (size_t)&m->__ss.__pc, (size_t)&m->__ss.__cpsr, (size_t)&m->__ss.__pad, sizeof(m->__ss));
  INFO("X01.neon_state", "within __ns (offset from mcontext start): v0=%zu v31=%zu fpsr=%zu fpcr=%zu; sizeof(__ns)=%zu, sizeof(__es)=%zu; mcontext total=%zu",
       (size_t)&m->__ns.__v[0], (size_t)&m->__ns.__v[31], (size_t)&m->__ns.__fpsr, (size_t)&m->__ns.__fpcr, sizeof(m->__ns), sizeof(m->__es),
       sizeof(*m));
  INFO("X01.consts", "MINSIGSTKSZ=%d SIGSTKSZ=%d SS_ONSTACK=%d SS_DISABLE=%d NSIG=%d", MINSIGSTKSZ, SIGSTKSZ, SS_ONSTACK, SS_DISABLE, NSIG);
}

/* ------------------------------------------------------------------------- */
/* X05: x18                                                                   */
/* ------------------------------------------------------------------------- */
static void x18_handler(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig; (void)si;
  /* record the x18 seen by the handler and write it back unchanged */
  static volatile uint64_t seen;
  seen = CD_X(uc, 18);
  (void)seen;
}

static volatile uint64_t g_x18_seen_in_handler;
static volatile int g_x18_handler_calls;
static void x18_handler2(int sig, siginfo_t *si, void *uctx) {
  ucontext_t *uc = (ucontext_t *)uctx;
  (void)sig; (void)si;
  g_x18_seen_in_handler = CD_X(uc, 18);
  g_x18_handler_calls++;
}

static _Atomic int g_x18_stop;
static void *x18_busy(void *a) { (void)a; volatile uint64_t x = 0; while (!atomic_load(&g_x18_stop)) x++; return NULL; }
static void *x18_kicker(void *a) {
  pthread_t target = *(pthread_t *)a;
  while (!atomic_load(&g_x18_stop)) {
    pthread_kill(target, SIGUSR1);
    usleep(200);
  }
  return NULL;
}

static int child_x18(void *a) {
  (void)a;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = x18_handler2;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, NULL);
  (void)x18_handler;
  /* 1: pure register spin, no signals, 1 s */
  uint64_t t0 = now_ns();
  uint64_t changes = 0, iters = 0;
  while (now_ns() - t0 < 1000000000ULL) {
    changes += cd_x18_spin(2000000, 0x1818181818181818ULL);
    iters += 2000000;
  }
  INFO("X05.x18", "register-only spin for 1 s (%llu M iterations, preemption by other work included): x18 changed %llu times", (unsigned long long)(iters / 1000000),
       (unsigned long long)changes);
  /* 2: with a signal every 200 us and 8 busy threads */
  atomic_store(&g_x18_stop, 0);
  pthread_t busy[8], kick, self = pthread_self();
  for (int i = 0; i < 8; i++) pthread_create(&busy[i], NULL, x18_busy, NULL);
  pthread_create(&kick, NULL, x18_kicker, &self);
  t0 = now_ns();
  changes = 0; iters = 0;
  g_x18_handler_calls = 0;
  while (now_ns() - t0 < 2000000000ULL) {
    changes += cd_x18_spin(200000, 0x1818181818181818ULL);
    iters += 200000;
  }
  atomic_store(&g_x18_stop, 1);
  pthread_join(kick, NULL);
  for (int i = 0; i < 8; i++) pthread_join(busy[i], NULL);
  INFO("X05.x18", "spin for 2 s with %d SIGUSR1 deliveries to this thread and 8 busy threads: x18 changed %llu times; last x18 seen by the handler in the context: %#llx",
       g_x18_handler_calls, (unsigned long long)changes, (unsigned long long)g_x18_seen_in_handler);
  /* 3: syscalls */
  changes = cd_x18_syscall(2000000, 0x1818181818181818ULL);
  INFO("X05.x18", "2 M direct 'svc #0x80' (getppid) calls: x18 changed %llu times", (unsigned long long)changes);
  /* 4: calls into libc/dyld stubs */
  uint64_t x18_in, x18_out;
  __asm__ volatile("mov x18, %1\n\tbl _getpid\n\tmov %0, x18" : "=r"(x18_out) : "r"(0x2828282828282828ULL) : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "memory", "cc");
  x18_in = 0x2828282828282828ULL;
  INFO("X05.x18", "x18 across a call to getpid() in libSystem: in=%#llx out=%#llx (%s)", (unsigned long long)x18_in, (unsigned long long)x18_out, x18_in == x18_out ? "preserved" : "CLOBBERED");
  return 0;
}

/* ------------------------------------------------------------------------- */
/* X06: sigaltstack API                                                       */
/* ------------------------------------------------------------------------- */
static volatile int g_alt_ran;
static void alt_small_handler(int sig, siginfo_t *si, void *uctx) {
  (void)sig; (void)si; (void)uctx;
  g_alt_ran = 1;
}

static int child_altapi(void *a) {
  (void)a;
  static const size_t sizes[] = {1024, 4096, 8192, MINSIGSTKSZ - 1, MINSIGSTKSZ, 16384, 32768, 65536, SIGSTKSZ};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    char *p = malloc(sizes[i] + 16);
    stack_t ss = {.ss_sp = p, .ss_size = sizes[i], .ss_flags = 0};
    int rc = sigaltstack(&ss, NULL);
    int e = errno;
    INFO("X06.sigaltstack", "sigaltstack(size=%zu): %s", sizes[i], rc ? errno_name(e) : "ok");
    free(p);
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
  }
  /* a handler delivered on a MINSIGSTKSZ stack: does it run? */
  for (size_t k = 0; k < 2; k++) {
    size_t sz = k == 0 ? MINSIGSTKSZ : SIGSTKSZ;
    char *p = malloc(sz);
    stack_t ss = {.ss_sp = p, .ss_size = sz, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = alt_small_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    g_alt_ran = 0;
    raise(SIGUSR2);
    INFO("X06.sigaltstack", "handler on a stack of exactly %s (%zu bytes): %s", k == 0 ? "MINSIGSTKSZ" : "SIGSTKSZ", sz, g_alt_ran ? "ran" : "did not run");
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, NULL);
    free(p);
  }
  /* alt stack of 1000 bytes registered through a raw syscall wrapper is refused above; also check "frame does not fit":
   * use a stack at the minimum and force a handler that uses 64 KiB of stack. */
  /* per-thread: a new thread has no alternate stack */
  stack_t cur;
  sigaltstack(NULL, &cur);
  INFO("X06.sigaltstack", "main thread after SS_DISABLE: ss_flags=%d (SS_DISABLE=%d)", cur.ss_flags, SS_DISABLE);
  return 0;
}

static void *alt_thread_probe(void *a) {
  stack_t cur;
  memset(&cur, 0xff, sizeof cur);
  sigaltstack(NULL, &cur);
  *(int *)a = cur.ss_flags;
  return NULL;
}

static void x_thread_altstack(void) {
  char *p = malloc(64 * KiB);
  stack_t ss = {.ss_sp = p, .ss_size = 64 * KiB, .ss_flags = 0};
  sigaltstack(&ss, NULL);
  int flags = -1;
  pthread_t th;
  pthread_create(&th, NULL, alt_thread_probe, &flags);
  pthread_join(th, NULL);
  INFO("X06.sigaltstack", "a thread created while the parent has a sigaltstack starts with ss_flags=%d (%s)", flags,
       (flags & SS_DISABLE) ? "SS_DISABLE: no inherited alternate stack, each thread must register its own" : "stack inherited");
  stack_t off = {.ss_flags = SS_DISABLE};
  sigaltstack(&off, NULL);
  free(p);
}

int main(int argc, char **argv) {
  dm_arm_watchdog(200);
  setvbuf(stdout, NULL, _IOLBF, 0);
  const char *only = argc > 1 ? argv[1] : NULL;
  cd_print_header(argv[0]);
#define RUN(tag, fn) do { if (!only || strstr(only, tag)) fn; } while (0)
  RUN("X01", x_layout());
  if (!only || strstr(only, "X02")) {
    int to = 0; char sb[96];
    int st = cd_run_child(child_geometry, (void *)(intptr_t)0, 20000, &to);
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st)) INFO("X02.child", "no alt: %s", cd_status_str(st, to, sb, sizeof sb));
    st = cd_run_child(child_geometry, (void *)(intptr_t)1, 20000, &to);
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st)) INFO("X02.child", "alt: %s", cd_status_str(st, to, sb, sizeof sb));
  }
  if (!only || strstr(only, "X03") || strstr(only, "X04")) {
    int to = 0; char sb[96];
    int st = cd_run_child(child_ctx0, NULL, 20000, &to);
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st)) INFO("X03.child", "thread stack: %s", cd_status_str(st, to, sb, sizeof sb));
    st = cd_run_child(child_ctx1, NULL, 20000, &to);
    if (to || WIFSIGNALED(st) || WEXITSTATUS(st)) INFO("X03.child", "sigaltstack: %s", cd_status_str(st, to, sb, sizeof sb));
  }
  if (!only || strstr(only, "X05")) {
    int to = 0; char sb[96];
    int st = cd_run_child(child_x18, NULL, 30000, &to);
    INFO("X05.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
  }
  if (!only || strstr(only, "X06")) {
    int to = 0; char sb[96];
    int st = cd_run_child(child_altapi, NULL, 20000, &to);
    INFO("X06.child", "result: %s", cd_status_str(st, to, sb, sizeof sb));
    x_thread_altstack();
  }
  printf("SUMMARY pass=%d fail=%d info=%d\n", g_pass, g_fail, g_info);
  return 0;
}
