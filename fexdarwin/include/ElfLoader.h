// SPDX-License-Identifier: MIT
// Minimal ELF64 x86-64 loader for freestanding static executables (ET_EXEC and static ET_DYN).
#pragma once

#include "GuestMemory.h"

#include <cstdint>
#include <string>
#include <vector>

struct LoadedElf {
  uint64_t Entry {};
  uint64_t LoadBias {};
  uint64_t PhdrAddr {}; // guest address of the program headers (AT_PHDR)
  uint16_t PhEnt {};
  uint16_t PhNum {};
  uint64_t LowAddr {};  // lowest mapped guest address
  uint64_t HighAddr {}; // end of the highest mapped segment (page rounded), initial program break
  bool IsDyn {};
  std::string Path;
};

struct ElfLoadOptions {
  // Where an ET_DYN image is placed (16 KiB aligned). ET_EXEC images are mapped at their link address.
  uint64_t DynBase {};
  bool Verbose {};
};

// Returns true on success. On failure Err describes the precise reason.
bool LoadElf(const std::string& Path, GuestMemory& Mem, const ElfLoadOptions& Opt, LoadedElf* Out, std::string* Err);
