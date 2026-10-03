/*
 * a_launch.c - launch a child with posix_spawn and report exactly how it ended.
 *
 * Why this exists: a shell only says "exit 137" when a Mach-O is refused, which
 * hides whether the kernel rejected the exec (posix_spawn returns an errno such
 * as EBADMACHO = 88) or accepted it and killed the process later (SIGKILL from
 * AMFI or the code-signing monitor).  This tool prints both.
 *
 * usage: a_launch [--4k] [--arch x86_64|arm64|arm64e] [--setexec] SECONDS PATH [ARGS...]
 *   --4k      call posix_spawnattr_set_4k_page_size_np (macOS 26+ SDK, spawn.h)
 *   --arch    posix_spawnattr_setarchpref_np with one preference
 *   --setexec POSIX_SPAWN_SETEXEC (replace this process instead of forking)
 *   SECONDS   kill the child after that long (exit status line says TIMEOUT)
 *
 * Output lines: "LAUNCH ..." (never PASS/FAIL, the caller judges).
 */
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mach/machine.h>

extern char **environ;

int main(int argc, char **argv) {
  int use4k = 0, setexec = 0, i = 1;
  cpu_type_t ct = 0;
  cpu_subtype_t cs = 0;
  const char *archname = "default";
  for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
    if (!strcmp(argv[i], "--4k")) use4k = 1;
    else if (!strcmp(argv[i], "--setexec")) setexec = 1;
    else if (!strcmp(argv[i], "--arch") && i + 1 < argc) {
      archname = argv[++i];
      if (!strcmp(archname, "x86_64")) { ct = CPU_TYPE_X86_64; cs = CPU_SUBTYPE_X86_64_ALL; }
      else if (!strcmp(archname, "arm64")) { ct = CPU_TYPE_ARM64; cs = CPU_SUBTYPE_ARM64_ALL; }
      else if (!strcmp(archname, "arm64e")) { ct = CPU_TYPE_ARM64; cs = CPU_SUBTYPE_ARM64E; }
      else { fprintf(stderr, "unknown arch %s\n", archname); return 2; }
    } else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  if (argc - i < 2) { fprintf(stderr, "usage: a_launch [--4k] [--arch A] [--setexec] SECONDS PATH [ARGS...]\n"); return 2; }
  int secs = atoi(argv[i]);
  const char *path = argv[i + 1];
  char **cargv = &argv[i + 1];

  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  if (use4k) {
    int rc = posix_spawnattr_set_4k_page_size_np(&at);
    printf("LAUNCH set_4k_page_size_np rc=%d (%s)\n", rc, rc ? strerror(rc) : "ok");
  }
  if (ct) {
    size_t cnt = 0;
    int rc = posix_spawnattr_setarchpref_np(&at, 1, &ct, &cs, &cnt);
    printf("LAUNCH setarchpref_np(%s) rc=%d cnt=%zu\n", archname, rc, cnt);
  }
  if (setexec) posix_spawnattr_setflags(&at, POSIX_SPAWN_SETEXEC);
  fflush(stdout);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid_t pid = 0;
  int rc = posix_spawn(&pid, path, NULL, &at, cargv, environ);
  if (rc != 0) {
    const char *name = rc == 88 ? "EBADMACHO" : rc == 86 ? "EBADARCH" : rc == 85 ? "EBADEXEC" : rc == 13 ? "EACCES" : rc == 8 ? "ENOEXEC" : "E?";
    printf("LAUNCH posix_spawn(%s, 4k=%d, arch=%s) FAILED rc=%d %s (%s)\n", path, use4k, archname, rc, name, strerror(rc));
    return 0;
  }
  printf("LAUNCH posix_spawn(%s, 4k=%d, arch=%s) ok pid=%d\n", path, use4k, archname, pid);
  fflush(stdout);
  int st = 0;
  int timed_out = 0;
  for (;;) {
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) break;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (el > secs) { kill(pid, SIGKILL); waitpid(pid, &st, 0); timed_out = 1; break; }
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
  if (timed_out) printf("LAUNCH child TIMEOUT after %d s (killed)\n", secs);
  else if (WIFEXITED(st)) printf("LAUNCH child exited code=%d elapsed=%.3fs\n", WEXITSTATUS(st), el);
  else if (WIFSIGNALED(st)) printf("LAUNCH child killed by signal %d (%s) elapsed=%.3fs\n", WTERMSIG(st), strsignal(WTERMSIG(st)), el);
  else printf("LAUNCH child status=0x%x\n", st);
  return 0;
}
