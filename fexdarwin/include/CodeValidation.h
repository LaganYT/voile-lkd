// SPDX-License-Identifier: MIT
// Code that keeps rewriting the host page it runs from. With write tracking (SMCChecks=mtrack, LKD mm/watch.c), every
// store to such a page costs a fault, the invalidation of the page's translations and a new translation once the code
// runs again. ELDEN RING's EAC launcher module (one RWX segment, GCC 7.5) does this about 33,000 times per second in a
// single 16 KiB page, which made its module mapping take about 4 minutes.
//
// A host page that takes ValidateAfter write faults within one Window leaves write tracking for the rest of the
// process image: LKD no longer write-protects it, and FEXCore checks the bytes of each instruction translated from it
// before running it (SyscallHandler::NeedsCodeValidation, the per-instruction check of SMCChecks=full). Other pages
// stay on write tracking. exec starts over.
//
// Order that keeps it correct: the fault handler marks the page, then invalidates it. A translation either sees the
// mark (and checks its bytes) or started before the invalidation, which waits for it and drops it. The state is
// lock-free: the fault side runs in a signal handler.
#pragma once

#include <cstdint>

namespace CodeValidation {
// A store fault on a watched host page (16 KiB aligned). True when this fault moved the page to validation: the
// caller then invalidates the page.
bool NoteWriteFault(uint64_t HostPage);
// Whether a host page of [Start, Start + Len) has left write tracking.
bool Validated(uint64_t Start, uint64_t Len);
// exec: the new image starts with every page on write tracking.
void Reset();
} // namespace CodeValidation
