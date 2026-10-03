// SPDX-License-Identifier: MIT
// x86-64 Linux ABI definitions used by the fexdarwin host: errno values, flag values, struct layouts and the
// conversions from Darwin. Everything here is the *guest* view (Linux x86-64), never the Darwin one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

namespace Linux {

// ---- errno (Linux values) ----
enum : int {
  L_EPERM = 1, L_ENOENT = 2, L_ESRCH = 3, L_EINTR = 4, L_EIO = 5, L_ENXIO = 6, L_E2BIG = 7, L_ENOEXEC = 8, L_EBADF = 9,
  L_ECHILD = 10, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14, L_EBUSY = 16, L_EEXIST = 17, L_EXDEV = 18,
  L_ENODEV = 19, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_ENFILE = 23, L_EMFILE = 24, L_ENOTTY = 25,
  L_ETXTBSY = 26, L_EFBIG = 27, L_ENOSPC = 28, L_ESPIPE = 29, L_EROFS = 30, L_EMLINK = 31, L_EPIPE = 32, L_EDOM = 33,
  L_ERANGE = 34, L_EDEADLK = 35, L_ENAMETOOLONG = 36, L_ENOLCK = 37, L_ENOSYS = 38, L_ENOTEMPTY = 39, L_ELOOP = 40,
  L_EOVERFLOW = 75, L_EOPNOTSUPP = 95, L_ETIMEDOUT = 110, L_EINPROGRESS = 115, L_ENOTSOCK = 88, L_EPROTO = 71,
};

// Translate a Darwin errno into the Linux value the guest expects.
int FromDarwinErrno(int DarwinErrno);

// ---- mmap / mprotect ----
constexpr int PROT_NONE_ = 0, PROT_READ_ = 1, PROT_WRITE_ = 2, PROT_EXEC_ = 4;
constexpr int MAP_SHARED_ = 0x01, MAP_PRIVATE_ = 0x02, MAP_TYPE_ = 0x0f, MAP_FIXED_ = 0x10, MAP_ANONYMOUS_ = 0x20;
constexpr int MAP_GROWSDOWN_ = 0x100, MAP_NORESERVE_ = 0x4000, MAP_POPULATE_ = 0x8000, MAP_STACK_ = 0x20000;
constexpr int MAP_FIXED_NOREPLACE_ = 0x100000;

// ---- open ----
constexpr int O_ACCMODE_ = 3, O_RDONLY_ = 0, O_WRONLY_ = 1, O_RDWR_ = 2, O_CREAT_ = 0x40, O_EXCL_ = 0x80, O_NOCTTY_ = 0x100,
              O_TRUNC_ = 0x200, O_APPEND_ = 0x400, O_NONBLOCK_ = 0x800, O_DIRECTORY_ = 0x10000, O_NOFOLLOW_ = 0x20000,
              O_CLOEXEC_ = 0x80000;
constexpr int AT_FDCWD_ = -100, AT_SYMLINK_NOFOLLOW_ = 0x100, AT_EMPTY_PATH_ = 0x1000;

// ---- clocks ----
constexpr int CLOCK_REALTIME_ = 0, CLOCK_MONOTONIC_ = 1, CLOCK_PROCESS_CPUTIME_ID_ = 2, CLOCK_THREAD_CPUTIME_ID_ = 3,
              CLOCK_MONOTONIC_RAW_ = 4, CLOCK_REALTIME_COARSE_ = 5, CLOCK_MONOTONIC_COARSE_ = 6, CLOCK_BOOTTIME_ = 7;

// ---- arch_prctl ----
constexpr int ARCH_SET_GS = 0x1001, ARCH_SET_FS = 0x1002, ARCH_GET_FS = 0x1003, ARCH_GET_GS = 0x1004;

// ---- signals ----
constexpr int SIG_BLOCK_ = 0, SIG_UNBLOCK_ = 1, SIG_SETMASK_ = 2;
constexpr int L_SIGHUP = 1, L_SIGINT = 2, L_SIGQUIT = 3, L_SIGILL = 4, L_SIGTRAP = 5, L_SIGABRT = 6, L_SIGBUS = 7, L_SIGFPE = 8,
              L_SIGKILL = 9, L_SIGUSR1 = 10, L_SIGSEGV = 11, L_SIGUSR2 = 12, L_SIGPIPE = 13, L_SIGALRM = 14, L_SIGTERM = 15,
              L_SIGCHLD = 17, L_SIGCONT = 18, L_SIGSTOP = 19;
constexpr uint64_t SA_SIGINFO_ = 4, SA_ONSTACK_ = 0x08000000, SA_RESTART_ = 0x10000000, SA_NODEFER_ = 0x40000000,
                   SA_RESETHAND_ = 0x80000000, SA_RESTORER_ = 0x04000000;
constexpr uint64_t SIG_DFL_ = 0, SIG_IGN_ = 1;
constexpr int SS_ONSTACK_ = 1, SS_DISABLE_ = 2;

// Kernel (not libc) layouts.
struct KernelSigaction {
  uint64_t Handler;
  uint64_t Flags;
  uint64_t Restorer;
  uint64_t Mask;
};
static_assert(sizeof(KernelSigaction) == 32);

struct StackT {
  uint64_t Sp;
  int32_t Flags;
  int32_t Pad;
  uint64_t Size;
};
static_assert(sizeof(StackT) == 24);

// ---- clone flags ----
constexpr uint64_t CLONE_VM_ = 0x100, CLONE_FS_ = 0x200, CLONE_FILES_ = 0x400, CLONE_SIGHAND_ = 0x800, CLONE_THREAD_ = 0x10000,
                   CLONE_SYSVSEM_ = 0x40000, CLONE_SETTLS_ = 0x80000, CLONE_PARENT_SETTID_ = 0x100000,
                   CLONE_CHILD_CLEARTID_ = 0x200000, CLONE_CHILD_SETTID_ = 0x01000000;

// ---- futex ----
constexpr int FUTEX_WAIT_ = 0, FUTEX_WAKE_ = 1, FUTEX_WAIT_BITSET_ = 9, FUTEX_WAKE_BITSET_ = 10, FUTEX_PRIVATE_FLAG_ = 128,
              FUTEX_CLOCK_REALTIME_ = 256, FUTEX_CMD_MASK_ = ~(128 | 256);

// ---- auxv ----
constexpr uint64_t AT_NULL_ = 0, AT_PHDR_ = 3, AT_PHENT_ = 4, AT_PHNUM_ = 5, AT_PAGESZ_ = 6, AT_BASE_ = 7, AT_FLAGS_ = 8,
                   AT_ENTRY_ = 9, AT_UID_ = 11, AT_EUID_ = 12, AT_GID_ = 13, AT_EGID_ = 14, AT_PLATFORM_ = 15, AT_HWCAP_ = 16,
                   AT_CLKTCK_ = 17, AT_SECURE_ = 23, AT_RANDOM_ = 25, AT_HWCAP2_ = 26, AT_EXECFN_ = 31;

// ---- struct stat as defined for x86-64 (144 bytes) ----
struct Stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint64_t st_nlink;
  uint32_t st_mode;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t pad0;
  uint64_t st_rdev;
  int64_t st_size;
  int64_t st_blksize;
  int64_t st_blocks;
  uint64_t st_atime_sec, st_atime_nsec;
  uint64_t st_mtime_sec, st_mtime_nsec;
  uint64_t st_ctime_sec, st_ctime_nsec;
  int64_t unused[3];
};
static_assert(sizeof(Stat) == 144);

struct Timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};
struct Timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};
struct Utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
struct Iovec {
  uint64_t Base;
  uint64_t Len;
};
static_assert(sizeof(Utsname) == 390);

void ConvertStat(const struct stat& In, Stat* Out);
int ConvertOpenFlags(int LinuxFlags);       // to Darwin open() flags
int ConvertClock(int LinuxClock);           // to Darwin clockid_t, -1 if unsupported
int ConvertMmapFlagsForFile(int LinuxFlags); // to Darwin mmap() flags
} // namespace Linux
