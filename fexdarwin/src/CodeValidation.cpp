// SPDX-License-Identifier: MIT
#include "CodeValidation.h"

#include <atomic>
#include <time.h>

namespace CodeValidation {
namespace {
constexpr uint64_t HostPageSize = 16384;
// 128 faults in 100 ms: a page has to take more than 1,280 write faults per second, for 100 ms, to leave write tracking.
// Measured: the EAC module's page takes ~33,000 per second; Rocket League in a match takes about 4 per 10 s in all.
constexpr uint32_t ValidateAfter = 128;
constexpr uint64_t WindowNs = 100'000'000;

// Open addressing on the page, never deleted until exec. A full neighbourhood leaves the page on write tracking.
constexpr unsigned Slots = 4096;
constexpr unsigned Probes = 32;
struct Slot {
  std::atomic<uint64_t> Key; // page | 1; 0 = empty
  std::atomic<uint64_t> WindowStart;
  std::atomic<uint32_t> Count;
  std::atomic<bool> On;
};
Slot Table[Slots];
std::atomic<uint32_t> ValidatedPages;
std::atomic<bool> Used; // a slot was taken since the last Reset

Slot* Find(uint64_t Page, bool Insert) {
  const uint64_t Key = Page | 1;
  unsigned H = static_cast<unsigned>(((Page / HostPageSize) * 0x9E3779B97F4A7C15ull) >> 52) % Slots;
  for (unsigned P = 0; P < Probes; ++P, H = (H + 1) % Slots) {
    uint64_t K = Table[H].Key.load(std::memory_order_acquire);
    if (K == 0) {
      if (!Insert) {
        return nullptr;
      }
      if (Table[H].Key.compare_exchange_strong(K, Key)) {
        Used.store(true, std::memory_order_relaxed);
        return &Table[H];
      }
      // K is now the key another thread put here.
    }
    if (K == Key) {
      return &Table[H];
    }
  }
  return nullptr;
}
} // namespace

bool NoteWriteFault(uint64_t HostPage) {
  Slot* S = Find(HostPage, true);
  if (!S || S->On.load(std::memory_order_relaxed)) {
    return false;
  }
  const uint64_t Now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
  if (Now - S->WindowStart.load(std::memory_order_relaxed) > WindowNs) {
    S->WindowStart.store(Now, std::memory_order_relaxed);
    S->Count.store(1, std::memory_order_relaxed);
    return false;
  }
  if (S->Count.fetch_add(1, std::memory_order_relaxed) + 1 < ValidateAfter) {
    return false;
  }
  bool Was = false;
  if (!S->On.compare_exchange_strong(Was, true)) {
    return false;
  }
  ValidatedPages.fetch_add(1);
  return true;
}

bool Validated(uint64_t Start, uint64_t Len) {
  if (ValidatedPages.load(std::memory_order_acquire) == 0 || Len == 0) {
    return false;
  }
  for (uint64_t P = Start & ~(HostPageSize - 1); P < Start + Len; P += HostPageSize) {
    const Slot* S = Find(P, false);
    if (S && S->On.load(std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void Reset() {
  ValidatedPages.store(0);
  if (!Used.exchange(false)) {
    return;
  }
  for (auto& S : Table) {
    S.On.store(false, std::memory_order_relaxed);
    S.Count.store(0, std::memory_order_relaxed);
    S.WindowStart.store(0, std::memory_order_relaxed);
    S.Key.store(0, std::memory_order_release);
  }
}
} // namespace CodeValidation
