// Witness (c): SSE, SSE2, SSSE3, SSE4.1/4.2, AVX, AVX2, FMA, F16C and x87 results.
// The program prints one line per operation ("name = hex words"). The same source built with -DWITNESS_HOSTED runs on
// the x86_64 path of Rosetta 2, used only as an independent oracle: the two transcripts must be identical.
// Inputs come from volatile memory so the compiler cannot fold the operations.
#include "rt.h"
#include <immintrin.h>

static void put_line(const char* name, const void* p, int bytes) {
  puts_(name);
  puts_(" =");
  const u32* w = (const u32*)p;
  for (int i = bytes / 4 - 1; i >= 0; i--) { puts_(" "); put_hex(w[i], 8); }
  puts_("\n");
}
#define OUT128(n, v) do { __m128i t_ = (__m128i)(v); put_line(n, &t_, 16); } while (0)
#define OUT256(n, v) do { __m256i t_ = (__m256i)(v); put_line(n, &t_, 32); } while (0)
#define OUT64(n, v) do { u64 t_ = (u64)(v); put_line(n, &t_, 8); } while (0)

static volatile float vf[16] = {1.5f, -2.25f, 0.1f, 3.0f, 1e-40f, -0.0f, 3.4e38f, 7.0f,
                                -1.0f, 0.333333f, 1e10f, -1e-10f, 2.5f, -2.5f, 1.0f, 65504.0f};
static volatile double vd[8] = {1.0 / 3.0, -2.0, 1e308, 4.9e-324, 0.1, -0.0, 2.5, -3.5};
static volatile u32 vbits[8] = {0x7fc00000u, 0xffc00001u, 0x7f800000u, 0xff800000u, 0x00000001u, 0x80000000u, 0x7f7fffffu, 0x3f800000u};
static volatile u8 vb[32] = {0, 1, 2, 3, 250, 251, 252, 253, 127, 128, 129, 255, 7, 77, 177, 200,
                             9, 19, 29, 39, 49, 59, 69, 79, 89, 99, 109, 119, 129, 139, 149, 159};
static volatile i32 vi[8] = {1, -1, 0x7fffffff, (i32)0x80000000, 12345, -54321, 3, -3};

static __m128 lf(int o) { return _mm_setr_ps(vf[o], vf[o + 1], vf[o + 2], vf[o + 3]); }
static __m128d ld(int o) { return _mm_setr_pd(vd[o], vd[o + 1]); }
static __m128i lb(int o) { __m128i r; u8* p = (u8*)&r; for (int i = 0; i < 16; i++) p[i] = vb[o + i]; return r; }
static __m128i li(int o) { return _mm_setr_epi32(vi[o], vi[o + 1], vi[o + 2], vi[o + 3]); }
static __m128 lnan(void) { __m128i r = _mm_setr_epi32(vbits[0], vbits[1], vbits[2], vbits[3]); return _mm_castsi128_ps(r); }

static void sse(void) {
  __m128 a = lf(0), b = lf(4), c = lf(8), n = lnan();
  OUT128("addps", _mm_add_ps(a, b));
  OUT128("subps", _mm_sub_ps(a, c));
  OUT128("mulps", _mm_mul_ps(a, c));
  OUT128("divps", _mm_div_ps(a, c));
  OUT128("sqrtps", _mm_sqrt_ps(_mm_setr_ps(vf[0], vf[3], vf[7], vf[2])));
  OUT128("minps_nan", _mm_min_ps(n, a));
  OUT128("minps_nan_rev", _mm_min_ps(a, n));
  OUT128("maxps_nan", _mm_max_ps(n, a));
  OUT128("addps_nan_inf", _mm_add_ps(n, n));
  OUT128("cmpltps", _mm_cmplt_ps(a, c));
  OUT128("cmpunordps", _mm_cmpunord_ps(n, a));
  OUT128("cvtps2dq", _mm_cvtps_epi32(_mm_setr_ps(vf[0], vf[13], vf[10], vf[12])));
  OUT128("cvttps2dq", _mm_cvttps_epi32(_mm_setr_ps(vf[0], vf[13], vf[10], vf[12])));
  OUT128("cvtdq2ps", _mm_cvtepi32_ps(li(0)));
  OUT128("shufps", _mm_shuffle_ps(a, b, 0x1b));
  OUT128("unpcklps", _mm_unpacklo_ps(a, c));
  OUT64("movmskps", _mm_movemask_ps(_mm_setr_ps(vf[1], vf[5], vf[0], vf[8])));
  OUT64("ucomiss_flags", ({ u64 f; float x = vf[0], y = vf[3]; __asm__ volatile("ucomiss %1, %2\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(x), "x"(y) : "cc"); f & 0x8d5; }));
  OUT64("ucomiss_nan_flags", ({ u64 f; float x = vf[0]; u32 nb = vbits[0]; float y; __builtin_memcpy(&y, &nb, 4); __asm__ volatile("ucomiss %1, %2\n\tpushfq\n\tpopq %0" : "=r"(f) : "x"(x), "x"(y) : "cc"); f & 0x8d5; }));
}

static void sse2(void) {
  __m128d a = ld(0), b = ld(2), c = ld(4);
  OUT128("addpd", _mm_add_pd(a, c));
  OUT128("mulpd_overflow", _mm_mul_pd(b, b));
  OUT128("divpd", _mm_div_pd(a, c));
  OUT128("sqrtpd", _mm_sqrt_pd(_mm_setr_pd(vd[4], vd[6])));
  OUT128("cvtpd2dq", _mm_cvtpd_epi32(_mm_setr_pd(vd[6], vd[7])));
  OUT128("cvttpd2dq", _mm_cvttpd_epi32(_mm_setr_pd(vd[6], vd[7])));
  OUT128("cvtpd2ps", _mm_cvtpd_ps(a));
  OUT128("cvtps2pd", _mm_cvtps_pd(lf(0)));
  OUT64("cvtsd2si", _mm_cvtsd_si64(_mm_set_sd(vd[6])));
  OUT64("cvttsd2si_big", _mm_cvttsd_si64(_mm_set_sd(vd[2])));
  __m128i x = lb(0), y = lb(16);
  OUT128("paddb", _mm_add_epi8(x, y));
  OUT128("paddusb", _mm_adds_epu8(x, y));
  OUT128("psubsb", _mm_subs_epi8(x, y));
  OUT128("pmullw", _mm_mullo_epi16(x, y));
  OUT128("pmulhw", _mm_mulhi_epi16(x, y));
  OUT128("pmulhuw", _mm_mulhi_epu16(x, y));
  OUT128("pmaddwd", _mm_madd_epi16(x, y));
  OUT128("pmuludq", _mm_mul_epu32(li(0), li(4)));
  OUT128("psadbw", _mm_sad_epu8(x, y));
  OUT128("pavgb", _mm_avg_epu8(x, y));
  OUT128("packsswb", _mm_packs_epi16(x, y));
  OUT128("packuswb", _mm_packus_epi16(x, y));
  OUT128("packssdw", _mm_packs_epi32(li(0), li(4)));
  OUT128("pcmpgtb", _mm_cmpgt_epi8(x, y));
  OUT128("pcmpeqd", _mm_cmpeq_epi32(li(0), li(4)));
  OUT128("punpckhbw", _mm_unpackhi_epi8(x, y));
  OUT128("pshufd", _mm_shuffle_epi32(li(0), 0x6c));
  OUT128("pshuflw", _mm_shufflelo_epi16(x, 0xb1));
  OUT128("psllq_var", _mm_sll_epi64(x, _mm_cvtsi32_si128(vi[6] + 4)));
  OUT128("psrad_big", _mm_srai_epi32(li(0), 31));
  OUT128("psrldq", _mm_srli_si128(x, 5));
  OUT64("pmovmskb", _mm_movemask_epi8(x));
  OUT64("pextrw", _mm_extract_epi16(x, 5));
}

static void sse3_4(void) {
  __m128i x = lb(0), y = lb(16);
  OUT128("haddps", _mm_hadd_ps(lf(0), lf(4)));
  OUT128("pshufb", _mm_shuffle_epi8(x, y));
  OUT128("palignr", _mm_alignr_epi8(x, y, 7));
  OUT128("pmaddubsw", _mm_maddubs_epi16(x, y));
  OUT128("pmulhrsw", _mm_mulhrs_epi16(x, y));
  OUT128("pabsb", _mm_abs_epi8(x));
  OUT128("psignb", _mm_sign_epi8(x, y));
  OUT128("dpps", _mm_dp_ps(lf(0), lf(8), 0xf1));
  OUT128("roundps_floor", _mm_round_ps(_mm_setr_ps(vf[0], vf[1], vf[12], vf[13]), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
  OUT128("roundps_nearest", _mm_round_ps(_mm_setr_ps(vf[0], vf[1], vf[12], vf[13]), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
  OUT128("blendvps", _mm_blendv_ps(lf(0), lf(4), lf(8)));
  OUT128("pminsd", _mm_min_epi32(li(0), li(4)));
  OUT128("pmaxud", _mm_max_epu32(li(0), li(4)));
  OUT128("pmulld", _mm_mullo_epi32(li(0), li(4)));
  OUT128("pmovsxbw", _mm_cvtepi8_epi16(x));
  OUT128("pmovzxbd", _mm_cvtepu8_epi32(y));
  OUT128("packusdw", _mm_packus_epi32(li(0), li(4)));
  OUT128("insertps", _mm_insert_ps(lf(0), lf(4), 0x9c));
  OUT64("ptest_flags", (u64)_mm_testz_si128(x, y) | ((u64)_mm_testc_si128(x, y) << 1));
  OUT64("phminposuw", (u64)_mm_cvtsi128_si32(_mm_minpos_epu16(x)));
  OUT64("crc32q", _mm_crc32_u64(0xffffffffu, 0x0123456789abcdefull ^ (u64)vi[4]));
  OUT64("popcnt", (u64)__builtin_popcountll(0xf0f0f0f00f0f0f0full ^ (u64)vi[5]));
  OUT128("pcmpistri_idx", _mm_cvtsi32_si128(_mm_cmpistri(x, y, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY)));
  OUT128("pclmulqdq", _mm_clmulepi64_si128(x, y, 0x11));
  OUT128("aesenc", _mm_aesenc_si128(x, y));
  OUT128("aesdeclast", _mm_aesdeclast_si128(x, y));
}

static void avx(void) {
  __m256 a = _mm256_setr_ps(vf[0], vf[1], vf[2], vf[3], vf[4], vf[5], vf[6], vf[7]);
  __m256 b = _mm256_setr_ps(vf[8], vf[9], vf[10], vf[11], vf[12], vf[13], vf[14], vf[15]);
  OUT256("vaddps", _mm256_add_ps(a, b));
  OUT256("vmulps", _mm256_mul_ps(a, b));
  OUT256("vfmadd231ps", _mm256_fmadd_ps(a, b, a));
  OUT256("vfmsub_pd", _mm256_fmsub_pd(_mm256_setr_pd(vd[0], vd[1], vd[4], vd[6]), _mm256_setr_pd(vd[4], vd[6], vd[0], vd[1]), _mm256_setr_pd(vd[6], vd[7], vd[1], vd[0])));
  OUT256("vpermps", _mm256_permutevar8x32_ps(a, _mm256_setr_epi32(7, 6, 5, 4, 0, 1, 2, 3)));
  OUT256("vperm2f128", _mm256_permute2f128_ps(a, b, 0x21));
  OUT256("vbroadcastss", _mm256_broadcast_ss((const float*)&vf[9]));
  OUT256("vpaddd", _mm256_add_epi32(_mm256_setr_epi32(vi[0], vi[1], vi[2], vi[3], vi[4], vi[5], vi[6], vi[7]), _mm256_set1_epi32(vi[2])));
  OUT256("vpshufb", _mm256_shuffle_epi8(_mm256_setr_m128i(lb(0), lb(16)), _mm256_setr_m128i(lb(16), lb(0))));
  OUT256("vpsravd", _mm256_srav_epi32(_mm256_setr_epi32(vi[0], vi[1], vi[2], vi[3], vi[4], vi[5], vi[6], vi[7]), _mm256_setr_epi32(0, 1, 31, 32, 4, 5, 33, 2)));
  OUT256("vpgatherdd", _mm256_i32gather_epi32((const int*)vi, _mm256_setr_epi32(7, 0, 3, 3, 1, 6, 2, 5), 4));
  OUT256("vcvtph2ps", _mm256_cvtph_ps(_mm_setr_epi16(0x3c00, (short)0xc000, 0x7bff, 0x0001, (short)0x8000, 0x7c00, 0x3555, (short)0xfc00)));
  OUT128("vcvtps2ph", _mm256_cvtps_ph(a, 0));
  OUT64("vmovmskps", _mm256_movemask_ps(b));
  OUT64("vtestz", _mm256_testz_si256(_mm256_castps_si256(a), _mm256_castps_si256(b)));
  __m256 m = _mm256_castsi256_ps(_mm256_setr_epi32(-1, 0, -1, 0, 0, -1, 0, -1));
  OUT256("vmaskmovps_load", _mm256_maskload_ps((const float*)vf, _mm256_castps_si256(m)));
}

static void x87(void) {
  double r;
  __asm__ volatile("fldl %1\n\tfldl %2\n\tfmulp\n\tfldl %3\n\tfaddp\n\tfstpl %0" : "=m"(r) : "m"(vd[0]), "m"(vd[4]), "m"(vd[6]));
  OUT64("x87_mul_add", *(u64*)&r);
  __asm__ volatile("fldl %1\n\tfsqrt\n\tfstpl %0" : "=m"(r) : "m"(vd[6]));
  OUT64("x87_sqrt", *(u64*)&r);
  __asm__ volatile("fld1\n\tfldl %1\n\tfdivrp\n\tfstpl %0" : "=m"(r) : "m"(vd[0]));
  OUT64("x87_div", *(u64*)&r);
  long double e;
  __asm__ volatile("fldl %1\n\tfldl %2\n\tfdivrp\n\tfstpt %0" : "=m"(e) : "m"(vd[0]), "m"(vd[6]));
  put_line("x87_div80", &e, 12);
  i64 q;
  u16 cw, cwz;
  __asm__ volatile("fnstcw %0" : "=m"(cw));
  cwz = cw | 0x0c00; // round toward zero
  __asm__ volatile("fldcw %2\n\tfldl %1\n\tfistpll %0\n\tfldcw %3" : "=m"(q) : "m"(vd[7]), "m"(cwz), "m"(cw));
  OUT64("x87_fistp_trunc", q);
  __asm__ volatile("fldl %1\n\tfistpll %0" : "=m"(q) : "m"(vd[7]));
  OUT64("x87_fistp_nearest", q);
  u16 sw;
  __asm__ volatile("fldz\n\tfldl %1\n\tfcompp\n\tfnstsw %0" : "=m"(sw) : "m"(vd[1]));
  OUT64("x87_fcompp_sw", sw & 0x4500);
  __asm__ volatile("fldpi\n\tfstpl %0" : "=m"(r));
  OUT64("x87_fldpi", *(u64*)&r);
  __asm__ volatile("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(r) : "m"(vd[0]));
  OUT64("x87_fsin", *(u64*)&r);
  __asm__ volatile("fldl %1\n\tf2xm1\n\tfstpl %0" : "=m"(r) : "m"(vd[0]));
  OUT64("x87_f2xm1", *(u64*)&r);
  __asm__ volatile("fldl %1\n\tfld1\n\tfyl2x\n\tfstpl %0" : "=m"(r) : "m"(vd[6]));
  OUT64("x87_fyl2x", *(u64*)&r);
}

static void mxcsr(void) {
  u32 old = _mm_getcsr();
  _mm_setcsr((old & ~0x6000u) | 0x2000u); // round down
  OUT128("cvtps2dq_round_down", _mm_cvtps_epi32(_mm_setr_ps(vf[0], vf[13], vf[1], vf[12])));
  OUT128("addps_round_down", _mm_add_ps(_mm_setr_ps(vf[2], vf[2], vf[9], vf[9]), _mm_setr_ps(vf[0], vf[1], vf[0], vf[1])));
  _mm_setcsr(old | 0x8040u); // FTZ and DAZ
  OUT128("mulps_ftz_daz", _mm_mul_ps(_mm_setr_ps(vf[4], vf[4], vf[0], vf[11]), _mm_setr_ps(vf[0], vf[3], vf[4], vf[11])));
  _mm_setcsr(old);
  OUT128("mulps_denormal", _mm_mul_ps(_mm_setr_ps(vf[4], vf[4], vf[0], vf[11]), _mm_setr_ps(vf[0], vf[3], vf[4], vf[11])));
}

static int run(void) {
  puts_("w03_simd transcript\n");
  sse();
  sse2();
  sse3_4();
  avx();
  x87();
  mxcsr();
  puts_("w03_simd end\n");
  return 0;
}
WITNESS_MAIN(run)
