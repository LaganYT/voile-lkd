// Witness (b): integer instruction semantics and flags. Expected values come from gen/gen_w02.py (a reference model of the
// Intel SDM semantics), never from the code under test. The same file also builds as a macOS x86_64 program (-DWITNESS_HOSTED)
// that runs under Rosetta 2, which gives a second, independent check of the tables.
#define QUIET_OK 1
#include "rt.h"
#include "w02_tables.h"

#define FL_MASK 0x8d5ULL /* CF PF AF ZF SF OF */

static const u64 V8[8] = {0x00, 0x01, 0x0f, 0x7f, 0x80, 0x81, 0xf0, 0xff};
static const u64 V16[8] = {0x0000, 0x0001, 0x00ff, 0x7fff, 0x8000, 0x8001, 0xff00, 0xffff};
static const u64 V32[8] = {0x0, 0x1, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0x0f0f0f0f, 0x12345678};
static const u64 V64[8] = {0x0, 0x1, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0x8000000000000001ULL, 0xffffffffffffffffULL, 0x00000000ffffffffULL, 0x123456789abcdef0ULL};
static const u64* const VALS[4] = {V8, V16, V32, V64};
static const int SIZES[4] = {8, 16, 32, 64};
static const u64 MASKS[4] = {0xff, 0xffff, 0xffffffffULL, ~0ULL};
static const u64 COUNTS[14] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65};
static const char* const ALU_NAMES[8] = {"add", "adc", "sub", "sbb", "and", "or", "xor", "cmp"};
static const char* const UN_NAMES[4] = {"neg", "not", "inc", "dec"};
static const char* const SH_NAMES[7] = {"shl", "shr", "sar", "rol", "ror", "rcl", "rcr"};
static const u16 ALU_FLMASK[8] = {0x8d5, 0x8d5, 0x8d5, 0x8d5, 0x8c5, 0x8c5, 0x8c5, 0x8d5};

static void fail_row(const char* what, const char* op, int size, u64 a, u64 b, int c, u64 got, u64 want, u64 gotfl, u64 wantfl) {
  g_fail++;
  puts_("FAIL "); puts_(what); puts_(" "); puts_(op); put_dec(size);
  puts_(" a=0x"); put_hex(a, 16); puts_(" b/count=0x"); put_hex(b, 16); puts_(" cf="); put_dec(c);
  puts_(" got=0x"); put_hex(got, 16); puts_(" want=0x"); put_hex(want, 16);
  puts_(" gotfl=0x"); put_hex(gotfl, 4); puts_(" wantfl=0x"); put_hex(wantfl, 4); puts_("\n");
}

// ---------- binary ALU ----------
#define ALU_ASM(INSN, M) \
  __asm__ volatile("pushq %3\n\tpopfq\n\t" INSN " %" M "2, %" M "0\n\tpushfq\n\tpopq %1" : "+a"(ra), "=d"(fl) : "c"(rb), "r"(init) : "cc", "memory")
static void alu_exec(int op, int si, u64 a, u64 b, int cf, u64* res, u64* flags) {
  u64 ra = a, rb = b, fl, init = 0x2 | (u64)cf;
  switch (op * 4 + si) {
#define OP4(O, INSN) \
  case O * 4 + 0: ALU_ASM(INSN, "b"); break; case O * 4 + 1: ALU_ASM(INSN, "w"); break; \
  case O * 4 + 2: ALU_ASM(INSN, "k"); break; case O * 4 + 3: ALU_ASM(INSN, "q"); break;
    OP4(0, "add") OP4(1, "adc") OP4(2, "sub") OP4(3, "sbb") OP4(4, "and") OP4(5, "or") OP4(6, "xor") OP4(7, "cmp")
  }
  *res = ra; *flags = fl;
}
static void test_alu(void) {
  u64 idx = 0;
  for (int si = 0; si < 4; si++)
    for (int op = 0; op < 8; op++)
      for (int ai = 0; ai < 8; ai++)
        for (int bi = 0; bi < 8; bi++) {
          const int carry = (op == 1 || op == 3);
          for (int cf = 0; cf <= carry; cf++, idx++) {
            u64 r, f; const u64 a = VALS[si][ai], b = VALS[si][bi];
            alu_exec(op, si, a, b, cf, &r, &f);
            f &= ALU_FLMASK[op];
            g_total++;
            if ((r & MASKS[si]) != ALU_RES[idx] || f != ALU_FL[idx]) fail_row("alu", ALU_NAMES[op], SIZES[si], a, b, cf, r & MASKS[si], ALU_RES[idx], f, ALU_FL[idx]);
          }
        }
  check64("alu rows consumed", idx, ALU_ROWS);
}

// ---------- unary ----------
#define UN_ASM(INSN, M) \
  __asm__ volatile("pushq %2\n\tpopfq\n\t" INSN " %" M "0\n\tpushfq\n\tpopq %1" : "+a"(ra), "=d"(fl) : "r"(init) : "cc", "memory")
static void un_exec(int op, int si, u64 a, int cf, u64* res, u64* flags) {
  u64 ra = a, fl, init = 0x2 | (u64)cf;
  switch (op * 4 + si) {
#define UOP4(O, INSN) \
  case O * 4 + 0: UN_ASM(INSN, "b"); break; case O * 4 + 1: UN_ASM(INSN, "w"); break; \
  case O * 4 + 2: UN_ASM(INSN, "k"); break; case O * 4 + 3: UN_ASM(INSN, "q"); break;
    UOP4(0, "neg") UOP4(1, "not") UOP4(2, "inc") UOP4(3, "dec")
  }
  *res = ra; *flags = fl;
}
static void test_unary(void) {
  u64 idx = 0;
  for (int si = 0; si < 4; si++)
    for (int op = 0; op < 4; op++)
      for (int ai = 0; ai < 8; ai++)
        for (int cf = 0; cf < 2; cf++, idx++) {
          u64 r, f; const u64 a = VALS[si][ai];
          un_exec(op, si, a, cf, &r, &f);
          f &= FL_MASK;
          g_total++;
          if ((r & MASKS[si]) != UN_RES[idx] || f != UN_FL[idx]) fail_row("unary", UN_NAMES[op], SIZES[si], a, 0, cf, r & MASKS[si], UN_RES[idx], f, UN_FL[idx]);
        }
  check64("unary rows consumed", idx, UN_ROWS);
}

// ---------- shifts and rotates by CL ----------
#define SH_ASM(INSN, M) \
  __asm__ volatile("pushq %3\n\tpopfq\n\t" INSN " %%cl, %" M "0\n\tpushfq\n\tpopq %1" : "+a"(ra), "=d"(fl) : "c"(cnt), "r"(init) : "cc", "memory")
static void sh_exec(int op, int si, u64 a, u64 cnt, int cf, u64* res, u64* flags) {
  u64 ra = a, fl, init = 0x2 | (u64)cf;
  switch (op * 4 + si) {
#define SOP4(O, INSN) \
  case O * 4 + 0: SH_ASM(INSN, "b"); break; case O * 4 + 1: SH_ASM(INSN, "w"); break; \
  case O * 4 + 2: SH_ASM(INSN, "k"); break; case O * 4 + 3: SH_ASM(INSN, "q"); break;
    SOP4(0, "shl") SOP4(1, "shr") SOP4(2, "sar") SOP4(3, "rol") SOP4(4, "ror") SOP4(5, "rcl") SOP4(6, "rcr")
  }
  *res = ra; *flags = fl;
}
static void test_shift(void) {
  u64 idx = 0;
  for (int si = 0; si < 4; si++)
    for (int op = 0; op < 7; op++)
      for (int ai = 0; ai < 8; ai++)
        for (int ci = 0; ci < 14; ci++)
          for (int cf = 0; cf < 2; cf++, idx++) {
            u64 r, f; const u64 a = VALS[si][ai];
            sh_exec(op, si, a, COUNTS[ci], cf, &r, &f);
            f &= SH_DM[idx];
            g_total++;
            if ((r & MASKS[si]) != SH_RES[idx] || f != SH_FL[idx]) fail_row("shift", SH_NAMES[op], SIZES[si], a, COUNTS[ci], cf, r & MASKS[si], SH_RES[idx], f, SH_FL[idx]);
          }
  check64("shift rows consumed", idx, SH_ROWS);
}

// ---------- multiply and divide (explicit vectors) ----------
static void test_muldiv(void) {
  // MUL: rows (size, a, b, lo, hi, cf)
  for (int i = 0; i < MUL_ROWS__N; i++) {
    const u64* r = &MUL_ROWS_[i * 6]; u64 size = r[0], a = r[1], b = r[2], lo, hi, fl;
    switch (size) {
    case 8: { u64 ax = a; __asm__ volatile("mulb %b2\n\tpushfq\n\tpopq %1" : "+a"(ax), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xff; hi = (ax >> 8) & 0xff; break; }
    case 16: { u64 ax = a, dx = 0; __asm__ volatile("mulw %w3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xffff; hi = dx & 0xffff; break; }
    case 32: { u64 ax = a, dx = 0; __asm__ volatile("mull %k3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xffffffffULL; hi = dx & 0xffffffffULL; break; }
    default: { u64 ax = a, dx = 0; __asm__ volatile("mulq %3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax; hi = dx; break; }
    }
    g_total++;
    const u64 cfof = (fl & 1) | ((fl >> 10) & 2);
    if (lo != r[3] || hi != r[4] || cfof != (r[5] ? 3 : 0)) fail_row("mul", "mul", (int)size, a, b, 0, lo, r[3], hi, r[4]);
  }
  // IMUL one operand
  for (int i = 0; i < IMUL1_ROWS__N; i++) {
    const u64* r = &IMUL1_ROWS_[i * 6]; u64 size = r[0], a = r[1], b = r[2], lo, hi, fl;
    switch (size) {
    case 8: { u64 ax = a; __asm__ volatile("imulb %b2\n\tpushfq\n\tpopq %1" : "+a"(ax), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xff; hi = (ax >> 8) & 0xff; break; }
    case 16: { u64 ax = a, dx = 0; __asm__ volatile("imulw %w3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xffff; hi = dx & 0xffff; break; }
    case 32: { u64 ax = a, dx = 0; __asm__ volatile("imull %k3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax & 0xffffffffULL; hi = dx & 0xffffffffULL; break; }
    default: { u64 ax = a, dx = 0; __asm__ volatile("imulq %3\n\tpushfq\n\tpopq %2" : "+a"(ax), "+d"(dx), "=r"(fl) : "r"(b) : "cc"); lo = ax; hi = dx; break; }
    }
    g_total++;
    const u64 cfof = (fl & 1) | ((fl >> 10) & 2);
    if (lo != r[3] || hi != r[4] || cfof != (r[5] ? 3 : 0)) fail_row("imul1", "imul", (int)size, a, b, 0, lo, r[3], hi, r[4]);
  }
  // IMUL two operand: rows (size, a, b, res, cf)
  for (int i = 0; i < IMUL2_ROWS__N; i++) {
    const u64* r = &IMUL2_ROWS_[i * 5]; u64 size = r[0], a = r[1], b = r[2], res, fl;
    switch (size) {
    case 16: { u64 x = a; __asm__ volatile("imulw %w2, %w0\n\tpushfq\n\tpopq %1" : "+r"(x), "=r"(fl) : "r"(b) : "cc"); res = x & 0xffff; break; }
    case 32: { u64 x = a; __asm__ volatile("imull %k2, %k0\n\tpushfq\n\tpopq %1" : "+r"(x), "=r"(fl) : "r"(b) : "cc"); res = x & 0xffffffffULL; break; }
    default: { u64 x = a; __asm__ volatile("imulq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(x), "=r"(fl) : "r"(b) : "cc"); res = x; break; }
    }
    g_total++;
    const u64 cfof = (fl & 1) | ((fl >> 10) & 2);
    if (res != r[3] || cfof != (r[4] ? 3 : 0)) fail_row("imul2", "imul", (int)size, a, b, 0, res, r[3], cfof, r[4] ? 3 : 0);
  }
  // DIV unsigned: rows (size, hi, lo, divisor, quo, rem)
  for (int i = 0; i < DIV_ROWS__N; i++) {
    const u64* r = &DIV_ROWS_[i * 6]; u64 size = r[0], hi = r[1], lo = r[2], d = r[3], q, rem;
    switch (size) {
    case 8: { u64 ax = (hi << 8) | lo; __asm__ volatile("divb %b1" : "+a"(ax) : "r"(d) : "cc"); q = ax & 0xff; rem = (ax >> 8) & 0xff; break; }
    case 16: { u64 ax = lo, dx = hi; __asm__ volatile("divw %w2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax & 0xffff; rem = dx & 0xffff; break; }
    case 32: { u64 ax = lo, dx = hi; __asm__ volatile("divl %k2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax & 0xffffffffULL; rem = dx & 0xffffffffULL; break; }
    default: { u64 ax = lo, dx = hi; __asm__ volatile("divq %2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax; rem = dx; break; }
    }
    g_total++;
    if (q != r[4] || rem != r[5]) fail_row("div", "div", (int)size, lo, d, 0, q, r[4], rem, r[5]);
  }
  // IDIV signed
  for (int i = 0; i < IDIV_ROWS__N; i++) {
    const u64* r = &IDIV_ROWS_[i * 6]; u64 size = r[0], hi = r[1], lo = r[2], d = r[3], q, rem;
    switch (size) {
    case 8: { u64 ax = (hi << 8) | lo; __asm__ volatile("idivb %b1" : "+a"(ax) : "r"(d) : "cc"); q = ax & 0xff; rem = (ax >> 8) & 0xff; break; }
    case 16: { u64 ax = lo, dx = hi; __asm__ volatile("idivw %w2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax & 0xffff; rem = dx & 0xffff; break; }
    case 32: { u64 ax = lo, dx = hi; __asm__ volatile("idivl %k2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax & 0xffffffffULL; rem = dx & 0xffffffffULL; break; }
    default: { u64 ax = lo, dx = hi; __asm__ volatile("idivq %2" : "+a"(ax), "+d"(dx) : "r"(d) : "cc"); q = ax; rem = dx; break; }
    }
    g_total++;
    if (q != r[4] || rem != r[5]) fail_row("idiv", "idiv", (int)size, lo, d, 0, q, r[4], rem, r[5]);
  }
}

// ---------- bit scans, popcnt, bswap ----------
static int cpu_has_abm(void) { u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0)); return (c >> 5) & 1; }
static int cpu_has_bmi1(void) { u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0)); return (b >> 3) & 1; }
static int cpu_has_popcnt(void) { u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0)); return (c >> 23) & 1; }
static void test_bits(void) {
  const int abm = cpu_has_abm(), bmi1 = cpu_has_bmi1(), pc = cpu_has_popcnt();
  puts_("  cpuid: abm(lzcnt)="); put_dec(abm); puts_(" bmi1(tzcnt)="); put_dec(bmi1); puts_(" popcnt="); put_dec(pc); puts_("\n");
  for (int i = 0; i < BIT_ROWS__N; i++) {
    const u64* r = &BIT_ROWS_[i * 7]; u64 size = r[0], v = r[1], got_f, got_r, fl;
    switch (size) {
    case 16: { u64 d = 0; __asm__ volatile("bsfw %w2, %w0\n\tpushfq\n\tpopq %1" : "+r"(d), "=r"(fl) : "r"(v) : "cc"); got_f = d & 0xffff; break; }
    case 32: { u64 d = 0; __asm__ volatile("bsfl %k2, %k0\n\tpushfq\n\tpopq %1" : "+r"(d), "=r"(fl) : "r"(v) : "cc"); got_f = d & 0xffffffffULL; break; }
    default: { u64 d = 0; __asm__ volatile("bsfq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(d), "=r"(fl) : "r"(v) : "cc"); got_f = d; break; }
    }
    g_total++; if (got_f != r[2] || (fl & 0x40)) fail_row("bsf", "bsf", (int)size, v, 0, 0, got_f, r[2], fl & 0x40, 0);
    switch (size) {
    case 16: { u64 d = 0; __asm__ volatile("bsrw %w1, %w0" : "+r"(d) : "r"(v) : "cc"); got_r = d & 0xffff; break; }
    case 32: { u64 d = 0; __asm__ volatile("bsrl %k1, %k0" : "+r"(d) : "r"(v) : "cc"); got_r = d & 0xffffffffULL; break; }
    default: { u64 d = 0; __asm__ volatile("bsrq %1, %0" : "+r"(d) : "r"(v) : "cc"); got_r = d; break; }
    }
    g_total++; if (got_r != r[3]) fail_row("bsr", "bsr", (int)size, v, 0, 0, got_r, r[3], 0, 0);
    if (pc) {
      u64 p;
      switch (size) {
      case 16: __asm__ volatile("popcntw %w1, %w0" : "=r"(p) : "r"(v) : "cc"); p &= 0xffff; break;
      case 32: __asm__ volatile("popcntl %k1, %k0" : "=r"(p) : "r"(v) : "cc"); p &= 0xffffffffULL; break;
      default: __asm__ volatile("popcntq %1, %0" : "=r"(p) : "r"(v) : "cc"); break;
      }
      g_total++; if (p != r[4]) fail_row("popcnt", "popcnt", (int)size, v, 0, 0, p, r[4], 0, 0);
    }
    if (bmi1) {
      u64 t;
      switch (size) {
      case 16: __asm__ volatile("tzcntw %w1, %w0" : "=r"(t) : "r"(v) : "cc"); t &= 0xffff; break;
      case 32: __asm__ volatile("tzcntl %k1, %k0" : "=r"(t) : "r"(v) : "cc"); t &= 0xffffffffULL; break;
      default: __asm__ volatile("tzcntq %1, %0" : "=r"(t) : "r"(v) : "cc"); break;
      }
      g_total++; if (t != r[5]) fail_row("tzcnt", "tzcnt", (int)size, v, 0, 0, t, r[5], 0, 0);
    }
    if (abm) {
      u64 l;
      switch (size) {
      case 16: __asm__ volatile("lzcntw %w1, %w0" : "=r"(l) : "r"(v) : "cc"); l &= 0xffff; break;
      case 32: __asm__ volatile("lzcntl %k1, %k0" : "=r"(l) : "r"(v) : "cc"); l &= 0xffffffffULL; break;
      default: __asm__ volatile("lzcntq %1, %0" : "=r"(l) : "r"(v) : "cc"); break;
      }
      g_total++; if (l != r[6]) fail_row("lzcnt", "lzcnt", (int)size, v, 0, 0, l, r[6], 0, 0);
    }
  }
  // zero source: bsf/bsr set ZF=1; popcnt gives 0 with ZF=1; tzcnt/lzcnt give the operand size with CF=1
  { u64 d = 5, fl; u64 z = 0; __asm__ volatile("bsfq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(d), "=r"(fl) : "r"(z) : "cc"); checkb("bsf zero sets ZF", (fl & 0x40) != 0); }
  { u64 d = 5, fl; u64 z = 0; __asm__ volatile("bsrq %2, %0\n\tpushfq\n\tpopq %1" : "+r"(d), "=r"(fl) : "r"(z) : "cc"); checkb("bsr zero sets ZF", (fl & 0x40) != 0); }
  if (pc) { u64 p, fl; u64 z = 0; __asm__ volatile("popcntq %2, %0\n\tpushfq\n\tpopq %1" : "=r"(p), "=r"(fl) : "r"(z) : "cc"); check64("popcnt 0", p, 0); check64("popcnt 0 flags", fl & FL_MASK, 0x40); }
  if (bmi1) { u64 t, fl; u64 z = 0; __asm__ volatile("tzcntq %2, %0\n\tpushfq\n\tpopq %1" : "=r"(t), "=r"(fl) : "r"(z) : "cc"); check64("tzcnt 0", t, 64); check64("tzcnt 0 flags (CF)", fl & FL_MASK & ~0x14ULL, 0x1); }
  if (abm) { u64 l, fl; u64 z = 0; __asm__ volatile("lzcntq %2, %0\n\tpushfq\n\tpopq %1" : "=r"(l), "=r"(fl) : "r"(z) : "cc"); check64("lzcnt 0", l, 64); check64("lzcnt 0 flags (CF)", fl & FL_MASK & ~0x14ULL, 0x1); }
  for (int i = 0; i < 4; i++) {
    u64 x = BSWAP_IN[i], y = BSWAP_IN[i];
    __asm__ volatile("bswapq %0" : "+r"(x)); __asm__ volatile("bswapl %k0" : "+r"(y));
    check64("bswap64", x, BSWAP64[i]); check64("bswap32", y, BSWAP32[i]);
  }
}

// ---------- bt family ----------
static void test_bt(void) {
  u64 mem[3] = {0x0000000000000005ULL, 0x8000000000000000ULL, 0x0000000000000002ULL};
  u64 fl, r;
  // register form: offset taken modulo the operand size
  r = 0x100; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : "r"((u64)8) : "cc"); check64("bt q reg bit8 set", fl & 1, 1);
  r = 0x100; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : "r"((u64)72) : "cc"); check64("bt q reg offset 72 -> bit 8", fl & 1, 1);
  r = 0x100; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : "r"((u64)9) : "cc"); check64("bt q reg bit9 clear", fl & 1, 0);
  r = 0x10;  __asm__ volatile("btl %k2, %k1\n\tsetc %b0" : "=r"(fl), "+r"(r) : "r"((u64)36) : "cc"); check64("bt l reg offset 36 -> bit 4", fl & 1, 1);
  r = 0x10;  __asm__ volatile("btw %w2, %w1\n\tsetc %b0" : "=r"(fl), "+r"(r) : "r"((u64)20) : "cc"); check64("bt w reg offset 20 -> bit 4", fl & 1, 1);
  // immediate forms
  r = 0x80;  __asm__ volatile("btq $7, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : : "cc"); check64("bt q imm 7", fl & 1, 1);
  r = 0;     __asm__ volatile("btsq $63, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : : "cc"); check64("bts q imm 63 old", fl & 1, 0); check64("bts q imm 63 new", r, 0x8000000000000000ULL);
  r = ~0ULL; __asm__ volatile("btrq $0, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : : "cc"); check64("btr q imm 0 old", fl & 1, 1); check64("btr q imm 0 new", r, 0xfffffffffffffffeULL);
  r = 0x3;   __asm__ volatile("btcq $1, %1\n\tsetc %b0" : "=r"(fl), "+r"(r) : : "cc"); check64("btc q imm 1 old", fl & 1, 1); check64("btc q imm 1 new", r, 0x1);
  // memory form with register offset: addresses beyond the first qword (bit string semantics)
  { u64 off = 64 + 63; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl) : "m"(mem[0]), "r"(off) : "cc"); check64("bt mem offset 127 -> mem[1] bit 63", fl & 1, 1); }
  { u64 off = 128 + 1; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl) : "m"(mem[0]), "r"(off) : "cc"); check64("bt mem offset 129 -> mem[2] bit 1", fl & 1, 1); }
  { u64 off = 2; __asm__ volatile("btsq %2, %1\n\tsetc %b0" : "=r"(fl), "+m"(mem[0]) : "r"(off) : "cc"); check64("bts mem old", fl & 1, 1); check64("bts mem new (already set)", mem[0], 5); }
  { u64 off = 64 + 5; __asm__ volatile("btsq %2, %1\n\tsetc %b0" : "=r"(fl), "+m"(mem[0]) : "r"(off) : "cc"); check64("bts mem offset 69 old", fl & 1, 0); check64("bts mem offset 69 -> mem[1]", mem[1], 0x8000000000000020ULL); }
  { u64 off = 64 + 5; __asm__ volatile("btrq %2, %1\n\tsetc %b0" : "=r"(fl), "+m"(mem[0]) : "r"(off) : "cc"); check64("btr mem offset 69 old", fl & 1, 1); check64("btr mem offset 69 -> mem[1]", mem[1], 0x8000000000000000ULL); }
  { u64 off = 64 + 63; __asm__ volatile("btcq %2, %1\n\tsetc %b0" : "=r"(fl), "+m"(mem[0]) : "r"(off) : "cc"); check64("btc mem offset 127 old", fl & 1, 1); check64("btc mem offset 127 -> mem[1]", mem[1], 0); }
  // negative bit offsets address bytes below the base
  { u64 buf[2] = {0, 0x1}; u64 off = (u64)(i64)-64; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl) : "m"(buf[1]), "r"(off) : "cc"); check64("bt mem negative offset -64", fl & 1, 0); }
  { u64 buf[2] = {0x4, 0}; u64 off = (u64)(i64)-62; __asm__ volatile("btq %2, %1\n\tsetc %b0" : "=r"(fl) : "m"(buf[1]), "r"(off) : "cc"); check64("bt mem negative offset -62 -> buf[0] bit 2", fl & 1, 1); }
}

// ---------- string instructions ----------
static void test_string(void) {
  u8 src[40], dst[40];
  for (int i = 0; i < 40; i++) { src[i] = (u8)(i * 7 + 1); dst[i] = 0xee; }
  { u64 rcx = 13; void *rsi = src + 3, *rdi = dst + 5; __asm__ volatile("cld\n\trep movsb" : "+c"(rcx), "+S"(rsi), "+D"(rdi) : : "memory", "cc");
    check64("rep movsb rcx", rcx, 0); check64("rep movsb rsi", (u64)rsi, (u64)(src + 16)); check64("rep movsb rdi", (u64)rdi, (u64)(dst + 18));
    int ok = 1; for (int i = 0; i < 13; i++) ok &= dst[5 + i] == src[3 + i]; ok &= dst[4] == 0xee && dst[18] == 0xee; checkb("rep movsb data and guard bytes", ok); }
  { u64 rcx = 4; void *rsi = src, *rdi = dst + 8; __asm__ volatile("cld\n\trep movsq" : "+c"(rcx), "+S"(rsi), "+D"(rdi) : : "memory", "cc");
    check64("rep movsq rdi", (u64)rdi, (u64)(dst + 40)); int ok = 1; for (int i = 0; i < 32; i++) ok &= dst[8 + i] == src[i]; checkb("rep movsq data", ok); }
  { for (int i = 0; i < 40; i++) dst[i] = 0xee;
    u64 rcx = 6; void *rsi = src + 10, *rdi = dst + 20; __asm__ volatile("std\n\trep movsb\n\tcld" : "+c"(rcx), "+S"(rsi), "+D"(rdi) : : "memory", "cc");
    check64("std rep movsb rsi", (u64)rsi, (u64)(src + 4)); check64("std rep movsb rdi", (u64)rdi, (u64)(dst + 14));
    int ok = 1; for (int i = 0; i < 6; i++) ok &= dst[20 - i] == src[10 - i]; ok &= dst[21] == 0xee && dst[14] == 0xee; checkb("std rep movsb data", ok); }
  { u64 rcx = 0; void *rsi = src, *rdi = dst; __asm__ volatile("cld\n\trep movsb" : "+c"(rcx), "+S"(rsi), "+D"(rdi) : : "memory", "cc"); check64("rep movsb with rcx=0 leaves rsi", (u64)rsi, (u64)src); }
  { u64 rcx = 9; void* rdi = dst; u64 rax = 0x41; __asm__ volatile("cld\n\trep stosb" : "+c"(rcx), "+D"(rdi) : "a"(rax) : "memory", "cc");
    check64("rep stosb rdi", (u64)rdi, (u64)(dst + 9)); int ok = 1; for (int i = 0; i < 9; i++) ok &= dst[i] == 0x41; ok &= dst[9] == 0xee; checkb("rep stosb data", ok); }
  { u64 rcx = 3; void* rdi = dst + 16; u64 rax = 0x1122334455667788ULL; __asm__ volatile("cld\n\trep stosq" : "+c"(rcx), "+D"(rdi) : "a"(rax) : "memory", "cc");
    u64 w; __builtin_memcpy(&w, dst + 24, 8); check64("rep stosq middle qword", w, 0x1122334455667788ULL); }
  { u64 rcx = 4; void* rdi = dst; u64 rax = 0xcafebabe; __asm__ volatile("cld\n\trep stosl" : "+c"(rcx), "+D"(rdi) : "a"(rax) : "memory", "cc"); u32 w; __builtin_memcpy(&w, dst + 12, 4); check64("rep stosl last dword", w, 0xcafebabe); }
  // repe cmpsb / repne scasb
  { u8 a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[8] = {1, 2, 3, 4, 9, 6, 7, 8}; u64 rcx = 8; void *rsi = a, *rdi = b; u64 fl;
    __asm__ volatile("cld\n\trepe cmpsb\n\tpushfq\n\tpopq %3" : "+c"(rcx), "+S"(rsi), "+D"(rdi), "=r"(fl) : : "memory", "cc");
    check64("repe cmpsb stops after the mismatch: rcx", rcx, 3); check64("repe cmpsb rsi", (u64)rsi, (u64)(a + 5)); checkb("repe cmpsb ZF clear", !(fl & 0x40)); }
  { u8 s[8] = {9, 9, 9, 7, 9, 9, 9, 9}; u64 rcx = 8; void* rdi = s; u64 rax = 7; u64 fl;
    __asm__ volatile("cld\n\trepne scasb\n\tpushfq\n\tpopq %3" : "+c"(rcx), "+D"(rdi), "+a"(rax), "=r"(fl) : : "memory", "cc");
    check64("repne scasb finds the byte: rcx", rcx, 4); check64("repne scasb rdi", (u64)rdi, (u64)(s + 4)); checkb("repne scasb ZF set", (fl & 0x40) != 0); }
  // lodsb / stosb / movsb single
  { u8 s[2] = {0x5a, 0}; u64 rax = 0; void* rsi = s; __asm__ volatile("cld\n\tlodsb" : "+a"(rax), "+S"(rsi) : : "memory", "cc"); check64("lodsb al", rax & 0xff, 0x5a); check64("lodsb rsi", (u64)rsi, (u64)(s + 1)); }
}

// ---------- exchange, compare-exchange, lock ----------
static void test_atomics(void) {
  u64 m = 0x1111, fl;
  { u64 rax = 0x1111, new_ = 0x2222; __asm__ volatile("cmpxchgq %2, %1\n\tpushfq\n\tpopq %3" : "+a"(rax), "+m"(m), "+r"(new_), "=r"(fl) : : "cc"); check64("cmpxchg success mem", m, 0x2222); checkb("cmpxchg success ZF", (fl & 0x40) != 0); check64("cmpxchg success rax", rax, 0x1111); }
  { u64 rax = 0x9999, new_ = 0x3333; __asm__ volatile("cmpxchgq %2, %1\n\tpushfq\n\tpopq %3" : "+a"(rax), "+m"(m), "+r"(new_), "=r"(fl) : : "cc"); check64("cmpxchg fail mem", m, 0x2222); checkb("cmpxchg fail ZF clear", !(fl & 0x40)); check64("cmpxchg fail loads rax", rax, 0x2222); }
  { u32 m32 = 0xdeadbeef; u64 rax = 0xdeadbeef, n = 0x12345678; __asm__ volatile("lock cmpxchgl %k2, %1" : "+a"(rax), "+m"(m32), "+r"(n) : : "cc"); check64("lock cmpxchg32 mem", m32, 0x12345678); }
  { u16 m16 = 0x1234; u64 rax = 0x1234, n = 0xabcd; __asm__ volatile("lock cmpxchgw %w2, %1" : "+a"(rax), "+m"(m16), "+r"(n) : : "cc"); check64("lock cmpxchg16 mem", m16, 0xabcd); }
  { u8 m8 = 0x7f; u64 rax = 0x7e, n = 0x01; __asm__ volatile("lock cmpxchgb %b2, %1" : "+a"(rax), "+m"(m8), "+r"(n) : : "cc"); check64("lock cmpxchg8 fail mem", m8, 0x7f); check64("lock cmpxchg8 fail al", rax & 0xff, 0x7f); }
  { u64 a = 5, b = 7; __asm__ volatile("xchgq %0, %1" : "+r"(a), "+m"(b)); check64("xchg reg,mem reg", a, 7); check64("xchg reg,mem mem", b, 5); }
  { u64 a = 5, b = 7; __asm__ volatile("xchgq %0, %1" : "+r"(a), "+r"(b)); check64("xchg reg,reg a", a, 7); check64("xchg reg,reg b", b, 5); }
  { u64 mem = 100, r = 23; __asm__ volatile("lock xaddq %0, %1" : "+r"(r), "+m"(mem) : : "cc"); check64("lock xadd mem", mem, 123); check64("lock xadd old value", r, 100); }
  { u64 mem = 0xf0; __asm__ volatile("lock addq $0x10, %0" : "+m"(mem) : : "cc"); check64("lock add", mem, 0x100); }
  { u64 mem = 0x100; __asm__ volatile("lock subq $1, %0" : "+m"(mem) : : "cc"); check64("lock sub", mem, 0xff); }
  { u64 mem = 0xff; __asm__ volatile("lock andq $0x0f, %0" : "+m"(mem) : : "cc"); check64("lock and", mem, 0x0f); }
  { u64 mem = 0x0f; __asm__ volatile("lock orq $0xf0, %0" : "+m"(mem) : : "cc"); check64("lock or", mem, 0xff); }
  { u64 mem = 0xff; __asm__ volatile("lock xorq $0x0f, %0" : "+m"(mem) : : "cc"); check64("lock xor", mem, 0xf0); }
  { u64 mem = 0; __asm__ volatile("lock incq %0" : "+m"(mem) : : "cc"); __asm__ volatile("lock incq %0" : "+m"(mem) : : "cc"); __asm__ volatile("lock decq %0" : "+m"(mem) : : "cc"); check64("lock inc inc dec", mem, 1); }
  { u64 mem = 5; __asm__ volatile("lock negq %0" : "+m"(mem) : : "cc"); check64("lock neg", mem, (u64)-5); }
  { u64 mem = 0; __asm__ volatile("lock btsq $5, %0" : "+m"(mem) : : "cc"); check64("lock bts", mem, 0x20); }
  // cmpxchg16b needs CPUID.1:ECX.CX16
  { u32 a, b, c, d; __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if ((c >> 13) & 1) {
      __attribute__((aligned(16))) u64 pair[2] = {0x1111, 0x2222};
      u64 rax = 0x1111, rdx = 0x2222; u64 rbx = 0xaaaa, rcx = 0xbbbb;
      __asm__ volatile("xchgq %%rbx, %4\n\tlock cmpxchg16b %1\n\txchgq %%rbx, %4" : "+a"(rax), "+m"(pair), "+d"(rdx) : "c"(rcx), "r"(rbx) : "cc");
      check64("cmpxchg16b lo", pair[0], 0xaaaa); check64("cmpxchg16b hi", pair[1], 0xbbbb);
    } else puts_("  cmpxchg16b not advertised by cpuid, skipped\n"); }
}

// ---------- misc integer ----------
static void test_misc(void) {
  { u64 r; __asm__ volatile("movzbl %b1, %k0" : "=r"(r) : "r"((u64)0xfffffffffffffff1ULL)); check64("movzx 8->32", r, 0xf1); }
  { u64 r; __asm__ volatile("movsbq %b1, %0" : "=r"(r) : "r"((u64)0x80)); check64("movsx 8->64", r, 0xffffffffffffff80ULL); }
  { u64 r; __asm__ volatile("movswl %w1, %k0" : "=r"(r) : "r"((u64)0x8001)); check64("movsx 16->32 zero-extends to 64", r, 0xffff8001ULL); }
  { u64 r; __asm__ volatile("movslq %k1, %0" : "=r"(r) : "r"((u64)0x80000000ULL)); check64("movsxd 32->64", r, 0xffffffff80000000ULL); }
  { u64 r = 0x1122334455667788ULL; __asm__ volatile("movl $1, %k0" : "+r"(r)); check64("mov r32 zero-extends", r, 1); }
  { u64 r = 0x1122334455667788ULL; __asm__ volatile("movw $0x99, %w0" : "+r"(r)); check64("mov r16 keeps upper bits", r, 0x1122334455660099ULL); }
  { u64 r = 0x1122334455667788ULL; __asm__ volatile("movb $0x99, %b0" : "+r"(r)); check64("mov r8 keeps upper bits", r, 0x1122334455667799ULL); }
  { u64 a = 0xffffffff80000000ULL; u64 d; __asm__ volatile("cltq" : "+a"(a)); check64("cdqe sign extends eax", a, 0xffffffff80000000ULL);
    u64 x = 0x80000000ULL; __asm__ volatile("cltq" : "+a"(x)); check64("cdqe 0x80000000", x, 0xffffffff80000000ULL);
    u64 y = 0x8000000000000000ULL; __asm__ volatile("cqto" : "+a"(y), "=d"(d)); check64("cqo rdx", d, ~0ULL);
    u64 z = 0x7fffffffffffffffULL; __asm__ volatile("cqto" : "+a"(z), "=d"(d)); check64("cqo positive", d, 0);
    u64 w = 0x8000; __asm__ volatile("cwtl" : "+a"(w)); check64("cwde", w, 0xffff8000ULL);
    u64 v = 0x80; __asm__ volatile("cbtw" : "+a"(v)); check64("cbw", v & 0xffff, 0xff80); }
  { u64 r; __asm__ volatile("leaq 0x10(%1,%2,8), %0" : "=r"(r) : "r"((u64)0x1000), "r"((u64)3)); check64("lea base+index*8+disp", r, 0x1000 + 24 + 0x10); }
  { u64 r; __asm__ volatile("leal 5(%k1,%k2,4), %k0" : "=r"(r) : "r"((u64)0xffffffffULL), "r"((u64)1)); check64("lea 32-bit wraps", r, 8); }
  { u64 r = 0; __asm__ volatile("addr32 leaq 0(%%ebx,%%ecx,2), %0" : "=r"(r) : "b"((u64)0xffffffff00000010ULL), "c"((u64)0xffffffff00000002ULL)); check64("lea with 0x67 prefix uses 32-bit address", r, 0x14); }
  // setcc / cmovcc over flag patterns built with popfq
  static const u16 FL[8] = {0x002, 0x003, 0x043, 0x082, 0x802, 0x886, 0x842, 0x0c7};
  for (int i = 0; i < 8; i++) {
    u64 fl = FL[i]; u64 out = 0;
    // evaluate all 16 conditions, bit k of out = condition k
    u64 res = 0;
#define COND(K, CC) { u64 t = 0; __asm__ volatile("pushq %1\n\tpopfq\n\tset" CC " %b0" : "+q"(t) : "r"(fl) : "cc"); res |= (t & 1) << K; }
    COND(0, "o") COND(1, "no") COND(2, "b") COND(3, "ae") COND(4, "e") COND(5, "ne") COND(6, "be") COND(7, "a")
    COND(8, "s") COND(9, "ns") COND(10, "p") COND(11, "np") COND(12, "l") COND(13, "ge") COND(14, "le") COND(15, "g")
    const int cf = (fl >> 0) & 1, pf = (fl >> 2) & 1, zf = (fl >> 6) & 1, sf = (fl >> 7) & 1, of = (fl >> 11) & 1;
    u64 want = 0;
    want |= (u64)of << 0; want |= (u64)!of << 1; want |= (u64)cf << 2; want |= (u64)!cf << 3; want |= (u64)zf << 4; want |= (u64)!zf << 5;
    want |= (u64)(cf | zf) << 6; want |= (u64)!(cf | zf) << 7; want |= (u64)sf << 8; want |= (u64)!sf << 9; want |= (u64)pf << 10; want |= (u64)!pf << 11;
    want |= (u64)(sf != of) << 12; want |= (u64)(sf == of) << 13; want |= (u64)(zf | (sf != of)) << 14; want |= (u64)(!zf && sf == of) << 15;
    (void)out;
    check64("setcc matrix for flag image", res, want);
  }
  { u64 r = 1, s = 2; __asm__ volatile("stc\n\tcmovcq %1, %0" : "+r"(r) : "r"(s) : "cc"); check64("cmovc taken", r, 2); }
  { u64 r = 1, s = 2; __asm__ volatile("clc\n\tcmovcq %1, %0" : "+r"(r) : "r"(s) : "cc"); check64("cmovc not taken", r, 1); }
  { u64 r = 0x1122334455667788ULL, s = 2; __asm__ volatile("clc\n\tcmovcl %k1, %k0" : "+r"(r) : "r"(s) : "cc"); check64("cmov 32-bit not taken still zero-extends", r, 0x55667788ULL); }
  // lahf / sahf
  { u64 ax; __asm__ volatile("stc\n\tlahf" : "=a"(ax) : : "cc"); check64("lahf after stc, AH bit0 (CF) and bit1 (always 1)", (ax >> 8) & 3, 3); }
  { u64 ax = 0x4100; u64 fl; __asm__ volatile("sahf\n\tpushfq\n\tpopq %1" : "+a"(ax), "=r"(fl) : : "cc"); check64("sahf sets CF and ZF", fl & 0x41, 0x41); }
  // pushfq/popfq roundtrip of arithmetic flags and DF (0x6d7 = CF PF AF ZF SF IF DF plus the reserved bit)
  { u64 fl; __asm__ volatile("pushq $0x6d7\n\tpopfq\n\tpushfq\n\tpopq %0\n\tcld" : "=r"(fl) : : "cc"); check64("popfq/pushfq roundtrip (CF PF AF ZF SF DF)", fl & 0xcd5, 0x4d5); }
  // lock-free load/store ordering helpers must at least be callable
  __asm__ volatile("mfence\n\tsfence\n\tlfence" ::: "memory");
  // push/pop
  { u64 r; __asm__ volatile("pushq $0x1234\n\tpopq %0" : "=r"(r) : : "memory"); check64("push imm / pop", r, 0x1234); }
  { u64 r; __asm__ volatile("movq %%rsp, %%rcx\n\tpushq %%rsp\n\tpopq %0\n\tcmpq %%rcx, %0\n\tsete %b0" : "=q"(r) : : "rcx", "cc", "memory"); check64("push rsp stores the pre-push rsp", r & 1, 1); }
}

static int run(void) {
  puts_("w02_int: integer semantics and flags against the reference model\n");
  test_alu(); test_unary(); test_shift(); test_muldiv(); test_bits(); test_bt(); test_string(); test_atomics(); test_misc();
  return finish("w02_int");
}
WITNESS_MAIN(run)
