/* steam-hostpid.so: an LD_PRELOAD library for x86-64 Linux programs run by voile-lkd.
 *
 * getpid() called from Valve's steamclient.so returns the macOS pid of the host process that runs the caller
 * (LKD's /proc/self/lkd_host_pid). Every other caller gets the guest pid, as without the library.
 *
 * Why: steamclient.so sends getpid() with each IPC message to Obelisk's Steam engine on macOS, which resets the pipe
 * 2 to 3 s later unless that pid is a live macOS process. Guest pids are LKD's own (2, 3, ...). Wine must keep them:
 * ntdll gives getpid() to wineserver (init_first_thread, unix_pid), which signals the process's threads with
 * tgkill(unix_pid, unix_tid) and reads it with process_vm_readv. So the library looks at who calls, not at which
 * process it is in. steamclient.so calls getpid through its PLT with a call instruction (22 sites, no tail call, in
 * build 1788652215), so the return address is inside steamclient.so.
 *
 * Outside LKD (no lkd_host_pid file) the guest pid is the real pid and is returned unchanged. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* The guest pid in the high half, the pid to return in the low half. One word, so that a thread never pairs the guest
 * pid of one process with the host pid of another (a fork copies the cache; guest pids are never reused). */
static _Atomic uint64_t g_cache;

static pid_t read_host_pid(void)
{
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/lkd_host_pid", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[16];
    long n = syscall(SYS_read, fd, buf, sizeof buf - 1);
    syscall(SYS_close, fd);
    if (n <= 0) return -1;
    long v = 0;
    for (long i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
    return v > 0 && v <= 0x7fffffff ? (pid_t)v : -1;
}

static int from_steamclient(void *caller)
{
    struct dl_find_object o;
    if (_dl_find_object(caller, &o) != 0 || !o.dlfo_link_map || !o.dlfo_link_map->l_name) return 0;
    const char *name = o.dlfo_link_map->l_name;
    const char *base = strrchr(name, '/');
    return strcmp(base ? base + 1 : name, "steamclient.so") == 0;
}

pid_t getpid(void)
{
    pid_t pid = (pid_t)syscall(SYS_getpid);
    if (!from_steamclient(__builtin_return_address(0))) return pid;
    uint64_t c = atomic_load_explicit(&g_cache, memory_order_relaxed);
    if ((pid_t)(c >> 32) == pid) return (pid_t)(uint32_t)c;
    pid_t host = read_host_pid();
    if (host <= 0) host = pid;
    atomic_store_explicit(&g_cache, (uint64_t)(uint32_t)pid << 32 | (uint32_t)host, memory_order_relaxed);
    return host;
}
