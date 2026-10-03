// SPDX-License-Identifier: MIT
#include "HitchLog.h"
#include "Host.h"

#include <FEXCore/Utils/SHMStats.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

extern "C" int fexdarwin_lkd_vma_describe(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot,
                                          uint64_t* off, char* name, unsigned long name_len);

namespace HitchLog {
namespace {
std::mutex StatsLock;
std::vector<FEXCore::SHMStats::ThreadStats*> AllStats; // never freed: a thread's counts stay in the sums after it ends
std::atomic<pid_t> WatcherPid {0};
std::atomic<uint64_t> SmcNs, SmcCount, InvalNs, InvalCount, InvalBytes;

// Faulting pages: open addressing, lock-free (the SMC handler runs in a signal handler). A full table drops new pages.
constexpr unsigned PageSlots = 1024;
std::atomic<uint64_t> PageKey[PageSlots];
std::atomic<uint64_t> PageCount[PageSlots];

void LogTopPages() {
  struct Entry { uint64_t Page, Count; };
  std::vector<Entry> E;
  for (unsigned I = 0; I < PageSlots; ++I) {
    const uint64_t N = PageCount[I].exchange(0, std::memory_order_relaxed);
    if (N) E.push_back({PageKey[I].load(std::memory_order_relaxed) & ~uint64_t {1}, N});
  }
  if (E.empty()) return;
  std::sort(E.begin(), E.end(), [](const Entry& A, const Entry& B) { return A.Count > B.Count; });
  lkd_task* Task = nullptr;
  {
    std::lock_guard<std::mutex> Lk(G->ThreadsLock);
    for (GuestThread* T : G->Threads)
      if (T->Task) { Task = T->Task; break; }
  }
  for (size_t I = 0; I < E.size() && I < 8; ++I) {
    uint64_t S = 0, End = 0, Off = 0;
    uint32_t Prot = 0;
    char Name[256] = "?";
    if (Task) fexdarwin_lkd_vma_describe(Task, E[I].Page, &S, &End, &Prot, &Off, Name, sizeof Name);
    fprintf(stderr, "[fexdarwin] pid %d   SMC page %#llx: %llu faults, %s+%#llx (vma %#llx-%#llx prot %u)\n", static_cast<int>(getpid()),
            (unsigned long long)E[I].Page, (unsigned long long)E[I].Count, Name[0] ? Name : "[anon]",
            (unsigned long long)(E[I].Page - S + Off), (unsigned long long)S, (unsigned long long)End, Prot);
  }
}

struct Totals {
  uint64_t JitNs, JitCount, CacheMiss, SmcNs, SmcCount, InvalNs, InvalCount, InvalBytes;
};

uint64_t CounterHz() {
  uint64_t F;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(F));
  return F;
}

Totals Read() {
  static const uint64_t Hz = CounterHz();
  Totals T {};
  uint64_t Cycles = 0;
  {
    std::lock_guard<std::mutex> Lk(StatsLock);
    for (auto* S : AllStats) {
      Cycles += std::atomic_ref<uint64_t>(S->AccumulatedJITTime).load(std::memory_order_relaxed);
      T.JitCount += std::atomic_ref<uint64_t>(S->AccumulatedJITCount).load(std::memory_order_relaxed);
      T.CacheMiss += std::atomic_ref<uint64_t>(S->AccumulatedCacheMissCount).load(std::memory_order_relaxed);
    }
  }
  T.JitNs = Hz ? static_cast<uint64_t>(static_cast<double>(Cycles) * 1e9 / static_cast<double>(Hz)) : 0;
  T.SmcNs = SmcNs.load(std::memory_order_relaxed);
  T.SmcCount = SmcCount.load(std::memory_order_relaxed);
  T.InvalNs = InvalNs.load(std::memory_order_relaxed);
  T.InvalCount = InvalCount.load(std::memory_order_relaxed);
  T.InvalBytes = InvalBytes.load(std::memory_order_relaxed);
  return T;
}

void Log(const char* What, const Totals& A, const Totals& B) {
  struct timeval Tv;
  gettimeofday(&Tv, nullptr);
  struct tm Lt;
  localtime_r(&Tv.tv_sec, &Lt);
  fprintf(stderr,
          "[fexdarwin] pid %d %s at %02d:%02d:%02d.%03d: JIT %.1f ms (%llu blocks, %llu lookup misses), SMC faults %llu (%.1f ms), "
          "invalidations %llu (%.1f ms, %llu KiB)\n",
          static_cast<int>(getpid()), What, Lt.tm_hour, Lt.tm_min, Lt.tm_sec, static_cast<int>(Tv.tv_usec / 1000),
          (B.JitNs - A.JitNs) / 1e6, (unsigned long long)(B.JitCount - A.JitCount), (unsigned long long)(B.CacheMiss - A.CacheMiss),
          (unsigned long long)(B.SmcCount - A.SmcCount), (B.SmcNs - A.SmcNs) / 1e6, (unsigned long long)(B.InvalCount - A.InvalCount),
          (B.InvalNs - A.InvalNs) / 1e6, (unsigned long long)((B.InvalBytes - A.InvalBytes) / 1024));
}

void* Watch(void*) {
  pthread_setname_np("fexdarwin-hitch-log");
  constexpr uint64_t Threshold = 8000000;
  Totals Prev = Read(), Summary = Prev;
  for (unsigned Tick = 1;; ++Tick) {
    usleep(50000);
    const Totals Now = Read();
    if (Now.JitNs - Prev.JitNs > Threshold || Now.SmcNs - Prev.SmcNs > Threshold || Now.InvalNs - Prev.InvalNs > Threshold) {
      Log("50 ms window", Prev, Now);
    }
    if (Tick % 200 == 0) {
      if (Now.JitCount != Summary.JitCount || Now.SmcCount != Summary.SmcCount) {
        Log("10 s", Summary, Now);
        if (Now.SmcCount - Summary.SmcCount >= 100) LogTopPages();
      }
      Summary = Now;
    }
    Prev = Now;
  }
  return nullptr;
}
} // namespace

bool Enabled() {
  static const bool On = [] {
    const char* E = getenv("FEXDARWIN_HITCH_LOG");
    return E && *E && strcmp(E, "0") != 0;
  }();
  return On;
}

uint64_t NowNs() {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void Attach(FEXCore::Core::InternalThreadState* Thread) {
  if (!Enabled() || !Thread) return;
  // The watcher reads the list under StatsLock: a fork while it holds the lock must not leave it locked in the child.
  static std::once_flag AtFork;
  std::call_once(AtFork, [] {
    pthread_atfork([] { StatsLock.lock(); }, [] { StatsLock.unlock(); }, [] { StatsLock.unlock(); });
  });
  auto* S = static_cast<FEXCore::SHMStats::ThreadStats*>(calloc(1, sizeof(FEXCore::SHMStats::ThreadStats)));
  if (!S) return;
  {
    std::lock_guard<std::mutex> Lk(StatsLock);
    AllStats.push_back(S);
  }
  Thread->ThreadStats = S;
  // Threads do not survive fork: a forked process starts its own watcher with its first new thread.
  const pid_t Me = getpid();
  pid_t Was = WatcherPid.load();
  if (Was != Me && WatcherPid.compare_exchange_strong(Was, Me)) {
    pthread_t W;
    if (pthread_create(&W, nullptr, Watch, nullptr) == 0) pthread_detach(W);
  }
}

void NoteSmcPage(uint64_t GuestAddress) {
  if (!Enabled()) return;
  const uint64_t Page = (GuestAddress & ~uint64_t {0x3fff}) | 1; // | 1: never 0, the empty key
  unsigned H = static_cast<unsigned>((Page >> 14) * 0x9E3779B97F4A7C15ull >> 54) % PageSlots;
  for (unsigned Probe = 0; Probe < 16; ++Probe, H = (H + 1) % PageSlots) {
    uint64_t K = PageKey[H].load(std::memory_order_relaxed);
    if (K == 0 && PageKey[H].compare_exchange_strong(K, Page)) K = Page;
    if (K == Page) {
      PageCount[H].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

void Add(Kind K, uint64_t Ns, uint64_t Bytes) {
  if (K == Kind::SmcFault) {
    SmcNs.fetch_add(Ns, std::memory_order_relaxed);
    SmcCount.fetch_add(1, std::memory_order_relaxed);
  } else {
    InvalNs.fetch_add(Ns, std::memory_order_relaxed);
    InvalCount.fetch_add(1, std::memory_order_relaxed);
    InvalBytes.fetch_add(Bytes, std::memory_order_relaxed);
  }
}
} // namespace HitchLog
