// SPDX-License-Identifier: MIT
#include "LinuxAbi.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>

namespace Linux {

int FromDarwinErrno(int E) {
  switch (E) {
  case 0: return 0;
  // 1..34 are identical on both systems except the ones listed below.
  case EAGAIN: return L_EAGAIN;            // Darwin 35
  case EDEADLK: return L_EDEADLK;          // Darwin 11
  case EINPROGRESS: return L_EINPROGRESS;  // Darwin 36
  case ENAMETOOLONG: return L_ENAMETOOLONG; // Darwin 63
  case ENOTEMPTY: return L_ENOTEMPTY;      // Darwin 66
  case ELOOP: return L_ELOOP;              // Darwin 62
  case ENOLCK: return L_ENOLCK;            // Darwin 77
  case ENOSYS: return L_ENOSYS;            // Darwin 78
  case EOVERFLOW: return L_EOVERFLOW;      // Darwin 84
  case ENOTSUP: return L_EOPNOTSUPP;       // Darwin 45 (EOPNOTSUPP is 102)
  case EOPNOTSUPP: return L_EOPNOTSUPP;
  case ETIMEDOUT: return L_ETIMEDOUT;      // Darwin 60
  case ENOTSOCK: return L_ENOTSOCK;        // Darwin 38
  case EPROTO: return L_EPROTO;            // Darwin 100
  default:
    if (E >= 1 && E <= 34) {
      return E;
    }
    return L_EIO; // visible catch-all, not a success
  }
}

void ConvertStat(const struct stat& In, Stat* Out) {
  *Out = {};
  Out->st_dev = static_cast<uint64_t>(In.st_dev);
  Out->st_ino = In.st_ino;
  Out->st_nlink = In.st_nlink;
  Out->st_mode = In.st_mode; // S_IF* values are identical on Linux and Darwin
  Out->st_uid = In.st_uid;
  Out->st_gid = In.st_gid;
  Out->st_rdev = static_cast<uint64_t>(In.st_rdev);
  Out->st_size = In.st_size;
  Out->st_blksize = In.st_blksize;
  Out->st_blocks = In.st_blocks;
  Out->st_atime_sec = In.st_atimespec.tv_sec;
  Out->st_atime_nsec = In.st_atimespec.tv_nsec;
  Out->st_mtime_sec = In.st_mtimespec.tv_sec;
  Out->st_mtime_nsec = In.st_mtimespec.tv_nsec;
  Out->st_ctime_sec = In.st_ctimespec.tv_sec;
  Out->st_ctime_nsec = In.st_ctimespec.tv_nsec;
}

int ConvertOpenFlags(int F) {
  int R = F & O_ACCMODE_; // 0,1,2 identical
  if (F & O_CREAT_) R |= O_CREAT;
  if (F & O_EXCL_) R |= O_EXCL;
  if (F & O_NOCTTY_) R |= O_NOCTTY;
  if (F & O_TRUNC_) R |= O_TRUNC;
  if (F & O_APPEND_) R |= O_APPEND;
  if (F & O_NONBLOCK_) R |= O_NONBLOCK;
  if (F & O_DIRECTORY_) R |= O_DIRECTORY;
  if (F & O_NOFOLLOW_) R |= O_NOFOLLOW;
  if (F & O_CLOEXEC_) R |= O_CLOEXEC;
  return R;
}

int ConvertClock(int C) {
  switch (C) {
  case CLOCK_REALTIME_: return CLOCK_REALTIME;
  case CLOCK_MONOTONIC_: return CLOCK_MONOTONIC;
  case CLOCK_MONOTONIC_RAW_: return CLOCK_MONOTONIC_RAW;
  case CLOCK_PROCESS_CPUTIME_ID_: return CLOCK_PROCESS_CPUTIME_ID;
  case CLOCK_THREAD_CPUTIME_ID_: return CLOCK_THREAD_CPUTIME_ID;
  case CLOCK_REALTIME_COARSE_: return CLOCK_REALTIME;
  case CLOCK_MONOTONIC_COARSE_: return CLOCK_MONOTONIC;
  case CLOCK_BOOTTIME_: return CLOCK_MONOTONIC; // Darwin's CLOCK_MONOTONIC includes sleep time (it is mach_continuous_time)
  default: return -1;
  }
}

int ConvertMmapFlagsForFile(int LinuxFlags) {
  int R = 0;
  if ((LinuxFlags & MAP_TYPE_) == MAP_SHARED_) R |= MAP_SHARED;
  if ((LinuxFlags & MAP_TYPE_) == MAP_PRIVATE_) R |= MAP_PRIVATE;
  return R;
}
} // namespace Linux
