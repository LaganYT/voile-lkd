// SPDX-License-Identifier: MIT
// Guest address space manager for the fexdarwin host (identity mapping: guest address == host address).
//
// macOS arm64 pages are 16 KiB. Every guest mapping is therefore rounded to 16 KiB, even though x86-64 Linux
// guests think in 4 KiB pages. Guest executable memory is never given PROT_EXEC on the host: FEXCore reads it as data
// and translates it, the CPU never runs it.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

class GuestMemory {
public:
  static constexpr uint64_t PageSize = 16384;
  static constexpr uint64_t PageMask = PageSize - 1;

  struct VMA {
    uint64_t End {};   // exclusive
    int Prot {};       // guest protection, Linux values (1 read, 2 write, 4 exec)
    std::string Name;  // diagnostic only
  };

  // Reserve [Base, Base + Size) with PROT_NONE. Returns false and fills Err on failure.
  bool Reserve(uint64_t Base, uint64_t Size, std::string* Err);

  uint64_t WindowBase() const { return Base; }
  uint64_t WindowEnd() const { return Base + Size; }
  bool InWindow(uint64_t Addr, uint64_t Len) const { return Addr >= Base && Addr + Len >= Addr && Addr + Len <= Base + Size; }

  // Where mmap(NULL) allocations stop (top-down); set once before any guest mapping.
  void SetMmapTop(uint64_t Top) { MmapTop = Top; }
  void SetMmapFloor(uint64_t Floor) { MmapFloor = Floor; }

  // Linux semantics, result is a guest address or -errno (Linux errno).
  int64_t Mmap(uint64_t Addr, uint64_t Len, int Prot, int LinuxFlags, int Fd, int64_t Off, const char* Name = "");
  int64_t Munmap(uint64_t Addr, uint64_t Len);
  int64_t Mprotect(uint64_t Addr, uint64_t Len, int Prot);

  // Map exactly [Addr, Addr + Len) anonymously with the given protection (used by the loader). Addr and Len are rounded
  // outward to 16 KiB. If the range overlaps an existing mapping the existing protection is widened (union) and the
  // function returns 1 so the caller can report it. Returns 0 on success, -errno on failure.
  int MapFixedAnon(uint64_t Addr, uint64_t Len, int Prot, const char* Name, bool* Widened);
  // Change the guest protection of an already mapped range.
  int SetProt(uint64_t Addr, uint64_t Len, int Prot);

  // brk() emulation. The heap region is reserved at SetupBrk.
  void SetupBrk(uint64_t Start, uint64_t MaxSize);
  uint64_t Brk(uint64_t NewBrk);

  // Frontend: range of executable memory that contains Addr (Size == 0 when Addr is not executable).
  struct ExecRange {
    uint64_t Base, Size;
    bool Writable;
  };
  ExecRange QueryExec(uint64_t Addr);

  // Self-modifying code tracking. MarkSmc write-protects the host pages covering [Start, Start + Len) when the guest
  // allows writes there (the guest protection is kept in the VMA table).
  void MarkSmc(uint64_t Start, uint64_t Len);
  // True when the page containing Addr is currently write-protected on behalf of translated code.
  bool IsSmcPage(uint64_t Addr);
  // Page bases of the SMC-protected pages intersecting [Addr, Addr + Len).
  void SmcPagesIn(uint64_t Addr, uint64_t Len, std::set<uint64_t>* Pages);
  // Restore the guest protection of every SMC-protected page intersecting [Addr, Addr + Len). The caller must already
  // hold the code invalidation lock and have invalidated the translations of that range.
  void ClearSmcPages(uint64_t Addr, uint64_t Len);

  // True when the whole range is mapped with at least the given guest protection.
  bool Check(uint64_t Addr, uint64_t Len, int NeedProt);
  // Guest protection of the page containing Addr, or -1 when the guest has nothing mapped there.
  int ProtAt(uint64_t Addr);

  // Debug: render the table.
  std::string Dump();

  static uint64_t RoundUp(uint64_t V) { return (V + PageMask) & ~PageMask; }
  static uint64_t RoundDown(uint64_t V) { return V & ~PageMask; }
  static int HostProt(int GuestProt);

private:
  using Map = std::map<uint64_t, VMA>;
  std::mutex Lock;
  Map VMAs;
  uint64_t Base {};
  uint64_t Size {};
  uint64_t MmapTop {};
  uint64_t MmapFloor {};
  uint64_t BrkStart {};
  uint64_t BrkCur {};
  uint64_t BrkMax {};
  std::set<uint64_t> SmcPages;

  // helpers, Lock must be held
  void SplitAt(uint64_t Addr);
  void EraseRange(uint64_t Start, uint64_t End);
  void InsertRange(uint64_t Start, uint64_t End, int Prot, const std::string& Name);
  bool RangeFree(uint64_t Start, uint64_t End);
  bool RangeMapped(uint64_t Start, uint64_t End);
  uint64_t FindFreeTopDown(uint64_t Len);
  void ReleaseToReservation(uint64_t Start, uint64_t End);
};
