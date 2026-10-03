// SPDX-License-Identifier: MIT
// Shared state of the fexdarwin host: one FEXCore context, the guest address space, the threads.
#pragma once

#include "GuestMemory.h"
#include "LinuxAbi.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <vector>

class DarwinSyscallHandler;
class DarwinSignalDelegator;

struct GuestThread {
  FEXCore::Core::InternalThreadState* State {};
  pthread_t PThread {};
  int32_t Tid {};
  bool IsMain {};
  uint64_t ClearTidAddr {};  // set_tid_address / CLONE_CHILD_CLEARTID
  uint64_t SigMask {};       // blocked guest signals (bit n-1 is signal n)
  Linux::StackT AltStack {}; // guest sigaltstack
  FEXCore::Core::CPUState::gdt_segment Gdt[32] {};
  void* HostAltStack {};
  size_t HostAltStackSize {};
  void* CallRetAlloc {};
  std::atomic<bool> Exited {false};
  int ExitCode {};
  uint64_t StartRip {};
  uint64_t StartRsp {};
  uint64_t StartRax {};         // value of rax at thread start (0 for the child of a clone)
  uint64_t StartFsBase {};
  bool InSignalFrame {};
  bool SyscallSetsRip {};      // set by a system call that loads the guest RIP itself (rt_sigreturn)
  // LKD mode (FEXDARWIN_LKD=1)
  struct lkd_task* Task {};    // the LKD task of this guest thread
  uint64_t OrigRax {};         // syscall number of the current or last syscall (ptrace orig_rax)
  uint64_t SysArgs[6] {};      // its arguments (thread dump)
  std::atomic<bool> Stopped {false};       // parked in a ptrace stop: registers are in memory
  std::atomic<bool> KickRequested {false};
  std::atomic<bool> InLkd {false};         // inside lkd_syscall/lkd_signal_next: LKD checks pending signals on its way out
  uint64_t FaultErr {}, FaultTrapNo {}, FaultAddr {}; // sigcontext err/trapno/cr2 of the pending synchronous fault
  std::atomic<uint64_t> SysCount {0}, SysNs {0};      // profile (FEXDARWIN_PROFILE): system calls of this thread
};

// The guest thread attached to the calling host thread (null on host-only threads).
extern thread_local GuestThread* CurrentGuestThread;

struct HostOptions {
  bool Trace = false;           // FEXDARWIN_TRACE=1 or --trace
  bool TraceSignals = false;
  bool ShowFeatures = false;
  bool NoSmc = false;           // --smc=none
  std::string SmcMode = "mtrack"; // none | mtrack | full
  std::vector<std::string> ExtraConfig; // --config NAME=VALUE passed to FEXCore::Config::Set
};

struct Host {
  HostOptions Opt;
  GuestMemory Mem;
  FEXCore::HostFeatures Features {};
  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  std::unique_ptr<DarwinSyscallHandler> Syscalls;
  std::unique_ptr<DarwinSignalDelegator> Signals;

  std::mutex ThreadsLock;
  std::vector<GuestThread*> Threads;
  std::atomic<int32_t> NextTid {0};
  int32_t Pid {};

  std::mutex SigLock;
  Linux::KernelSigaction SigActions[65] {};

  uint64_t StackTop {};
  uint64_t StackSize {};
};

extern Host* G;

// ---- thread management (GuestThread.cpp) ----
// Creates the FEXCore thread state for a new guest thread. The caller fills StartRip/StartRsp/... then calls RunGuestThread
// on the host thread that will execute it.
GuestThread* CreateGuestThread(GuestThread* Parent);
// Executes the guest thread on the calling host thread until the guest exits it. Never returns for the main thread.
void RunGuestThread(GuestThread* T);
void ForEachThread(void (*Fn)(GuestThread*, void*), void* Arg);
// Terminate the whole process (exit_group semantics).
[[noreturn]] void ExitProcess(int Code);

// rt_sigreturn: restores the guest context from the signal frame the guest stack pointer designates. Returns the restored
// RAX. Sets T->SyscallSetsRip because the guest RIP is loaded from the frame.
int64_t GuestRtSigreturn(GuestThread* T);
// Drop the translations of SMC-protected pages in [P, P + L) before the host writes there for the guest.
void PrepareGuestWrite(GuestThread* T, uint64_t P, uint64_t L);

// ---- tracing ----
void TraceLog(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
