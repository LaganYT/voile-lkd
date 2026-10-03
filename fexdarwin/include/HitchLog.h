// SPDX-License-Identifier: MIT
// Hitch log (FEXDARWIN_HITCH_LOG=1): engine work that can stall a guest thread long enough to drop frames, measured
// inside the process instead of by an external sampler (which suspends every thread and causes hitches itself).
// FEXCore accumulates translation time per thread once the host gives the thread a ThreadStats block; fexdarwin times
// SMC write faults and code invalidations. A watcher thread looks at 50 ms windows and logs, with the wall-clock time,
// each window where one of them took more than 8 ms, and a summary every 10 s.
#pragma once

#include <FEXCore/Debug/InternalThreadState.h>

#include <cstdint>

namespace HitchLog {
bool Enabled();
// Gives the thread a ThreadStats block (FEXCore then accumulates its JIT time); starts the watcher in this process.
void Attach(FEXCore::Core::InternalThreadState* Thread);

enum class Kind { SmcFault, Invalidate };
// Counts SMC write faults per 16 KiB host page; the 10 s summary names the pages that fault most (module and offset).
void NoteSmcPage(uint64_t GuestAddress);
void Add(Kind K, uint64_t Ns, uint64_t Bytes = 0);
uint64_t NowNs();

// Times a scope when the log is on.
class Scope {
public:
  Scope(Kind K, uint64_t Bytes = 0)
    : K(K)
    , Bytes(Bytes)
    , T0(Enabled() ? NowNs() : 0) {}
  ~Scope() {
    if (T0) Add(K, NowNs() - T0, Bytes);
  }

private:
  Kind K;
  uint64_t Bytes;
  uint64_t T0;
};
} // namespace HitchLog
