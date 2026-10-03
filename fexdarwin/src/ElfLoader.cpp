// SPDX-License-Identifier: MIT
#include "ElfLoader.h"
#include <FEXCore/Utils/GuestBase.h>
#include "LinuxAbi.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "ElfDefs.h"
#include <algorithm>
#include <cstdarg>
#include <fcntl.h>
#include <map>
#include <unistd.h>

namespace {
std::string Fmt(const char* F, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* F, ...) {
  char Buf[512];
  va_list Ap;
  va_start(Ap, F);
  vsnprintf(Buf, sizeof Buf, F, Ap);
  va_end(Ap);
  return Buf;
}

struct FdCloser {
  int Fd;
  ~FdCloser() {
    if (Fd >= 0) ::close(Fd);
  }
};
} // namespace

bool LoadElf(const std::string& Path, GuestMemory& Mem, const ElfLoadOptions& Opt, LoadedElf* Out, std::string* Err) {
  int Fd = ::open(Path.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) {
    *Err = Fmt("cannot open %s: %s", Path.c_str(), strerror(errno));
    return false;
  }
  FdCloser Closer {Fd};

  Elf64_Ehdr Eh;
  if (::pread(Fd, &Eh, sizeof Eh, 0) != static_cast<ssize_t>(sizeof Eh)) {
    *Err = "file too short for an ELF header";
    return false;
  }
  if (memcmp(Eh.e_ident, ELFMAG, SELFMAG) != 0) {
    *Err = "not an ELF file (bad magic)";
    return false;
  }
  if (Eh.e_ident[EI_CLASS] != ELFCLASS64 || Eh.e_ident[EI_DATA] != ELFDATA2LSB) {
    *Err = "not a little-endian ELF64 file";
    return false;
  }
  if (Eh.e_machine != EM_X86_64) {
    *Err = Fmt("not an x86-64 ELF (e_machine=%u)", Eh.e_machine);
    return false;
  }
  if (Eh.e_type != ET_EXEC && Eh.e_type != ET_DYN) {
    *Err = Fmt("unsupported ELF type %u (need ET_EXEC or ET_DYN)", Eh.e_type);
    return false;
  }
  if (Eh.e_phentsize != sizeof(Elf64_Phdr) || Eh.e_phnum == 0 || Eh.e_phnum > 64) {
    *Err = Fmt("unsupported program header table (entsize=%u num=%u)", Eh.e_phentsize, Eh.e_phnum);
    return false;
  }

  std::vector<Elf64_Phdr> Ph(Eh.e_phnum);
  if (::pread(Fd, Ph.data(), Ph.size() * sizeof(Elf64_Phdr), Eh.e_phoff) != static_cast<ssize_t>(Ph.size() * sizeof(Elf64_Phdr))) {
    *Err = "cannot read program headers";
    return false;
  }

  uint64_t Lo = ~0ULL, Hi = 0;
  uint64_t DynamicVaddr = 0, DynamicSize = 0;
  for (auto& P : Ph) {
    if (P.p_type == PT_INTERP) {
      *Err = "the executable has a PT_INTERP (dynamic executable); only static executables are supported";
      return false;
    }
    if (P.p_type == PT_DYNAMIC) {
      DynamicVaddr = P.p_vaddr;
      DynamicSize = P.p_memsz;
    }
    if (P.p_type != PT_LOAD) continue;
    if (P.p_filesz > P.p_memsz) {
      *Err = "PT_LOAD with p_filesz > p_memsz";
      return false;
    }
    if (P.p_align > 1 && ((P.p_vaddr ^ P.p_offset) & (P.p_align - 1)) != 0) {
      *Err = Fmt("PT_LOAD vaddr %#llx and offset %#llx are not congruent modulo the alignment", (unsigned long long)P.p_vaddr, (unsigned long long)P.p_offset);
      return false;
    }
    Lo = std::min<uint64_t>(Lo, P.p_vaddr);
    Hi = std::max<uint64_t>(Hi, P.p_vaddr + P.p_memsz);
  }
  if (Lo == ~0ULL) {
    *Err = "no PT_LOAD segment";
    return false;
  }

  const bool IsDyn = Eh.e_type == ET_DYN;
  uint64_t Bias = 0;
  if (IsDyn) {
    if (!Opt.DynBase || (Opt.DynBase & GuestMemory::PageMask)) {
      *Err = "ET_DYN image but no valid DynBase was provided";
      return false;
    }
    Bias = Opt.DynBase - GuestMemory::RoundDown(Lo);
  }
  const uint64_t MapLo = GuestMemory::RoundDown(Lo + Bias);
  const uint64_t MapHi = GuestMemory::RoundUp(Hi + Bias);
  if (!Mem.InWindow(MapLo, MapHi - MapLo)) {
    *Err = Fmt("image range %#llx-%#llx is outside the guest window %#llx-%#llx. Link the program inside the window "
               "(see README) or use a static-pie.",
               (unsigned long long)MapLo, (unsigned long long)MapHi, (unsigned long long)Mem.WindowBase(),
               (unsigned long long)Mem.WindowEnd());
    return false;
  }

  // Per 16 KiB page protection = union of the segments that touch the page.
  std::map<uint64_t, int> PageProt;
  for (auto& P : Ph) {
    if (P.p_type != PT_LOAD) continue;
    int Prot = 0;
    if (P.p_flags & PF_R) Prot |= Linux::PROT_READ_;
    if (P.p_flags & PF_W) Prot |= Linux::PROT_WRITE_;
    if (P.p_flags & PF_X) Prot |= Linux::PROT_EXEC_;
    const uint64_t S = GuestMemory::RoundDown(P.p_vaddr + Bias);
    const uint64_t E = GuestMemory::RoundUp(P.p_vaddr + Bias + P.p_memsz);
    for (uint64_t Pg = S; Pg < E; Pg += GuestMemory::PageSize) PageProt[Pg] |= Prot;
  }

  // 1. map everything read/write so the contents can be written
  {
    int Rc = Mem.MapFixedAnon(MapLo, MapHi - MapLo, Linux::PROT_READ_ | Linux::PROT_WRITE_, Path.c_str(), nullptr);
    if (Rc != 0) {
      *Err = Fmt("cannot map the image at %#llx: errno %d", (unsigned long long)MapLo, -Rc);
      return false;
    }
  }
  // 2. copy file contents (bss is already zero)
  for (auto& P : Ph) {
    if (P.p_type != PT_LOAD || P.p_filesz == 0) continue;
    uint8_t* Dst = static_cast<uint8_t*>(FEXCore::G2H(P.p_vaddr + Bias));
    size_t Done = 0;
    while (Done < P.p_filesz) {
      ssize_t R = ::pread(Fd, Dst + Done, P.p_filesz - Done, P.p_offset + Done);
      if (R <= 0) {
        *Err = Fmt("short read while loading a segment (%s)", R < 0 ? strerror(errno) : "unexpected end of file");
        return false;
      }
      Done += R;
    }
  }
  // 3. relocate static-pie images (R_X86_64_RELATIVE only)
  // Linux never relocates: real static-pie programs and ld.so relocate themselves. FEXDARWIN_NO_RELOC=1 keeps that
  // behavior; the default still relocates the freestanding witnesses, which have no self-relocation code.
  const char* NoReloc = getenv("FEXDARWIN_NO_RELOC");
  if (IsDyn && DynamicSize && !(NoReloc && *NoReloc == '1')) {
    auto* Dyn = static_cast<Elf64_Dyn*>(FEXCore::G2H(DynamicVaddr + Bias));
    uint64_t Rela = 0, RelaSz = 0, RelaEnt = sizeof(Elf64_Rela);
    for (; Dyn->d_tag != DT_NULL; ++Dyn) {
      if (Dyn->d_tag == DT_RELA) Rela = Dyn->d_un.d_ptr;
      else if (Dyn->d_tag == DT_RELASZ) RelaSz = Dyn->d_un.d_val;
      else if (Dyn->d_tag == DT_RELAENT) RelaEnt = Dyn->d_un.d_val;
      else if (Dyn->d_tag == DT_REL || Dyn->d_tag == DT_JMPREL || Dyn->d_tag == DT_NEEDED) {
        *Err = Fmt("unsupported dynamic tag %lld: not a self-contained static-pie", (long long)Dyn->d_tag);
        return false;
      }
    }
    if (Rela && RelaSz) {
      auto* R = static_cast<Elf64_Rela*>(FEXCore::G2H(Rela + Bias));
      for (uint64_t I = 0; I < RelaSz / RelaEnt; ++I) {
        if (ELF64_R_TYPE(R[I].r_info) != R_X86_64_RELATIVE) {
          *Err = Fmt("unsupported relocation type %u at index %llu (only R_X86_64_RELATIVE is handled)",
                     (unsigned)ELF64_R_TYPE(R[I].r_info), (unsigned long long)I);
          return false;
        }
        *static_cast<uint64_t*>(FEXCore::G2H(R[I].r_offset + Bias)) = Bias + R[I].r_addend;
      }
    }
  }
  // 4. final protections
  for (auto& [Pg, Prot] : PageProt) {
    int Rc = Mem.SetProt(Pg, GuestMemory::PageSize, Prot);
    if (Rc != 0) {
      *Err = Fmt("mprotect failed at %#llx: errno %d", (unsigned long long)Pg, -Rc);
      return false;
    }
  }

  Out->Entry = Eh.e_entry + Bias;
  Out->LoadBias = Bias;
  Out->PhEnt = Eh.e_phentsize;
  Out->PhNum = Eh.e_phnum;
  Out->LowAddr = MapLo;
  Out->HighAddr = MapHi;
  Out->IsDyn = IsDyn;
  Out->Path = Path;
  // AT_PHDR: find the PT_LOAD that contains the file offset of the program header table.
  Out->PhdrAddr = 0;
  for (auto& P : Ph) {
    if (P.p_type == PT_LOAD && Eh.e_phoff >= P.p_offset && Eh.e_phoff + Eh.e_phnum * sizeof(Elf64_Phdr) <= P.p_offset + P.p_filesz) {
      Out->PhdrAddr = P.p_vaddr + (Eh.e_phoff - P.p_offset) + Bias;
      break;
    }
  }
  if (!Out->PhdrAddr) {
    *Err = "the program headers are not inside any PT_LOAD segment (AT_PHDR cannot be provided)";
    return false;
  }
  if (Opt.Verbose) {
    fprintf(stderr, "[fexdarwin] ELF %s: %s entry=%#llx bias=%#llx range=%#llx-%#llx\n", Path.c_str(), IsDyn ? "ET_DYN" : "ET_EXEC",
            (unsigned long long)Out->Entry, (unsigned long long)Bias, (unsigned long long)MapLo, (unsigned long long)MapHi);
  }
  return true;
}
