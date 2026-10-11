// SPDX-License-Identifier: MIT
// Report of a fatal host fault, written to stderr just before the process dies. It says where the host was (image,
// offset, symbol, VM region, frame-pointer backtrace) and where the guest was (its module and offset at RIP, the system
// call in progress, return addresses found on its stack), enough to name the faulting code with atos and objdump.
//
// Only the fatal paths of the signal handler call Host and Guest, and the thread dump calls HostThreads: nothing runs or
// is allocated before a fatal fault. It never waits
// for a lock the faulting thread may hold: guest memory is read with mach_vm_read_overwrite and the VMA table with a
// try-lock. The dladdr fallback for addresses outside fexdarwin-run is the one exception (dyld's lock), printed last.
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/ucontext.h>

struct GuestThread;

namespace CrashReport {
// Name of the code region of a host address that is not in a loaded image ("JIT code", "FEX dispatcher"), or null.
using CodeRegionFn = const char* (*)(uint64_t HostAddr);

// The host side: fault address, pc, lr, their images, symbols and VM regions, the load address of fexdarwin-run, the
// host registers and the frame-pointer chain of the interrupted context.
void Host(ucontext_t* Uc, uint64_t FaultAddr, CodeRegionFn Region);

// The guest side of the thread T: its system call if it is in one, the module and offset at its RIP, the code bytes
// around RIP and the return addresses found on its stack.
void Guest(GuestThread* T);

// Guest tid of a host thread (pthread_threadid_np), to label the threads of HostThreads.
struct ThreadLabel {
  uint64_t HostId;
  int32_t Tid;
};

// Thread dump: name, state and frame-pointer backtrace of every host thread of this process, written to Fd. Each
// thread is suspended only while its registers and frame chain are copied, and every one runs again before any symbol
// is looked up (dladdr takes dyld's lock), so a process whose threads hold engine locks forever can still be read.
void HostThreads(int Fd, const ThreadLabel* Labels, size_t NLabels);
} // namespace CrashReport
