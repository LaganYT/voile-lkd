// SPDX-License-Identifier: MIT
// Engine side of the LKD contract. See src/LkdGlue.cpp.
#pragma once
#include <lkd/lkd.h>

struct GuestThread;
namespace FEXCore::Core {
struct CpuStateFrame;
struct InternalThreadState;
} // namespace FEXCore::Core

bool LkdEnabled();                      // FEXDARWIN_LKD=1
int LkdSetup();                         // installs the kick handler and calls lkd_init
void LkdHandleSyscall(GuestThread* T, FEXCore::Core::CpuStateFrame* Frame);
// Host entry that delivers pending LKD signals and re-enters the dispatcher. A signal handler resumes a thread here with
// x0 = GuestThread*, x28 = its state frame, the host stack at ReturningStackLocation and the guest registers spilled.
extern "C" void fexdarwin_kick_trampoline();

// FP state helpers shared with GuestSignals.cpp (Linux XSAVE layout, GuestSignalAbi.h).
namespace Linux {
struct XState;
}
void SaveGuestFpState(FEXCore::Core::InternalThreadState* Th, Linux::XState* Xs);
void RestoreGuestFpState(FEXCore::Core::InternalThreadState* Th, const Linux::XState* Xs);
void ResetGuestFpForHandler(FEXCore::Core::InternalThreadState* Th); // clean FPU and SIMD state at handler entry
