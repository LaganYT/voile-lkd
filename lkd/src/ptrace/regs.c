/* regs.c: the x86-64 register view rules of ptrace. Owner: ptrace.
 *
 * Models arch/x86/kernel/ptrace.c of Linux v6.12: getreg()/putreg() (word offsets of struct
 * user_regs_struct), set_segment_reg(), set_flags() (FLAG_MASK), the FS/GS base limit, and
 * ptrace_get_debugreg()/ptrace_set_debugreg(); arch/x86/kernel/fpu/regset.c for the FXSAVE and XSTATE
 * write checks (xfpregs_set, xstateregs_set, validate_user_xstate_header).
 */
#include "pt_priv.h"

/* FLAG_MASK_32: CF PF AF ZF SF TF DF OF RF AC; the other bits of eflags keep their value. */
#define FLAG_MASK 0x50dd5ull
#define USER_RPL 3u
#define DR6_RESERVED 0xffff0ff0ull

uint64_t pt_getreg(const lkd_regs *r, unsigned off)
{
    const uint64_t *w = (const uint64_t *)r;
    uint64_t v = w[off / 8];
    switch (off / 8) {
    case LX_REG_CS: case LX_REG_SS: case LX_REG_DS: case LX_REG_ES: case LX_REG_FS: case LX_REG_GS:
        return v & 0xffff; /* get_segment_reg(): the selectors are 16-bit */
    default:
        return v;
    }
}

/* invalid_selector(): a non-null selector must have RPL 3. */
static int invalid_selector(uint16_t v) { return v != 0 && (v & 3u) != USER_RPL; }

int pt_putreg(lkd_regs *r, unsigned off, uint64_t val)
{
    uint64_t *w = (uint64_t *)r;
    unsigned i = off / 8;
    switch (i) {
    case LX_REG_FS: case LX_REG_GS: case LX_REG_DS: case LX_REG_ES: {
        uint16_t sel = (uint16_t)val; /* set_segment_reg() takes a u16 */
        if (invalid_selector(sel)) return -LX_EIO;
        w[i] = sel;
        return 0;
    }
    case LX_REG_CS: case LX_REG_SS: {
        uint16_t sel = (uint16_t)val;
        if (invalid_selector(sel) || sel == 0) return -LX_EIO; /* "can't actually change these in 64-bit mode" */
        w[i] = sel;
        return 0;
    }
    case LX_REG_EFLAGS:
        w[i] = (w[i] & ~FLAG_MASK) | (val & FLAG_MASK);
        return 0;
    case LX_REG_FS_BASE: case LX_REG_GS_BASE:
        if (val >= PT_TASK_SIZE_MAX) return -LX_EIO;
        w[i] = val;
        return 0;
    default:
        w[i] = val;
        return 0;
    }
}

uint64_t pt_get_debugreg(const struct pt_slot *s, unsigned n)
{
    if (n < 4) return s->dr[n];
    if (n == 6) return s->dr[6] ^ DR6_RESERVED;   /* virtual_dr6 is kept in inverted polarity */
    if (n == 7) return s->dr[7];
    return 0;
}

/* No hardware breakpoint exists behind the engine: an address can be stored (a disabled breakpoint, as
 * Linux registers it) but DR7 cannot enable one. That write fails visibly with EIO. */
int pt_set_debugreg(struct pt_slot *s, unsigned n, uint64_t val)
{
    if (n < 4) {
        /* hw_breakpoint_arch_parse(): a user breakpoint in kernel space is EINVAL */
        if (val >= PT_TASK_SIZE_MAX) return -LX_EINVAL;
        s->dr[n] = val;
        return 0;
    }
    if (n == 6) { s->dr[6] = val ^ DR6_RESERVED; return 0; }
    if (n == 7) {
        if (val & 0xffull) {
            lkd_log_once("ptrace: POKEUSER DR7 enabling a hardware breakpoint: unsupported by the engine (EIO)");
            return -LX_EIO;
        }
        s->dr[7] = val;
        return 0;
    }
    return -LX_EIO; /* DR4 and DR5 do not exist */
}

/* mxcsr_feature_mask: the MXCSR_MASK field of the FXSAVE image, 0x0000ffbf when it is zero. */
uint32_t pt_mxcsr_mask(const uint8_t *fx)
{
    uint32_t m;
    memcpy(&m, fx + 28, 4);
    return m ? m : 0x0000ffbfu;
}

/* validate_user_xstate_header() plus the MXCSR check of copy_uabi_to_xstate(). cur is the current image
 * (its software-reserved bytes 464..511 carry the supported feature mask when FP_XSTATE_MAGIC1 is
 * there). Returns 0 or -LX_EINVAL. */
int pt_check_xstate(const uint8_t *cur, const uint8_t *img, size_t len)
{
    if (len < 576) return -LX_EINVAL;
    uint64_t xfeatures, xcomp;
    memcpy(&xfeatures, img + 512, 8);
    memcpy(&xcomp, img + 520, 8);
    if (xcomp) return -LX_EINVAL;
    for (int i = 528; i < 576; i++) if (img[i]) return -LX_EINVAL;
    uint32_t magic1;
    memcpy(&magic1, cur + 464, 4);
    if (magic1 == 0x46505853u) { /* FP_XSTATE_MAGIC1: struct _fpx_sw_bytes is present */
        uint64_t supported;
        memcpy(&supported, cur + 464 + 8, 8);
        if (xfeatures & ~supported) return -LX_EINVAL;
    }
    if (xfeatures & 3u) { /* XFEATURE_MASK_FP | XFEATURE_MASK_SSE */
        uint32_t mx;
        memcpy(&mx, img + 24, 4);
        if (mx & ~pt_mxcsr_mask(cur)) return -LX_EINVAL;
    }
    return 0;
}
