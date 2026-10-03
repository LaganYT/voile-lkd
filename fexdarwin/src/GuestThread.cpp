// SPDX-License-Identifier: MIT
#include "DarwinSignalDelegator.h"
#include "DarwinSyscallHandler.h"
#include "HitchLog.h"
#include "Host.h"

#include <FEXCore/Core/X86Enums.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

Host* G = nullptr;

GuestThread* CreateGuestThread(GuestThread* Parent) {
  using FEXCore::Core::CPUState;
  auto* T = new GuestThread();
  T->Tid = Parent ? ++G->NextTid + G->Pid : G->Pid;
  T->IsMain = Parent == nullptr;

  // A child copies the CPU state of its parent (clone semantics), the first thread starts from the reset state.
  const CPUState* Init = Parent ? &Parent->State->CurrentFrame->State : nullptr;
  T->State = G->CTX->CreateThread(Init);
  T->State->FrontendPtr = T;
  HitchLog::Attach(T->State);
  auto* Frame = T->State->CurrentFrame;

  // Call-ret shadow stack with a guard page on each side (the guard size is one *host* page: mprotect granularity).
  constexpr size_t Guard = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
  constexpr size_t Total = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE + 2 * Guard;
  void* Alloc = ::mmap(nullptr, Total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Alloc == MAP_FAILED) {
    fprintf(stderr, "[fexdarwin] cannot allocate the call-ret stack: %s\n", strerror(errno));
    _exit(70);
  }
  T->CallRetAlloc = Alloc;
  uint8_t* Base = static_cast<uint8_t*>(Alloc) + Guard;
  ::mprotect(Base, FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, PROT_READ | PROT_WRITE);
  T->State->CallRetStackBase = Base;
  Frame->State.callret_sp = reinterpret_cast<uint64_t>(Base) + FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE / 4;

  // GDT: one flat 64-bit code segment, as the Linux frontend sets up.
  Frame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_GDT] = &T->Gdt[0];
  Frame->State.segment_arrays[CPUState::SEGMENT_ARRAY_INDEX_LDT] = &T->Gdt[0];
  if (Parent) {
    memcpy(T->Gdt, Parent->Gdt, sizeof(T->Gdt));
    T->SigMask = Parent->SigMask;
  } else {
    // Linux x86-64 user selectors: CS = GDT entry 6 with RPL 3 (0x33), SS = entry 5 with RPL 3 (0x2b)
    // (arch/x86/include/asm/segment.h). FEX selects the GDT entry with Selector >> 3, so the RPL bits are safe.
    Frame->State.cs_idx = (CPUState::DEFAULT_USER_CS << 3) | 3;
    Frame->State.ss_idx = (5 << 3) | 3;
    auto* Seg = CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
    CPUState::SetGDTBase(Seg, 0);
    CPUState::SetGDTLimit(Seg, 0xF'FFFFU);
    Frame->State.cs_cached = CPUState::CalculateGDTBase(*Seg);
    Seg->L = 1; // long mode
    Seg->D = 0;
  }

  {
    std::lock_guard<std::mutex> lk(G->ThreadsLock);
    G->Threads.push_back(T);
  }
  return T;
}

void ForEachThread(void (*Fn)(GuestThread*, void*), void* Arg) {
  std::lock_guard<std::mutex> lk(G->ThreadsLock);
  for (auto* T : G->Threads) Fn(T, Arg);
}

void RunGuestThread(GuestThread* T) {
  namespace X = FEXCore::X86State;
  G->Signals->RegisterThread(T);
  auto& St = T->State->CurrentFrame->State;
  St.rip = T->StartRip;
  St.gregs[X::REG_RSP] = T->StartRsp;
  St.gregs[X::REG_RAX] = T->StartRax;
  if (T->StartFsBase) St.fs_cached = T->StartFsBase;
  G->CTX->ExecuteThread(T->State);
  // ExecuteThread returns when the thread is told to stop. The process-wide exit paths never get here.
}

[[noreturn]] void ExitProcess(int Code) {
  // Diagnostics: FEXDARWIN_EXIT_PAUSE=<seconds> keeps the host process alive before it ends, so its address space can be
  // inspected with vmmap. The supervisor watchdog still ends it if the run is torn down.
  if (const char* P = getenv("FEXDARWIN_EXIT_PAUSE"); P && *P) sleep(static_cast<unsigned>(atoi(P)));
  _exit(Code);
}
