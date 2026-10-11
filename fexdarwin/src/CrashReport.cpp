// SPDX-License-Identifier: MIT
#include "CrashReport.h"
extern "C" {
#include <lkd/syscall_nr.h> // C header without its own extern "C"
}
#include "DarwinContext.h"
#include "Host.h"
#include "LkdGlue.h"

#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/GuestBase.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

extern "C" const struct mach_header_64 _mh_execute_header;
extern "C" int fexdarwin_lkd_vma_describe_nowait(lkd_task* t, uint64_t gva, uint64_t* start, uint64_t* end, uint32_t* prot,
                                                 uint64_t* off, char* name, unsigned long name_len);
extern "C" void fexdarwin_lkd_task_comm(lkd_task* t, char* out, unsigned long len);

namespace {
int OutFd = 2; // stderr, or the dump file while HostThreads writes

void Put(const char* Fmt, ...) __attribute__((format(printf, 1, 2)));
void Put(const char* Fmt, ...) {
  char Buf[768];
  va_list Ap;
  va_start(Ap, Fmt);
  const int N = vsnprintf(Buf, sizeof Buf, Fmt, Ap);
  va_end(Ap);
  if (N <= 0) return;
  size_t Len = std::min<size_t>(N, sizeof Buf - 1);
  const char* P = Buf;
  while (Len) {
    const ssize_t R = ::write(OutFd, P, Len);
    if (R <= 0) return;
    P += R;
    Len -= R;
  }
}

using ull = unsigned long long;

// Reads host memory without faulting: false when any byte is unmapped or unreadable.
bool ReadHost(uint64_t Addr, void* Out, size_t Len) {
  mach_vm_size_t Got = 0;
  return mach_vm_read_overwrite(mach_task_self(), Addr, Len, reinterpret_cast<mach_vm_address_t>(Out), &Got) == KERN_SUCCESS &&
         Got == Len;
}

// fexdarwin-run as loaded, found from its own Mach-O header without asking dyld. The symbol table stays in releases
// (only debug information is stripped), so function names come from it.
struct MainImage {
  uint64_t Header = 0;        // load address (atos -l)
  uint64_t Start = 0, End = 0; // its segments in memory, __PAGEZERO excluded
  uint64_t Slide = 0;
  const nlist_64* Syms = nullptr;
  uint32_t NSyms = 0;
  const char* Strs = nullptr;
  uint32_t StrSize = 0;
};

MainImage LoadMainImage() {
  MainImage M;
  const auto* H = &_mh_execute_header;
  if (H->magic != MH_MAGIC_64) return M;
  const auto* P = reinterpret_cast<const uint8_t*>(H + 1);
  const segment_command_64* Linkedit = nullptr;
  const symtab_command* Symtab = nullptr;
  uint64_t TextVm = 0, Lo = ~0ull, Hi = 0;
  bool HaveText = false;
  for (uint32_t I = 0; I < H->ncmds; I++) {
    const auto* Lc = reinterpret_cast<const load_command*>(P);
    if (Lc->cmd == LC_SEGMENT_64) {
      const auto* S = reinterpret_cast<const segment_command_64*>(Lc);
      if (strncmp(S->segname, "__TEXT", sizeof S->segname) == 0) {
        TextVm = S->vmaddr;
        HaveText = true;
      } else if (strncmp(S->segname, "__LINKEDIT", sizeof S->segname) == 0) {
        Linkedit = S;
      }
      if (S->maxprot != 0) { // not __PAGEZERO
        Lo = std::min<uint64_t>(Lo, S->vmaddr);
        Hi = std::max<uint64_t>(Hi, S->vmaddr + S->vmsize);
      }
    } else if (Lc->cmd == LC_SYMTAB) {
      Symtab = reinterpret_cast<const symtab_command*>(Lc);
    }
    P += Lc->cmdsize;
  }
  if (!HaveText) return M;
  M.Header = reinterpret_cast<uint64_t>(H);
  M.Slide = M.Header - TextVm;
  M.Start = Lo + M.Slide;
  M.End = Hi + M.Slide;
  if (Linkedit && Symtab) {
    const uint64_t Base = Linkedit->vmaddr + M.Slide - Linkedit->fileoff;
    M.Syms = reinterpret_cast<const nlist_64*>(Base + Symtab->symoff);
    M.NSyms = Symtab->nsyms;
    M.Strs = reinterpret_cast<const char*>(Base + Symtab->stroff);
    M.StrSize = Symtab->strsize;
  }
  return M;
}

// Nearest defined symbol at or below Addr in fexdarwin-run (mangled C++ names: c++filt reads them).
const char* MainSymbol(const MainImage& M, uint64_t Addr, uint64_t* Off) {
  const uint64_t Unslid = Addr - M.Slide;
  const nlist_64* Best = nullptr;
  for (uint32_t I = 0; I < M.NSyms; I++) {
    const nlist_64& N = M.Syms[I];
    if ((N.n_type & N_STAB) || (N.n_type & N_TYPE) != N_SECT) continue;
    if (N.n_value <= Unslid && (!Best || N.n_value > Best->n_value)) Best = &N;
  }
  if (!Best || Best->n_un.n_strx >= M.StrSize) return nullptr;
  *Off = Unslid - Best->n_value;
  const char* Name = M.Strs + Best->n_un.n_strx;
  return Name[0] == '_' ? Name + 1 : Name;
}

// " fexdarwin-run+0x... (symbol+0x...)", " [JIT code]", " libsystem_x.dylib+0x... (symbol+0x...)" or " [no image]". A
// return address is looked up one byte back: a call at the end of a function returns past it.
void PutWhere(const MainImage& M, uint64_t Addr, CrashReport::CodeRegionFn Region, bool AllowDyld, bool IsReturn) {
  const uint64_t Look = IsReturn && Addr ? Addr - 1 : Addr;
  if (M.Header && Look >= M.Start && Look < M.End) {
    uint64_t Off = 0;
    const char* S = MainSymbol(M, Look, &Off);
    Put(" fexdarwin-run+%#llx", (ull)(Addr - M.Header));
    if (S) Put(" (%s+%#llx)", S, (ull)(Off + (Addr - Look)));
    return;
  }
  if (const char* R = Region ? Region(Addr) : nullptr) {
    Put(" [%s]", R);
    return;
  }
  Dl_info I {};
  if (AllowDyld && dladdr(reinterpret_cast<const void*>(Look), &I) && I.dli_fname && I.dli_fbase) {
    const char* Base = strrchr(I.dli_fname, '/');
    Put(" %s+%#llx", Base ? Base + 1 : I.dli_fname, (ull)(Addr - reinterpret_cast<uint64_t>(I.dli_fbase)));
    if (I.dli_sname && I.dli_saddr) Put(" (%s+%#llx)", I.dli_sname, (ull)(Addr - reinterpret_cast<uint64_t>(I.dli_saddr)));
    return;
  }
  Put(" [no image]");
}

void PutRegion(const char* Label, uint64_t Addr) {
  mach_vm_address_t A = Addr;
  mach_vm_size_t S = 0;
  natural_t Depth = 0;
  vm_region_submap_info_data_64_t Info {};
  mach_msg_type_number_t C = VM_REGION_SUBMAP_INFO_COUNT_64;
  if (mach_vm_region_recurse(mach_task_self(), &A, &S, &Depth, reinterpret_cast<vm_region_recurse_info_t>(&Info), &C) != KERN_SUCCESS) {
    Put("[fexdarwin]   %s %#llx: no VM region at or above it\n", Label, (ull)Addr);
    return;
  }
  if (A > Addr) {
    Put("[fexdarwin]   %s %#llx: unmapped (next VM region at %#llx)\n", Label, (ull)Addr, (ull)A);
    return;
  }
  const auto P = [](vm_prot_t V, char* Out) {
    Out[0] = (V & VM_PROT_READ) ? 'r' : '-';
    Out[1] = (V & VM_PROT_WRITE) ? 'w' : '-';
    Out[2] = (V & VM_PROT_EXECUTE) ? 'x' : '-';
    Out[3] = 0;
  };
  char Cur[4], Max[4];
  P(Info.protection, Cur);
  P(Info.max_protection, Max);
  Put("[fexdarwin]   %s %#llx: VM region [%#llx, %#llx) %s/%s tag %u%s\n", Label, (ull)Addr, (ull)A, (ull)(A + S), Cur, Max,
      Info.user_tag, Addr < 0x100000000ull && A == 0 ? " (__PAGEZERO: a host access to a raw guest or null-based pointer)" : "");
}

// Describes guest address V in the VMA table of Task, without waiting for its lock.
void PutGuestWhere(lkd_task* Task, uint64_t V) {
  uint64_t S = 0, E = 0, Off = 0;
  uint32_t Prot = 0;
  char Name[256];
  const int R = Task ? fexdarwin_lkd_vma_describe_nowait(Task, V, &S, &E, &Prot, &Off, Name, sizeof Name) : -LX_ENOENT;
  if (R == -LX_EBUSY) {
    Put(" (VMA table busy)");
  } else if (R != 0) {
    Put(" (not mapped in the guest)");
  } else {
    Put(" %s+%#llx (vma %#llx-%#llx %c%c%c)", Name[0] ? Name : "[anon]", (ull)(V - S + Off), (ull)S, (ull)E, (Prot & 1) ? 'r' : '-',
        (Prot & 2) ? 'w' : '-', (Prot & 4) ? 'x' : '-');
  }
}

bool GuestExecutable(lkd_task* Task, uint64_t V) {
  uint64_t S, E, Off;
  uint32_t Prot = 0;
  char Name[2];
  return fexdarwin_lkd_vma_describe_nowait(Task, V, &S, &E, &Prot, &Off, Name, sizeof Name) == 0 && (Prot & 4);
}
} // namespace

void CrashReport::Host(ucontext_t* Uc, uint64_t FaultAddr, CodeRegionFn Region) {
  const MainImage M = LoadMainImage();
  const uint64_t Pc = DarwinCtx::GetPc(Uc);
  const uint64_t Lr = DarwinCtx::GetReg(Uc, 30) & 0x0000'7fff'ffff'ffffull;
  char Name[64] = "";
  pthread_getname_np(pthread_self(), Name, sizeof Name);
  Put("[fexdarwin]   host pid %d, thread \"%s\", fexdarwin-run loaded at %#llx (slide %#llx; atos -o fexdarwin-run -l %#llx ADDR)\n",
      static_cast<int>(getpid()), Name, (ull)M.Header, (ull)M.Slide, (ull)M.Header);
  Put("[fexdarwin]   pc %#llx", (ull)Pc);
  PutWhere(M, Pc, Region, false, false);
  Put("\n[fexdarwin]   lr %#llx", (ull)Lr);
  PutWhere(M, Lr, Region, false, true);
  Put("\n[fexdarwin]   esr %#x far %#llx\n", DarwinCtx::GetEsr(Uc), (ull)DarwinCtx::GetFar(Uc));
  PutRegion("fault address", FaultAddr);
  PutRegion("pc", Pc);
  for (int I = 0; I < 29; I += 4) {
    Put("[fexdarwin]   x%-2d %#18llx", I, (ull)DarwinCtx::GetReg(Uc, I));
    for (int J = I + 1; J < I + 4 && J < 29; J++) Put("  x%-2d %#18llx", J, (ull)DarwinCtx::GetReg(Uc, J));
    Put("\n");
  }
  Put("[fexdarwin]   fp %#llx sp %#llx\n", (ull)DarwinCtx::GetReg(Uc, 29), (ull)DarwinCtx::GetSp(Uc));
  // dladdr (dyld's lock) only from here on, once everything above is out.
  Put("[fexdarwin]   host backtrace (frame pointers):\n[fexdarwin]     #0  %#llx", (ull)Pc);
  PutWhere(M, Pc, Region, true, false);
  Put("\n");
  uint64_t Fp = DarwinCtx::GetReg(Uc, 29);
  for (int I = 1; I < 48; I++) {
    uint64_t Pair[2];
    if (!Fp || (Fp & 7) || !ReadHost(Fp, Pair, sizeof Pair)) break;
    const uint64_t Ret = Pair[1] & 0x0000'7fff'ffff'ffffull; // no pointer authentication bits
    if (!Ret) break;
    Put("[fexdarwin]     #%-2d %#llx", I, (ull)Ret);
    PutWhere(M, Ret, Region, true, true);
    Put("\n");
    if (Pair[0] <= Fp) break;
    Fp = Pair[0];
  }
}

void CrashReport::HostThreads(int Fd, const ThreadLabel* Labels, size_t NLabels) {
  constexpr int MaxFrames = 48;
  constexpr uint64_t NoPac = 0x0000'7fff'ffff'ffffull;
  struct Snap {
    thread_t Port = MACH_PORT_NULL;
    uint64_t Id = 0;
    char Name[64] = "";
    int RunState = 0;
    bool Self = false, Got = false;
    uint64_t Pc = 0, Lr = 0, Sp = 0;
    uint64_t Ret[MaxFrames] {};
    int NRet = 0;
  };
  const int SavedFd = OutFd;
  OutFd = Fd;
  const MainImage M = LoadMainImage();
  thread_act_array_t List = nullptr;
  mach_msg_type_number_t N = 0;
  if (task_threads(mach_task_self(), &List, &N) != KERN_SUCCESS) {
    Put("host threads: task_threads failed\n");
    OutFd = SavedFd;
    return;
  }
  std::vector<Snap> Snaps(N); // allocated before any thread is suspended: one may hold the malloc lock
  const thread_t Me = mach_thread_self();
  for (mach_msg_type_number_t I = 0; I < N; I++) {
    Snap& S = Snaps[I];
    S.Port = List[I];
    thread_identifier_info_data_t Ident {};
    mach_msg_type_number_t C = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(S.Port, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&Ident), &C) == KERN_SUCCESS) S.Id = Ident.thread_id;
    thread_extended_info_data_t Ext {};
    C = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(S.Port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&Ext), &C) == KERN_SUCCESS) {
      memcpy(S.Name, Ext.pth_name, std::min(sizeof S.Name, sizeof Ext.pth_name));
      S.Name[sizeof S.Name - 1] = 0;
      S.RunState = Ext.pth_run_state;
    }
    if (S.Port == Me) {
      S.Self = true;
      continue;
    }
    if (thread_suspend(S.Port) != KERN_SUCCESS) continue;
    arm_thread_state64_t St {};
    C = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(S.Port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&St), &C) == KERN_SUCCESS) {
      S.Got = true;
      S.Pc = St.__pc & NoPac;
      S.Lr = St.__lr & NoPac;
      S.Sp = St.__sp;
      uint64_t Fp = St.__fp;
      while (S.NRet < MaxFrames) {
        uint64_t Pair[2];
        if (!Fp || (Fp & 7) || !ReadHost(Fp, Pair, sizeof Pair)) break;
        const uint64_t Ret = Pair[1] & NoPac;
        if (!Ret) break;
        S.Ret[S.NRet++] = Ret;
        if (Pair[0] <= Fp) break;
        Fp = Pair[0];
      }
    }
    thread_resume(S.Port);
  }
  mach_port_deallocate(mach_task_self(), Me);

  // Every thread runs again: symbols may now be looked up.
  static const char* const States[] = {"?", "running", "stopped", "waiting", "uninterruptible", "halted"};
  Put("host threads: %u (fexdarwin-run loaded at %#llx; atos -o fexdarwin-run -l %#llx ADDR)\n", N, (ull)M.Header, (ull)M.Header);
  for (const Snap& S : Snaps) {
    int32_t Tid = 0;
    for (size_t L = 0; L < NLabels; L++) {
      if (Labels[L].HostId == S.Id) Tid = Labels[L].Tid;
    }
    Put("\n-- host thread %#llx \"%s\" %s", (ull)S.Id, S.Name, States[S.RunState >= 1 && S.RunState <= 5 ? S.RunState : 0]);
    if (Tid) Put(", guest tid %d", Tid);
    Put("\n");
    if (S.Self) {
      Put("   (the thread writing this dump)\n");
    } else if (!S.Got) {
      Put("   (registers not readable)\n");
    } else {
      Put("   pc %#llx", (ull)S.Pc);
      PutWhere(M, S.Pc, nullptr, true, false);
      Put("\n   lr %#llx", (ull)S.Lr);
      PutWhere(M, S.Lr, nullptr, true, true);
      Put("\n   sp %#llx\n", (ull)S.Sp);
      for (int I = 0; I < S.NRet; I++) {
        Put("   #%-2d %#llx", I + 1, (ull)S.Ret[I]);
        PutWhere(M, S.Ret[I], nullptr, true, true);
        Put("\n");
      }
    }
    mach_port_deallocate(mach_task_self(), S.Port);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(List), N * sizeof(thread_t));
  OutFd = SavedFd;
}

void CrashReport::Guest(GuestThread* T) {
  namespace X = FEXCore::X86State;
  auto* Frame = T->State->CurrentFrame;
  const auto& St = Frame->State;
  char Comm[32] = "";
  if (T->Task) fexdarwin_lkd_task_comm(T->Task, Comm, sizeof Comm);
  Put("[fexdarwin]   guest tid %d \"%s\", guest window at %#llx\n", T->Tid, Comm, (ull)FEXCore::GuestBase);
  Put("[fexdarwin]   guest r10=%#llx r11=%#llx r12=%#llx r13=%#llx r14=%#llx r15=%#llx fs_base=%#llx gs_base=%#llx\n",
      (ull)St.gregs[X::REG_R10], (ull)St.gregs[X::REG_R11], (ull)St.gregs[X::REG_R12], (ull)St.gregs[X::REG_R13],
      (ull)St.gregs[X::REG_R14], (ull)St.gregs[X::REG_R15], (ull)St.fs_cached, (ull)St.gs_cached);
  if (T->InLkd.load()) {
    const uint64_t Nr = T->OrigRax;
    const char* N = Nr < 1024 ? lkd_syscall_name(static_cast<unsigned long>(Nr)) : nullptr;
    Put("[fexdarwin]   in system call %llu (%s): %#llx, %#llx, %#llx, %#llx, %#llx, %#llx\n", (ull)Nr, N ? N : "?", (ull)T->SysArgs[0],
        (ull)T->SysArgs[1], (ull)T->SysArgs[2], (ull)T->SysArgs[3], (ull)T->SysArgs[4], (ull)T->SysArgs[5]);
  } else {
    Put("[fexdarwin]   not inside LKD (FEX syscall info %#llx)\n", (ull)Frame->InSyscallInfo);
  }
  Put("[fexdarwin]   guest rip %#llx", (ull)St.rip);
  PutGuestWhere(T->Task, St.rip);
  uint8_t Code[32];
  if (St.rip >= 16 && ReadHost(FEXCore::GuestBase + St.rip - 16, Code, sizeof Code)) {
    Put("\n[fexdarwin]   guest code [rip-16, rip+16):");
    for (int I = 0; I < 32; I++) Put(I == 16 ? " >%02x" : " %02x", Code[I]);
  }
  Put("\n");
  // Stack words that point into executable guest mappings: candidate return addresses, innermost first.
  const uint64_t Rsp = St.gregs[X::REG_RSP] & ~7ull;
  Put("[fexdarwin]   guest stack words pointing into code (rsp %#llx):\n", (ull)Rsp);
  uint64_t Buf[64];
  unsigned Hits = 0;
  for (uint64_t Off = 0; Off < 64 * 1024 && Hits < 32; Off += sizeof Buf) {
    if (!ReadHost(FEXCore::GuestBase + Rsp + Off, Buf, sizeof Buf)) break;
    for (size_t I = 0; I < sizeof Buf / 8 && Hits < 32; I++) {
      const uint64_t V = Buf[I];
      if (V < 0x10000 || V >= (1ull << 47) || !T->Task || !GuestExecutable(T->Task, V)) continue;
      Put("[fexdarwin]     [rsp+%#llx] %#llx", (ull)(Off + I * 8), (ull)V);
      PutGuestWhere(T->Task, V);
      Put("\n");
      Hits++;
    }
  }
}
