// SPDX-License-Identifier: MIT
#include "GuestMemory.h"
#include <FEXCore/Utils/GuestBase.h>
#include "LinuxAbi.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

int GuestMemory::HostProt(int G) {
  int H = 0;
  if (G & (Linux::PROT_READ_ | Linux::PROT_WRITE_ | Linux::PROT_EXEC_)) {
    H |= PROT_READ; // executable-only guest memory still has to be readable: the translator reads it as data
  }
  if (G & Linux::PROT_WRITE_) {
    H |= PROT_WRITE;
  }
  return H;
}

bool GuestMemory::Reserve(uint64_t B, uint64_t S, std::string* Err) {
  void* P = ::mmap(FEXCore::G2H(B), S, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (P == MAP_FAILED) {
    if (Err) *Err = std::string("reserve guest window failed: ") + strerror(errno);
    return false;
  }
  if (P != FEXCore::G2H(B)) {
    char Buf[160];
    snprintf(Buf, sizeof Buf, "kernel placed the guest window at %p instead of %#llx", P, (unsigned long long)B);
    ::munmap(P, S);
    if (Err) *Err = Buf;
    return false;
  }
  Base = B;
  Size = S;
  return true;
}

void GuestMemory::SplitAt(uint64_t Addr) {
  auto It = VMAs.upper_bound(Addr);
  if (It == VMAs.begin()) return;
  --It;
  if (It->first < Addr && Addr < It->second.End) {
    VMA Tail = It->second;
    It->second.End = Addr;
    VMAs.emplace(Addr, Tail);
  }
}

void GuestMemory::EraseRange(uint64_t Start, uint64_t End) {
  SplitAt(Start);
  SplitAt(End);
  VMAs.erase(VMAs.lower_bound(Start), VMAs.lower_bound(End));
  SmcPages.erase(SmcPages.lower_bound(Start), SmcPages.lower_bound(End));
}

void GuestMemory::InsertRange(uint64_t Start, uint64_t End, int Prot, const std::string& Name) {
  VMA V;
  V.End = End;
  V.Prot = Prot;
  V.Name = Name;
  VMAs[Start] = V;
}

bool GuestMemory::RangeFree(uint64_t Start, uint64_t End) {
  auto It = VMAs.lower_bound(Start);
  if (It != VMAs.end() && It->first < End) return false;
  if (It != VMAs.begin()) {
    --It;
    if (It->second.End > Start) return false;
  }
  return true;
}

bool GuestMemory::RangeMapped(uint64_t Start, uint64_t End) {
  uint64_t Cur = Start;
  auto It = VMAs.upper_bound(Start);
  if (It != VMAs.begin()) --It;
  while (It != VMAs.end() && Cur < End) {
    if (It->first > Cur) return false;
    if (It->second.End > Cur) Cur = It->second.End;
    ++It;
  }
  return Cur >= End;
}

uint64_t GuestMemory::FindFreeTopDown(uint64_t Len) {
  uint64_t Cur = MmapTop;
  while (Cur - MmapFloor >= Len) {
    auto It = VMAs.lower_bound(Cur);
    if (It == VMAs.begin()) {
      return Cur - Len;
    }
    --It;
    const uint64_t VEnd = std::min(It->second.End, Cur);
    if (Cur - VEnd >= Len) {
      return Cur - Len;
    }
    Cur = It->first;
  }
  return 0;
}

void GuestMemory::ReleaseToReservation(uint64_t Start, uint64_t End) {
  // Replace the mapping with a fresh PROT_NONE reservation: the pages are returned to the system and the window stays
  // reserved so nobody else (malloc, the JIT) can take the addresses.
  ::mmap(FEXCore::G2H(Start), End - Start, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
}

int64_t GuestMemory::Mmap(uint64_t Addr, uint64_t Len, int Prot, int Flags, int Fd, int64_t Off, const char* Name) {
  using namespace Linux;
  if (Len == 0) return -L_EINVAL;
  const bool Fixed = (Flags & (MAP_FIXED_ | MAP_FIXED_NOREPLACE_)) != 0;
  const bool Anon = (Flags & MAP_ANONYMOUS_) != 0;
  const int Type = Flags & MAP_TYPE_;
  if (Type != MAP_PRIVATE_ && Type != MAP_SHARED_) return -L_EINVAL;
  if (!Anon && Type == MAP_SHARED_) return -L_ENOSYS; // shared file mappings are not implemented, never faked
  if (Len > (1ULL << 46)) return -L_ENOMEM;
  const uint64_t Rounded = RoundUp(Len);

  std::lock_guard<std::mutex> lk(Lock);
  uint64_t Where = 0;
  if (Fixed) {
    if (Addr & PageMask) return -L_EINVAL; // limitation: 4 KiB aligned but not 16 KiB aligned fixed mappings
    if (!InWindow(Addr, Rounded)) return -L_ENOMEM;
    if ((Flags & MAP_FIXED_NOREPLACE_) && !RangeFree(Addr, Addr + Rounded)) return -L_EEXIST;
    Where = Addr;
  } else {
    if (Addr && !(Addr & PageMask) && InWindow(Addr, Rounded) && RangeFree(Addr, Addr + Rounded)) {
      Where = Addr;
    } else {
      Where = FindFreeTopDown(Rounded);
      if (!Where) return -L_ENOMEM;
    }
  }

  const bool NeedFill = !Anon;
  void* P = ::mmap(FEXCore::G2H(Where), Rounded, NeedFill ? (PROT_READ | PROT_WRITE) : HostProt(Prot),
                   MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
  if (P == MAP_FAILED) return -FromDarwinErrno(errno);
  if (NeedFill) {
    // File mapping emulated by a private copy (read-only semantics are exact, writes are not propagated to the file).
    size_t Done = 0;
    while (Done < Len) {
      ssize_t R = ::pread(Fd, static_cast<uint8_t*>(FEXCore::G2H(Where)) + Done, Len - Done, Off + Done);
      if (R < 0) {
        const int E = errno;
        ReleaseToReservation(Where, Where + Rounded);
        return -FromDarwinErrno(E);
      }
      if (R == 0) break; // past end of file: remaining bytes stay zero
      Done += R;
    }
    if (::mprotect(FEXCore::G2H(Where), Rounded, HostProt(Prot)) != 0) {
      const int E = errno;
      ReleaseToReservation(Where, Where + Rounded);
      return -FromDarwinErrno(E);
    }
  }
  EraseRange(Where, Where + Rounded);
  InsertRange(Where, Where + Rounded, Prot, Name ? Name : "");
  return static_cast<int64_t>(Where);
}

int64_t GuestMemory::Munmap(uint64_t Addr, uint64_t Len) {
  using namespace Linux;
  if ((Addr & PageMask) || Len == 0) return -L_EINVAL;
  const uint64_t Rounded = RoundUp(Len);
  if (!InWindow(Addr, Rounded)) return -L_EINVAL;
  std::lock_guard<std::mutex> lk(Lock);
  EraseRange(Addr, Addr + Rounded);
  ReleaseToReservation(Addr, Addr + Rounded);
  return 0;
}

int64_t GuestMemory::Mprotect(uint64_t Addr, uint64_t Len, int Prot) {
  using namespace Linux;
  if (Addr & PageMask) return -L_EINVAL;
  if (Prot & ~(PROT_READ_ | PROT_WRITE_ | PROT_EXEC_)) return -L_EINVAL;
  if (Len == 0) return 0;
  const uint64_t Rounded = RoundUp(Len);
  if (!InWindow(Addr, Rounded)) return -L_ENOMEM;
  std::lock_guard<std::mutex> lk(Lock);
  if (!RangeMapped(Addr, Addr + Rounded)) return -L_ENOMEM;
  SplitAt(Addr);
  SplitAt(Addr + Rounded);
  for (auto It = VMAs.lower_bound(Addr); It != VMAs.end() && It->first < Addr + Rounded; ++It) {
    It->second.Prot = Prot;
  }
  // Pages that translated code depends on stay write-protected on the host until a write faults.
  for (uint64_t Pg = Addr; Pg < Addr + Rounded; Pg += PageSize) {
    int H = HostProt(Prot);
    if (SmcPages.count(Pg)) H &= ~PROT_WRITE;
    if (::mprotect(FEXCore::G2H(Pg), PageSize, H) != 0) return -FromDarwinErrno(errno);
  }
  return 0;
}

int GuestMemory::MapFixedAnon(uint64_t Addr, uint64_t Len, int Prot, const char* Name, bool* Widened) {
  using namespace Linux;
  const uint64_t Start = RoundDown(Addr);
  const uint64_t End = RoundUp(Addr + Len);
  if (!InWindow(Start, End - Start)) return -L_ENOMEM;
  std::lock_guard<std::mutex> lk(Lock);
  if (Widened) *Widened = false;
  for (uint64_t Pg = Start; Pg < End; Pg += PageSize) {
    auto It = VMAs.upper_bound(Pg);
    bool Mapped = false;
    if (It != VMAs.begin()) {
      --It;
      Mapped = It->first <= Pg && Pg < It->second.End;
    }
    if (Mapped) {
      // Two segments share one 16 KiB host page: keep the union of the protections.
      SplitAt(Pg);
      SplitAt(Pg + PageSize);
      auto V = VMAs.find(Pg);
      const int Union = V->second.Prot | Prot;
      if (Union != V->second.Prot) {
        V->second.Prot = Union;
        ::mprotect(FEXCore::G2H(Pg), PageSize, HostProt(Union) | PROT_WRITE); // keep writable while loading
        if (Widened) *Widened = true;
      }
      continue;
    }
    void* P = ::mmap(FEXCore::G2H(Pg), PageSize, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    if (P == MAP_FAILED) return -FromDarwinErrno(errno);
    InsertRange(Pg, Pg + PageSize, Prot, Name ? Name : "");
  }
  return 0;
}

int GuestMemory::SetProt(uint64_t Addr, uint64_t Len, int Prot) {
  return static_cast<int>(Mprotect(RoundDown(Addr), RoundUp(Addr + Len) - RoundDown(Addr), Prot));
}

void GuestMemory::SetupBrk(uint64_t Start, uint64_t MaxSize) {
  BrkStart = BrkCur = RoundUp(Start);
  BrkMax = BrkStart + MaxSize;
}

uint64_t GuestMemory::Brk(uint64_t NewBrk) {
  if (NewBrk == 0 || NewBrk < BrkStart || NewBrk > BrkMax) return BrkCur;
  const uint64_t OldEnd = RoundUp(BrkCur);
  const uint64_t NewEnd = RoundUp(NewBrk);
  if (NewEnd > OldEnd) {
    bool W = false;
    if (MapFixedAnon(OldEnd, NewEnd - OldEnd, Linux::PROT_READ_ | Linux::PROT_WRITE_, "[heap]", &W) != 0) return BrkCur;
    // MapFixedAnon leaves new pages readable and writable which is the final protection of the heap.
  } else if (NewEnd < OldEnd) {
    Munmap(NewEnd, OldEnd - NewEnd);
  }
  BrkCur = NewBrk;
  return BrkCur;
}

GuestMemory::ExecRange GuestMemory::QueryExec(uint64_t Addr) {
  std::lock_guard<std::mutex> lk(Lock);
  auto It = VMAs.upper_bound(Addr);
  if (It == VMAs.begin()) return {0, 0, false};
  --It;
  if (Addr >= It->second.End || !(It->second.Prot & Linux::PROT_EXEC_)) return {0, 0, false};
  return {It->first, It->second.End - It->first, (It->second.Prot & Linux::PROT_WRITE_) != 0};
}

bool GuestMemory::Check(uint64_t Addr, uint64_t Len, int Need) {
  if (Len == 0) return true;
  std::lock_guard<std::mutex> lk(Lock);
  uint64_t Cur = Addr;
  const uint64_t End = Addr + Len;
  if (End < Addr) return false;
  auto It = VMAs.upper_bound(Addr);
  if (It == VMAs.begin()) return false;
  --It;
  while (Cur < End) {
    if (It == VMAs.end() || It->first > Cur || It->second.End <= Cur) return false;
    if ((It->second.Prot & Need) != Need) return false;
    Cur = It->second.End;
    ++It;
  }
  return true;
}

int GuestMemory::ProtAt(uint64_t Addr) {
  std::lock_guard<std::mutex> lk(Lock);
  auto It = VMAs.upper_bound(Addr);
  if (It == VMAs.begin()) return -1;
  --It;
  return Addr < It->second.End ? It->second.Prot : -1;
}

void GuestMemory::MarkSmc(uint64_t Start, uint64_t Len) {
  const uint64_t B = RoundDown(Start);
  const uint64_t E = RoundUp(Start + Len);
  std::lock_guard<std::mutex> lk(Lock);
  for (uint64_t Pg = B; Pg < E; Pg += PageSize) {
    auto It = VMAs.upper_bound(Pg);
    if (It == VMAs.begin()) continue;
    --It;
    if (Pg >= It->second.End || !(It->second.Prot & Linux::PROT_WRITE_)) continue;
    if (SmcPages.insert(Pg).second) {
      ::mprotect(FEXCore::G2H(Pg), PageSize, HostProt(It->second.Prot) & ~PROT_WRITE);
    }
  }
}

bool GuestMemory::IsSmcPage(uint64_t Addr) {
  std::lock_guard<std::mutex> lk(Lock);
  return SmcPages.count(RoundDown(Addr)) != 0;
}

void GuestMemory::SmcPagesIn(uint64_t Addr, uint64_t Len, std::set<uint64_t>* Pages) {
  std::lock_guard<std::mutex> lk(Lock);
  for (auto It = SmcPages.lower_bound(RoundDown(Addr)); It != SmcPages.end() && *It < Addr + Len; ++It) {
    Pages->insert(*It);
  }
}

void GuestMemory::ClearSmcPages(uint64_t Addr, uint64_t Len) {
  std::lock_guard<std::mutex> lk(Lock);
  auto It = SmcPages.lower_bound(RoundDown(Addr));
  while (It != SmcPages.end() && *It < Addr + Len) {
    const uint64_t Pg = *It;
    auto V = VMAs.upper_bound(Pg);
    if (V != VMAs.begin()) {
      --V;
      if (Pg < V->second.End) {
        ::mprotect(FEXCore::G2H(Pg), PageSize, HostProt(V->second.Prot));
      }
    }
    It = SmcPages.erase(It);
  }
}

std::string GuestMemory::Dump() {
  std::lock_guard<std::mutex> lk(Lock);
  std::string Out;
  char Buf[96];
  for (auto& [S, V] : VMAs) {
    snprintf(Buf, sizeof Buf, "  %#014llx-%#014llx %c%c%c ", (unsigned long long)S, (unsigned long long)V.End,
             (V.Prot & 1) ? 'r' : '-', (V.Prot & 2) ? 'w' : '-', (V.Prot & 4) ? 'x' : '-');
    Out += Buf;
    Out += V.Name;
    Out += '\n';
  }
  return Out;
}
