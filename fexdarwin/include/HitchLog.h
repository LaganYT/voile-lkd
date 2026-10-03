// SPDX-License-Identifier: MIT
// Hitch log (FEXDARWIN_HITCH_LOG=1): engine work that can stall a guest thread long enough to drop frames, measured
// inside the process instead of by an external sampler (which suspends every thread and causes hitches itself).
// FEXCore accumulates translation time per thread once the host gives the thread a ThreadStats block; fexdarwin times
// SMC write faults and code invalidations. A watcher thread looks at 50 ms windows and logs, with the wall-clock time,
// each window where one of them took more than 8 ms, and a summary every 10 s.
//
// Profile (FEXDARWIN_PROFILE=1, period FEXDARWIN_PROFILE_MS, 10000 by default): the same watcher logs where the process spent its time: CPU of the process
// and of each guest thread (named by its guest comm), guest system calls (count and time inside LKD, which includes
// blocking waits) with the ones that took each thread the most time, translation time, and the system calls that took
// the process the most time.
#pragma once

#include <FEXCore/Debug/InternalThreadState.h>

#include <cstdint>

struct GuestThread;

namespace HitchLog {
bool Enabled();        // hitch log or profile: threads get a ThreadStats block, SMC and invalidations are timed
bool ProfileEnabled();
// Gives the thread a ThreadStats block (FEXCore then accumulates its JIT time); starts the watcher in this process.
void Attach(FEXCore::Core::InternalThreadState* Thread);
// In the child of a host fork: starts the child's watcher (the parent's did not survive the fork).
void AfterFork();
// Profile: logs the time since the last 10 s profile now (process exit, exec of a new image).
void LogProfileNow(const char* Why);

enum class Kind { SmcFault, Invalidate };
// Counts SMC write faults per 16 KiB host page; the 10 s summary names the pages that fault most (module and offset).
void NoteSmcPage(uint64_t GuestAddress);
void Add(Kind K, uint64_t Ns, uint64_t Bytes = 0);
uint64_t NowNs();
// Profile: one guest system call of thread T, Ns nanoseconds (wall time inside LKD).
void NoteSyscall(GuestThread* T, uint64_t Nr, uint64_t Ns);
// Profile: a path call that failed with ENOENT; one in FEXDARWIN_PROFILE_MISSES (1000) is logged with its path.
void NoteMissingPath(uint64_t Nr, const uint64_t* Args);

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
