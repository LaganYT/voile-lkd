/*
 * common.h - shared helpers for the darwin-memory witnesses.
 *
 * Output convention: every test prints lines "PASS <id> ...", "FAIL <id> ..."
 * or "INFO <id> ...".  PASS means the behaviour FEXCore would need is present
 * (or the stated expectation holds), FAIL means it is absent or the expectation
 * is wrong, INFO is a plain measurement.  "NOTTESTABLE <id> ..." marks a fact
 * that cannot be measured here and says why.
 *
 * Nothing in this file needs root, entitlements or the network.
 */
#ifndef DM_COMMON_H
#define DM_COMMON_H

#define _DARWIN_C_SOURCE 1

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libkern/OSCacheControl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>

static int g_pass, g_fail, g_info;

#define DM_OUT(kind, id, ...)                                                  \
  do {                                                                         \
    printf("%s %s ", kind, id);                                                \
    printf(__VA_ARGS__);                                                       \
    printf("\n");                                                              \
    fflush(stdout);                                                            \
  } while (0)
#define PASS(id, ...) do { g_pass++; DM_OUT("PASS", id, __VA_ARGS__); } while (0)
#define FAIL(id, ...) do { g_fail++; DM_OUT("FAIL", id, __VA_ARGS__); } while (0)
#define INFO(id, ...) do { g_info++; DM_OUT("INFO", id, __VA_ARGS__); } while (0)
#define NOTTESTABLE(id, ...) do { DM_OUT("NOTTESTABLE", id, __VA_ARGS__); } while (0)
#define CHECK(cond, id, ...)                                                   \
  do {                                                                         \
    if (cond) PASS(id, __VA_ARGS__); else FAIL(id, __VA_ARGS__);               \
  } while (0)

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static const char *errno_name(int e) {
  switch (e) {
  case 0: return "0";
  case EPERM: return "EPERM";
  case ENOENT: return "ENOENT";
  case ESRCH: return "ESRCH";
  case EINTR: return "EINTR";
  case EIO: return "EIO";
  case ENXIO: return "ENXIO";
  case ENOEXEC: return "ENOEXEC";
  case EBADF: return "EBADF";
  case ENOMEM: return "ENOMEM";
  case EACCES: return "EACCES";
  case EFAULT: return "EFAULT";
  case EBUSY: return "EBUSY";
  case EEXIST: return "EEXIST";
  case ENODEV: return "ENODEV";
  case EINVAL: return "EINVAL";
  case EMFILE: return "EMFILE";
  case EAGAIN: return "EAGAIN";
  case ENOTSUP: return "ENOTSUP";
  case EOVERFLOW: return "EOVERFLOW";
  case 86: return "EBADARCH";
  case 87: return "ESHLIBVERS";
  case 88: return "EBADMACHO";
  default: return "E?";
  }
}

static const char *kr_name(kern_return_t kr) {
  switch (kr) {
  case KERN_SUCCESS: return "KERN_SUCCESS";
  case KERN_INVALID_ADDRESS: return "KERN_INVALID_ADDRESS";
  case KERN_PROTECTION_FAILURE: return "KERN_PROTECTION_FAILURE";
  case KERN_NO_SPACE: return "KERN_NO_SPACE";
  case KERN_INVALID_ARGUMENT: return "KERN_INVALID_ARGUMENT";
  case KERN_FAILURE: return "KERN_FAILURE";
  case KERN_RESOURCE_SHORTAGE: return "KERN_RESOURCE_SHORTAGE";
  case KERN_NO_ACCESS: return "KERN_NO_ACCESS";
  case KERN_MEMORY_FAILURE: return "KERN_MEMORY_FAILURE";
  case KERN_MEMORY_ERROR: return "KERN_MEMORY_ERROR";
  case KERN_NOT_SUPPORTED: return "KERN_NOT_SUPPORTED";
  case KERN_INVALID_RIGHT: return "KERN_INVALID_RIGHT";
  case KERN_INVALID_CAPABILITY: return "KERN_INVALID_CAPABILITY";
  case KERN_DENIED: return "KERN_DENIED";
  case KERN_ABORTED: return "KERN_ABORTED";
  default: return "KERN_?";
  }
}

static const char *arch_name(void) {
#if defined(__arm64__) || defined(__aarch64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x86_64";
#else
  return "unknown";
#endif
}

static int is_translated(void) {
  int v = 0;
  size_t l = sizeof v;
  if (sysctlbyname("sysctl.proc_translated", &v, &l, NULL, 0) != 0) return -1;
  return v;
}

static const char *prot_str(int p, char *buf) {
  buf[0] = (p & 1) ? 'r' : '-';
  buf[1] = (p & 2) ? 'w' : '-';
  buf[2] = (p & 4) ? 'x' : '-';
  buf[3] = 0;
  return buf;
}

/* Watchdog: SIGALRM prints and exits.  Every program arms it in main(). */
static void dm_watchdog_handler(int sig) {
  (void)sig;
  static const char m[] = "FAIL WATCHDOG program exceeded its time budget\n";
  (void)!write(1, m, sizeof m - 1);
  _exit(124);
}
static void dm_arm_watchdog(unsigned seconds) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = dm_watchdog_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGALRM, &sa, NULL);
  alarm(seconds);
}

/* Basic region lookup through mach_vm_region (VM_REGION_BASIC_INFO_64). */
typedef struct {
  mach_vm_address_t start;
  mach_vm_size_t size;
  vm_prot_t prot, maxprot;
  int share_mode;
  bool valid;
} dm_region;

static dm_region dm_region_at_or_after(mach_vm_address_t addr) {
  dm_region r;
  memset(&r, 0, sizeof r);
  mach_vm_address_t a = addr;
  mach_vm_size_t sz = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t obj = MACH_PORT_NULL;
  kern_return_t kr = mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info, &cnt, &obj);
  if (kr != KERN_SUCCESS) return r;
  r.valid = true;
  r.start = a;
  r.size = sz;
  r.prot = info.protection;
  r.maxprot = info.max_protection;
  r.share_mode = info.shared;
  return r;
}

/* true if [addr, addr+size) has no mapping at all */
static bool dm_range_is_free(mach_vm_address_t addr, mach_vm_size_t size) {
  dm_region r = dm_region_at_or_after(addr);
  if (!r.valid) return true;
  return r.start >= addr + size;
}

/* Protection of the single page containing addr, or -1 if unmapped. */
static int dm_prot_at(mach_vm_address_t addr, vm_prot_t *maxprot_out, mach_vm_address_t *rstart, mach_vm_size_t *rsize) {
  dm_region r = dm_region_at_or_after(addr);
  if (!r.valid || r.start > addr) return -1;
  if (maxprot_out) *maxprot_out = r.maxprot;
  if (rstart) *rstart = r.start;
  if (rsize) *rsize = r.size;
  return r.prot;
}

#endif
