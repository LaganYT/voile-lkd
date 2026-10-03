// SPDX-License-Identifier: MIT
// Access to the arm64 Darwin ucontext_t (mcontext64) used by the signal handlers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <signal.h>
#include <sys/ucontext.h>

namespace DarwinCtx {
// __darwin_arm_thread_state64 is { __x[29]; __fp; __lr; __sp; __pc; __cpsr; __flags }: x0..x28, x29, x30, sp, pc are
// contiguous, which lets the FEXCore helpers (ManuallyLoadJumpBuf, HandleUnalignedAccess) index them as GPRs[0..31].
static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * 8);
static_assert(offsetof(__darwin_arm_thread_state64, __lr) == 30 * 8);
static_assert(offsetof(__darwin_arm_thread_state64, __sp) == 31 * 8);
static_assert(offsetof(__darwin_arm_thread_state64, __pc) == 32 * 8);

inline __darwin_arm_thread_state64& SS(ucontext_t* Uc) {
  return Uc->uc_mcontext->__ss;
}
inline __darwin_arm_neon_state64& NS(ucontext_t* Uc) {
  return Uc->uc_mcontext->__ns;
}
inline uint64_t* GPRs(ucontext_t* Uc) {
  return reinterpret_cast<uint64_t*>(&SS(Uc).__x[0]);
}
inline uint64_t GetPc(ucontext_t* Uc) {
  return SS(Uc).__pc;
}
inline void SetPc(ucontext_t* Uc, uint64_t Pc) {
  SS(Uc).__pc = Pc;
}
inline uint64_t GetSp(ucontext_t* Uc) {
  return SS(Uc).__sp;
}
inline void SetSp(ucontext_t* Uc, uint64_t Sp) {
  SS(Uc).__sp = Sp;
}
inline uint64_t GetReg(ucontext_t* Uc, int I) {
  return GPRs(Uc)[I]; // 0..30, 31 is SP
}
inline void SetReg(ucontext_t* Uc, int I, uint64_t V) {
  GPRs(Uc)[I] = V;
}
inline __uint128_t GetFpr(ucontext_t* Uc, int I) {
  return NS(Uc).__v[I];
}
inline void SetFpr(ucontext_t* Uc, int I, __uint128_t V) {
  NS(Uc).__v[I] = V;
}
inline __uint128_t* Fprs(ucontext_t* Uc) {
  return reinterpret_cast<__uint128_t*>(&NS(Uc).__v[0]);
}
inline uint32_t GetPState(ucontext_t* Uc) {
  return SS(Uc).__cpsr;
}
// Exception state: __esr is the ESR_EL1 syndrome and __far the fault address (measured, evidence probe-sig1-001.log).
inline uint32_t GetEsr(ucontext_t* Uc) {
  return Uc->uc_mcontext->__es.__esr;
}
inline uint64_t GetFar(ucontext_t* Uc) {
  return Uc->uc_mcontext->__es.__far;
}
// macOS delivers SIGBUS with si_code 1 for permission faults, execute faults AND alignment faults, so si_code cannot tell
// them apart. The data abort fault status code in the ESR can: 0x21 is an alignment fault.
inline bool IsDataAlignmentFault(ucontext_t* Uc) {
  const uint32_t Esr = GetEsr(Uc);
  return (Esr >> 26) == 0x24 /* data abort from a lower exception level */ && (Esr & 0x3f) == 0x21;
}
} // namespace DarwinCtx
