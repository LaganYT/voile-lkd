// SPDX-License-Identifier: MIT
#include "HitchLog.h"
#include "Host.h"

#include <FEXCore/Utils/SHMStats.h>
extern "C" {
#include <lkd/lkd.h>
#include <lkd/syscall_nr.h>
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <mach/mach.h>
#include <mutex>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int fexdarwin_lkd_vma_describe(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot,
                                          uint64_t* off, char* name, unsigned long name_len);
extern "C" void fexdarwin_lkd_task_comm(lkd_task* t, char* out, unsigned long len);

namespace HitchLog {
namespace {
bool Switch(const char* Name) {
  const char* E = getenv(Name);
  return E && *E && strcmp(E, "0") != 0;
}

bool HitchesEnabled() {
  static const bool On = Switch("FEXDARWIN_HITCH_LOG");
  return On;
}

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

// Profile: system calls per number (count, wall nanoseconds inside LKD).
std::atomic<uint64_t> SysCount[LKD_NR_TABLE_SIZE], SysNs[LKD_NR_TABLE_SIZE];

uint64_t CpuNs(const struct timeval& Tv) {
  return static_cast<uint64_t>(Tv.tv_sec) * 1000000000ull + static_cast<uint64_t>(Tv.tv_usec) * 1000ull;
}

double Ms(uint64_t Ns) {
  return Ns / 1e6;
}

class Profile {
public:
  Profile() { Sample(true); }

  // Logs the time since the previous call (10 s, or less at exit and exec). Wait: false skips the per-thread rows
  // when another thread holds the thread list (exit must not hang on it).
  void Log(const char* Why, bool Wait = true) {
    struct ThreadRow {
      int32_t Tid;
      char Comm[16];
      uint64_t Cpu, Sys, SysNs, Jit;
      std::string Calls; // the thread's system calls that took it the most time
    };
    struct SysRow {
      unsigned Nr;
      uint64_t Count, Ns;
    };
    const Snapshot Prev = Last;
    Sample(Wait);
    std::vector<ThreadRow> Rows;
    for (const auto& [Tid, Now] : Last.Threads) {
      const auto It = Prev.Threads.find(Tid);
      const ThreadSample Was = It == Prev.Threads.end() ? ThreadSample {} : It->second;
      ThreadRow R {Tid, {}, Now.Cpu - Was.Cpu, Now.Sys - Was.Sys, Now.SysNs - Was.SysNs, Now.Jit - Was.Jit, {}};
      memcpy(R.Comm, Now.Comm, sizeof R.Comm);
      if (!Now.PerNr.empty()) {
        std::vector<std::pair<uint64_t, unsigned>> ByNs;
        for (unsigned Nr = 0; Nr < LKD_NR_TABLE_SIZE; ++Nr) {
          const uint64_t Ns = Now.PerNr[LKD_NR_TABLE_SIZE + Nr] - (Was.PerNr.empty() ? 0 : Was.PerNr[LKD_NR_TABLE_SIZE + Nr]);
          if (Ns) ByNs.emplace_back(Ns, Nr);
        }
        std::sort(ByNs.begin(), ByNs.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
        for (size_t I = 0; I < ByNs.size() && I < 5; ++I) {
          const unsigned Nr = ByNs[I].second;
          const uint64_t N = Now.PerNr[Nr] - (Was.PerNr.empty() ? 0 : Was.PerNr[Nr]);
          const char* Name = lkd_syscall_name(Nr);
          char B[96];
          snprintf(B, sizeof B, "%s%s %llu/%.1f ms", I ? ", " : "", Name ? Name : "?", (unsigned long long)N, Ms(ByNs[I].first));
          R.Calls += B;
        }
      }
      Rows.push_back(std::move(R));
    }
    std::sort(Rows.begin(), Rows.end(), [](const ThreadRow& A, const ThreadRow& B) { return A.Cpu > B.Cpu; });
    std::vector<SysRow> Calls;
    uint64_t AllCalls = 0, AllNs = 0;
    for (unsigned Nr = 0; Nr < LKD_NR_TABLE_SIZE; ++Nr) {
      const SysRow R {Nr, Last.SysCount[Nr] - Prev.SysCount[Nr], Last.SysNs[Nr] - Prev.SysNs[Nr]};
      AllCalls += R.Count;
      AllNs += R.Ns;
      if (R.Count) Calls.push_back(R);
    }
    std::sort(Calls.begin(), Calls.end(), [](const SysRow& A, const SysRow& B) { return A.Ns > B.Ns; });

    struct tm Lt;
    const time_t Sec = Last.Wall.tv_sec;
    localtime_r(&Sec, &Lt);
    const int Pid = static_cast<int>(getpid());
    fprintf(stderr,
            "[fexdarwin] pid %d profile (%s) %.2f s to %02d:%02d:%02d.%03d \"%s\": cpu user %.1f ms sys %.1f ms, syscalls %llu "
            "(%.1f ms inside), JIT %.1f ms (%llu blocks), SMC faults %llu (%.1f ms), invalidations %llu (%.1f ms)\n",
            Pid, Why, (Last.Mono - Prev.Mono) / 1e9, Lt.tm_hour, Lt.tm_min, Lt.tm_sec, static_cast<int>(Last.Wall.tv_usec / 1000),
            Last.MainComm, Ms(Last.User - Prev.User), Ms(Last.Kernel - Prev.Kernel), (unsigned long long)AllCalls, Ms(AllNs),
            Ms(Last.T.JitNs - Prev.T.JitNs), (unsigned long long)(Last.T.JitCount - Prev.T.JitCount),
            (unsigned long long)(Last.T.SmcCount - Prev.T.SmcCount), Ms(Last.T.SmcNs - Prev.T.SmcNs),
            (unsigned long long)(Last.T.InvalCount - Prev.T.InvalCount), Ms(Last.T.InvalNs - Prev.T.InvalNs));
    for (size_t I = 0; I < Rows.size() && I < 10; ++I) {
      if (!Rows[I].Cpu && !Rows[I].Sys) break;
      fprintf(stderr, "[fexdarwin] pid %d   thread %d \"%s\": cpu %.1f ms, syscalls %llu (%.1f ms inside), JIT %.1f ms; %s\n", Pid,
              Rows[I].Tid, Rows[I].Comm, Ms(Rows[I].Cpu), (unsigned long long)Rows[I].Sys, Ms(Rows[I].SysNs), Ms(Rows[I].Jit),
              Rows[I].Calls.c_str());
    }
    for (size_t I = 0; I < Calls.size() && I < 12; ++I) {
      const char* Name = lkd_syscall_name(Calls[I].Nr);
      fprintf(stderr, "[fexdarwin] pid %d   syscall %s: %llu calls, %.1f ms (%.1f us each)\n", Pid, Name ? Name : "?",
              (unsigned long long)Calls[I].Count, Ms(Calls[I].Ns), Calls[I].Ns / 1e3 / static_cast<double>(Calls[I].Count));
    }
  }

private:
  struct ThreadSample {
    char Comm[16];
    uint64_t Cpu, Sys, SysNs, Jit;
    std::vector<uint64_t> PerNr; // counts then nanoseconds per number, empty before the thread's first call
  };
  struct Snapshot {
    uint64_t Mono {}, User {}, Kernel {};
    struct timeval Wall {};
    Totals T {};
    char MainComm[16] {};
    std::unordered_map<int32_t, ThreadSample> Threads;
    uint64_t SysCount[LKD_NR_TABLE_SIZE] {}, SysNs[LKD_NR_TABLE_SIZE] {};
  };
  Snapshot Last;

  void Sample(bool Wait) {
    static const uint64_t Hz = CounterHz();
    Snapshot S;
    S.Mono = NowNs();
    gettimeofday(&S.Wall, nullptr);
    struct rusage Ru;
    getrusage(RUSAGE_SELF, &Ru);
    S.User = CpuNs(Ru.ru_utime);
    S.Kernel = CpuNs(Ru.ru_stime);
    S.T = Read();
    for (unsigned Nr = 0; Nr < LKD_NR_TABLE_SIZE; ++Nr) {
      S.SysCount[Nr] = SysCount[Nr].load(std::memory_order_relaxed);
      S.SysNs[Nr] = SysNs[Nr].load(std::memory_order_relaxed);
    }
    // A thread in the list is alive: EndGuestThread takes it out under the lock before releasing its task.
    std::unique_lock<std::mutex> Lk(G->ThreadsLock, std::defer_lock);
    if (Wait) Lk.lock();
    if (Lk.owns_lock() || Lk.try_lock()) {
      for (GuestThread* T : G->Threads) {
        ThreadSample Ts {};
        fexdarwin_lkd_task_comm(T->Task, Ts.Comm, sizeof Ts.Comm);
        if (T->Task && lkd_task_tid(T->Task) == lkd_task_pid(T->Task)) memcpy(S.MainComm, Ts.Comm, sizeof Ts.Comm);
        if (T->PThread) {
          thread_basic_info_data_t Info;
          mach_msg_type_number_t Count = THREAD_BASIC_INFO_COUNT;
          const mach_port_t Port = pthread_mach_thread_np(T->PThread);
          if (thread_info(Port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&Info), &Count) == KERN_SUCCESS)
            Ts.Cpu = static_cast<uint64_t>(Info.user_time.seconds + Info.system_time.seconds) * 1000000000ull +
                     static_cast<uint64_t>(Info.user_time.microseconds + Info.system_time.microseconds) * 1000ull;
        }
        Ts.Sys = T->SysCount.load(std::memory_order_relaxed);
        Ts.SysNs = T->SysNs.load(std::memory_order_relaxed);
        if (const std::atomic<uint64_t>* Per = T->SysNr.load(std::memory_order_acquire)) {
          Ts.PerNr.resize(2 * LKD_NR_TABLE_SIZE);
          for (unsigned I = 0; I < 2 * LKD_NR_TABLE_SIZE; ++I) Ts.PerNr[I] = Per[I].load(std::memory_order_relaxed);
        }
        if (auto* St = T->State ? T->State->ThreadStats : nullptr; St && Hz)
          Ts.Jit = static_cast<uint64_t>(static_cast<double>(std::atomic_ref<uint64_t>(St->AccumulatedJITTime).load(std::memory_order_relaxed)) *
                                         1e9 / static_cast<double>(Hz));
        S.Threads[T->Tid] = std::move(Ts);
      }
    }
    if (!S.MainComm[0]) memcpy(S.MainComm, Last.MainComm, sizeof S.MainComm); // at exit the list is empty
    Last = std::move(S);
  }
};

// Watcher ticks (50 ms) per profile: FEXDARWIN_PROFILE_MS, 10 s by default, at least one tick.
unsigned ProfileTicks() {
  static const unsigned Ticks = [] {
    const char* E = getenv("FEXDARWIN_PROFILE_MS");
    const long Ms = E && *E ? atol(E) : 10000;
    return static_cast<unsigned>(Ms < 50 ? 1 : Ms / 50);
  }();
  return Ticks;
}

// The profile of this process (ProfPid): a forked child inherits its parent's and replaces it in its own watcher.
std::mutex ProfLock;
Profile* Prof;
pid_t ProfPid;

void* Watch(void*) {
  pthread_setname_np("fexdarwin-hitch-log");
  constexpr uint64_t Threshold = 8000000;
  const bool Hitches = HitchesEnabled();
  if (ProfileEnabled()) {
    std::lock_guard<std::mutex> Lk(ProfLock);
    Prof = new Profile(); // never freed: the exit log may still use it
    ProfPid = getpid();
  }
  Totals Prev = Read(), Summary = Prev;
  for (unsigned Tick = 1;; ++Tick) {
    usleep(50000);
    const Totals Now = Read();
    if (ProfileEnabled() && Tick % ProfileTicks() == 0) {
      std::lock_guard<std::mutex> Lk(ProfLock);
      Prof->Log("period");
    }
    if (!Hitches) {
      Prev = Now;
      continue;
    }
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
// Threads do not survive fork: a forked process starts its own watcher (AfterFork, or its first new thread).
void StartWatcher() {
  const pid_t Me = getpid();
  pid_t Was = WatcherPid.load();
  if (Was != Me && WatcherPid.compare_exchange_strong(Was, Me)) {
    pthread_t W;
    if (pthread_create(&W, nullptr, Watch, nullptr) == 0) pthread_detach(W);
  }
}
} // namespace

bool Enabled() {
  return HitchesEnabled() || ProfileEnabled();
}

bool ProfileEnabled() {
  static const bool On = Switch("FEXDARWIN_PROFILE");
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
  StartWatcher();
}

void AfterFork() {
  if (!Enabled()) return;
  // The parent's lock state and profile do not belong to this process.
  new (&ProfLock) std::mutex();
  Prof = nullptr;
  StartWatcher();
}

void LogProfileNow(const char* Why) {
  if (!ProfileEnabled()) return;
  std::unique_lock<std::mutex> Lk(ProfLock, std::try_to_lock);
  if (Lk.owns_lock() && Prof && ProfPid == getpid()) Prof->Log(Why, false);
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

void NoteSyscall(GuestThread* T, uint64_t Nr, uint64_t Ns) {
  T->SysCount.fetch_add(1, std::memory_order_relaxed);
  T->SysNs.fetch_add(Ns, std::memory_order_relaxed);
  if (Nr >= LKD_NR_TABLE_SIZE) return;
  SysCount[Nr].fetch_add(1, std::memory_order_relaxed);
  SysNs[Nr].fetch_add(Ns, std::memory_order_relaxed);
  std::atomic<uint64_t>* Per = T->SysNr.load(std::memory_order_acquire);
  if (!Per) {
    Per = new std::atomic<uint64_t>[2 * LKD_NR_TABLE_SIZE]();
    T->SysNr.store(Per, std::memory_order_release);
  }
  Per[Nr].fetch_add(1, std::memory_order_relaxed);
  Per[LKD_NR_TABLE_SIZE + Nr].fetch_add(Ns, std::memory_order_relaxed);
}

void NoteMissingPath(uint64_t Nr, const uint64_t* Args) {
  static const uint64_t Every = [] {
    const char* E = getenv("FEXDARWIN_PROFILE_MISSES");
    const long N = E && *E ? atol(E) : 1000;
    return static_cast<uint64_t>(N < 1 ? 1 : N);
  }();
  static std::atomic<uint64_t> Count {0};
  int Dirfd;
  uint64_t Path;
  switch (Nr) {
  case LKD_NR_openat:
  case LKD_NR_newfstatat:
  case LKD_NR_statx: Dirfd = static_cast<int>(Args[0]); Path = Args[1]; break;
  case LKD_NR_getxattr:
  case LKD_NR_lgetxattr: Dirfd = -100; Path = Args[0]; break;
  default: return;
  }
  if (Count.fetch_add(1, std::memory_order_relaxed) % Every) return;
  char Name[512];
  if (lkd_user_strn(Path, Name, sizeof Name) < 0) return;
  const char* Call = lkd_syscall_name(Nr);
  fprintf(stderr, "[fexdarwin] pid %d missing path (%s, dirfd %d): %s\n", static_cast<int>(getpid()), Call ? Call : "?", Dirfd,
          Name);
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
