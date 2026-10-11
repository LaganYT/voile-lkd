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
- w30_scmdev: SCM_RIGHTS of descriptors that LKD emulates, between guest processes in different host processes. A
  child sends /dev/null (O_RDWR|O_NONBLOCK), /dev/zero, /dev/urandom, /dev/full, a synthetic directory (a parent of a
  bind mount, absent from the root), /proc/self, /proc/self/status after reading 6 bytes, an O_PATH descriptor and a
  signalfd; the witness uses each one (fstat numbers 1:3, 1:5, 1:9, 1:7, read and write rules, poll, mmap of
  /dev/zero, getdents64, openat and fchdir relative to the directories, the /proc position, /proc/self/fd links,
  O_PATH rules, its own SIGUSR1 read from the signalfd) and sends three back. Then Wine's case: a child with /dev/null
  as fds 0, 1 and 2 execs the witness, which sends them. Run with the synthetic directory and its entry:
  `FEXDARWIN_BIND="/lkdsyn/w=<dir with bin>:ro" ... fexdarwin-run /lkdsyn/w/w30_scmdev /lkdsyn w`. With the
  fexdarwin-run of c545aa6 (same LKD as 97f227e): `SCM_RIGHTS of a dev object cannot cross host processes`, nothing
  received. After: W30 PASS.
- w31_threadvm: thread creation when the engine has no host memory left for one more thread (each guest thread
  reserves about 280 MiB of host address space). Creates up to 280 waiting threads (optional argument), then joins
  them and creates 50 more; pthread_create may only fail with EAGAIN and the process must live. Run as above, and
  once with `FEXDARWIN_GUEST_BASE=0x7f00000000`, which leaves the host the 30-odd GiB it had before 2026-10-10. With
  the fexdarwin-run of b148137: the thread that found no room dies with `FATAL unhandled fault ... 0x10005297` (also
  without the variable: the old window base left that little room). After: 280 threads; with the variable, 123
  threads, then EAGAIN, W31 PASS.
- w32_bigshared: a 4 GiB MAP_SHARED mapping of a memfd in a process with thousands of mappings, as Neptune maps a
  D3D12 game's committed UPLOAD buffer (ELDEN RING creates three of 4 GiB). Arguments: size in GiB (4), filler
  mappings (4000: every other page of one region made read-only), limit in ms for the mmap (200). Checks that the new
  object reads as zeros, that two views show the same bytes past 4 GiB, and that a view mapped after code ran from
  another view of the same bytes starts write-protected (code rewritten through it runs from the RX view), also when
  the code page sits 32 MiB into the object. With the fexdarwin-run of 7450c4e: mmap of 4 GiB 37.6 ms with 18
  mappings, 13.6 s with 2,017, past 120 s with 8,017. After: 0.03, 0.07 and 0.26 ms, W32 PASS.
- w33_forkwx: the child of a fork runs the engine's dispatcher right after the fork (request wine-5). macOS starts the
  child's thread with the MAP_JIT write state its parent thread had at its last context switch
  (tests/darwin-process/f7_fork_jitstate), so a parent switched out during one of the engine's JIT writes forked a
  child that died with SIGBUS on the dispatcher's first instruction. A caller with 60,000 direct calls to 256 leaves
  is translated once; each of 600 rounds maps the leaves again, runs the caller (every call site is linked), unmaps
  the leaves (the engine undoes all those links at once, one short JIT write each, while 2 spinner threads per CPU
  load the machine), then forks a child that exits at once. Arguments: rounds, call sites, spinner threads. With the
  fexdarwin-run of 8ddfed2: 15 of 600 children died (`FATAL unhandled fault: signal 10`, pc in the dispatcher, esr
  0x8200000f), about 35 s. After: W33 PASS.
- w34_vlocksig: 16 threads copy small results to guest memory as fast as they can (clock_gettime and getrusage, a VMA
  lookup each), 4 threads map and unmap private and shared memory, and a thread sends SIGUSR1 to the copying threads
  (Wine's thread suspension). Arguments: seconds, then `nosig`, `sigidle` (signals to a sleeping thread only) or
  `sigmap` (to the mapping threads only). With fexdarwin-run built from 76bcea8: frozen within a second whenever the
  signalled threads use the VMA lock (every thread waiting on mm->vlock, a writer queued, no holder: macOS 27's
  pthread_rwlock, fact G1); `nosig` and `sigidle` pass. After (mm_rwlock): W34 PASS, 3 runs out of 3, 91 to 99
  million copies in 20 s instead of 27 million without signals before.
- w35_scmpipe: a pipe end sent through SCM_RIGHTS to another process keeps its blocking mode. Wine's case: the child
  creates a pipe, makes its read end non-blocking, sends the write end; the parent must see a blocking descriptor and
  write 256 KiB into it while the child reads slowly. Then a write end sent non-blocking must stay so (EAGAIN when
  full). No argument. With fexdarwin-run built from 76bcea8: the write end arrived non-blocking and the blocking writes
  got EAGAIN 614,628 times (Wine ends a thread whose request write fails that way). After: W35 PASS, 3 runs out of 3.
