# glibc witnesses (dynamic, Debian rootfs glibc)

Build: `EXP/tools/venv/bin/python -m ziglang cc -target x86_64-linux-gnu.2.36 -O1 -pthread -o bin/NAME src/NAME.c`
Run:   `FEXDARWIN_LKD=1 FEXDARWIN_ROOT=<rootfs> FEXDARWIN_ROOT_RO=1 FEXDARWIN_BIND="/w=<dir with bin>:ro" run-with-timeout.sh 40 fexdarwin-run /w/NAME [seconds]`

- w22_mutexsig: glibc mutex and condvar under a barrage of async signals (kick path). PASS before and after the fix.
- w23_codedatapage: mutex in an rw- 4 KiB page sharing its 16 KiB host page with running r-x code (nptunix.so
  layout). Before the LKD mm/watch.c fix: SIGSEGV (rc 139). After: PASS (exact counter, mutex free).
- w24_condtimedwait: pthread_cond_timedwait on CLOCK_REALTIME times out at 100 ms (30/30).
