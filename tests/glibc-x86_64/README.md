# glibc witnesses (dynamic, Debian rootfs glibc)

Build: `EXP/tools/venv/bin/python -m ziglang cc -target x86_64-linux-gnu.2.36 -O1 -pthread -o bin/NAME src/NAME.c`
Run:   `FEXDARWIN_LKD=1 FEXDARWIN_ROOT=<rootfs> FEXDARWIN_ROOT_RO=1 FEXDARWIN_BIND="/w=<dir with bin>:ro" run-with-timeout.sh 40 fexdarwin-run /w/NAME [seconds]`

- w22_mutexsig: glibc mutex and condvar under a barrage of async signals (kick path). PASS before and after the fix.
- w23_codedatapage: mutex in an rw- 4 KiB page sharing its 16 KiB host page with running r-x code (nptunix.so
  layout). Before the LKD mm/watch.c fix: SIGSEGV (rc 139). After: PASS (exact counter, mutex free).
- w24_condtimedwait: pthread_cond_timedwait on CLOCK_REALTIME times out at 100 ms (30/30).
- w25_hostpid (with w25_steamclient, run by `hostpid.sh`, which builds them with zig): `/proc/<pid>/lkd_host_pid`
  in the main thread, a thread, after fork, exec and posix_spawn, and guest/steam-hostpid.so (getpid() from a library
  named steamclient.so, from elsewhere, in a thread, after a fork). The script checks each host pid against `ps`
  while the witness waits: a live fexdarwin-run, with the expected macOS parent.
  `ROOT=<rootfs> ZIG=<zig> tests/glibc-x86_64/hostpid.sh`. Before the LKD change: no lkd_host_pid file. After: PASS.
- w26_reaper: orphans and their new parent. Wine's double fork: the grandchild goes to the family's init, the
  witness itself (getppid, its /proc/<pid>/status PPid read before it makes another syscall, the parent's comm, exe,
  cmdline and status readable as EAC reads them, SIGCHLD and wait4 by the init). PR_SET_CHILD_SUBREAPER: orphans go
  to the subreaper, which reaps one; its children do not inherit the flag. PR_SET_PDEATHSIG: the signal comes from
  the dying parent (SI_USER, its pid), then again when the subreaper dies, and the witness reaps the orphan. Build
  and run as above, with no argument (5 guest processes at most). With the fexdarwin-run of the -13-dev runtime:
  8 failures (PPid of the dead middle process, getppid 1, parent unreadable, no SIGCHLD, no wait). After: W26 PASS.
- w27_eflags: the RFLAGS a tracer and a signal handler see. Flags set by xor, add (overflow, carry) and cmp just before
  a syscall: in the signal frame of a tgkill to itself (uc_mcontext REG_EFL), and at the getpid entry stops of a traced
  child (NT_PRSTATUS eflags, equal to r11), again after the witness ran code from a page it keeps writing. Checks the
  status flags, bit 1 and IF set, and no reserved bit (3, 5, 15, 22-63). Build and run as above, no argument. With the
  fexdarwin-run of the -13-dev runtime: 11 failures out of 12 contexts, all with bit 31 set (FEXCore's
  ReconstructCompactedEFLAGS packs its NZCV bytes into bits 24-31). After: W27 PASS.
- w29_smcthreads: code rewritten by one thread and run by another, while a third keeps storing data into the same
  16 KiB host page. After a 100 ms warm-up (the storm moves the page to per-instruction checks, fexdarwin
  CodeValidation), 20,000 rounds: the writer patches `mov eax, imm32` to the round number and publishes it, the runner
  serializes (cpuid) and must get that number back. Build and run as above, optional argument the round count. With
  fexdarwin-run built from c545aa6: 2,390 to 6,153 rounds out of 20,000 return the old value (write tracking
  protects a page only after it translated the page's code; a store that lands in between is never seen). After:
  W29 PASS (5 runs out of 5, 20,000 rounds in about 60 ms instead of 1.6 s).
