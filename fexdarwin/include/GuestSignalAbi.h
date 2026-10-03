// SPDX-License-Identifier: MIT
// x86-64 Linux signal frame layout (kernel view), used to deliver synchronous faults to guest handlers and to implement
// rt_sigreturn. Layout verified against arch/x86/include/uapi/asm/sigcontext.h and signal_64.c (sizes asserted below).
#pragma once
#include "LinuxAbi.h"

#include <cstddef>
#include <cstdint>

namespace Linux {
struct SigContext64 {
  uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
  uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
  uint64_t eflags;
  uint16_t cs, gs, fs, ss;
  uint64_t err;
  uint64_t trapno;
  uint64_t oldmask;
  uint64_t cr2;
  uint64_t fpstate; // pointer to the FP state in the frame
  uint64_t reserved1[8];
};
static_assert(sizeof(SigContext64) == 256);
static_assert(offsetof(SigContext64, rip) == 128 && offsetof(SigContext64, eflags) == 136 && offsetof(SigContext64, err) == 152 &&
              offsetof(SigContext64, fpstate) == 184);

constexpr uint64_t UC_FP_XSTATE = 1, UC_SIGCONTEXT_SS = 2, UC_STRICT_RESTORE_SS = 4;

struct UContext64 {
  uint64_t uc_flags;
  uint64_t uc_link;
  StackT uc_stack;
  SigContext64 uc_mcontext;
  uint64_t uc_sigmask;
};
static_assert(sizeof(UContext64) == 304);

struct SigInfo64 {
  int32_t si_signo;
  int32_t si_errno;
  int32_t si_code;
  int32_t pad;
  uint64_t si_addr; // union member used by SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP
  uint8_t rest[128 - 24];
};
static_assert(sizeof(SigInfo64) == 128);

struct RtSigFrame {
  uint64_t pretcode;
  UContext64 uc;
  SigInfo64 info;
};
static_assert(sizeof(RtSigFrame) == 440);

// FXSAVE area (512 bytes) followed by the XSAVE header and the AVX upper halves, as the kernel lays it out.
struct FxSave {
  uint16_t fcw, fsw;
  uint8_t ftw, rsv0;
  uint16_t fop;
  uint64_t rip, rdp;
  uint32_t mxcsr, mxcsr_mask;
  uint8_t st[8][16];
  uint8_t xmm[16][16];
  uint8_t rsv1[48];
  // software reserved bytes 464..511 (struct _fpx_sw_bytes)
  uint32_t magic1;
  uint32_t extended_size;
  uint64_t xfeatures;
  uint32_t xstate_size;
  uint32_t padding[7];
};
static_assert(sizeof(FxSave) == 512);
static_assert(offsetof(FxSave, st) == 32 && offsetof(FxSave, xmm) == 160 && offsetof(FxSave, magic1) == 464);

struct __attribute__((packed)) XState {
  FxSave fx;
  uint64_t xstate_bv;
  uint64_t xcomp_bv;
  uint8_t rsv[48];
  uint8_t ymmh[16][16];
  uint32_t magic2; // FP_XSTATE_MAGIC2, directly after the XSAVE area
};
static_assert(offsetof(XState, ymmh) == 576 && sizeof(XState) == 836);
constexpr uint32_t FP_XSTATE_MAGIC1 = 0x46505853, FP_XSTATE_MAGIC2 = 0x46505845;

// Hardware trap numbers and page fault error bits stored in sigcontext.
constexpr uint64_t TRAP_DE = 0, TRAP_DB = 1, TRAP_BP = 3, TRAP_OF = 4, TRAP_UD = 6, TRAP_GP = 13, TRAP_PF = 14;
constexpr uint64_t PF_PROT = 1, PF_WRITE = 2, PF_USER = 4, PF_INSTR = 16;
constexpr int SEGV_MAPERR_ = 1, SEGV_ACCERR_ = 2, SI_KERNEL_ = 0x80;
} // namespace Linux
