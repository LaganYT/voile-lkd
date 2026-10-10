# voile-lkd

The Linux engine of [Voile](https://github.com/Zagrios/voile): it runs x86-64 **Linux** programs, unmodified,
in native macOS arm64 processes, with no virtual machine and no Hypervisor.framework. Obelisk uses it for Windows
games whose anti-cheat supports Linux (Easy Anti-Cheat on Rocket League): the game runs in voile-wine built for
Linux, the anti-cheat loads its Linux module and sees a real Linux kernel interface.

Private. See [docs/V1.md](docs/V1.md) for what works, the measurements and the known limits.

## Pieces

| Folder | What it is |
|---|---|
| `lkd/` | **LKD**, Linux kernel on Darwin: a C library that implements about 375 Linux x86-64 system calls (memory, files, processes and threads, signals, futex, sockets, procfs, ptrace) on top of macOS. One host process per guest process. |
| `fexdarwin/` | The host program, `fexdarwin-run`: FEXCore translates the guest's x86-64 code to arm64, every guest `SYSCALL` goes to LKD. Signals, thread kicks, self-modifying code, fork/exec, the safety watchdog and a guest thread dump (`FEXDARWIN_DUMP_DIR`). |
| `fex/` | Submodule [Zagrios/FEX](https://github.com/Zagrios/FEX): FEX-Emu with the Darwin port. |
| `wsd/` | The window server: Cocoa windows, Metal presentation of the frames the guest renders (zero copy), keyboard and mouse events, and audio output through CoreAudio. The guest's Wine drivers (`wineobelisk.drv`, `wineobeliskaudio.drv`, Neptune's `nptunix`) talk to it over a UNIX socket (`wsd/PROTOCOL.md`, `wsd/wsd_protocol.h`). `wsdctl` injects test input. |
| `guest/` | x86-64 Linux libraries for the guest, built with zig: `steam-hostpid.so`, an `LD_PRELOAD` library that makes `getpid()` return the macOS pid of the host process when Valve's `steamclient.so` calls it (`/proc/<pid>/lkd_host_pid`); Obelisk's Steam engine drops a client whose pid is not a live macOS process. |
| `tests/` | Witness programs: x86-64 ELF (`elf-x86_64`, freestanding), glibc (`glibc-x86_64`), Darwin facts, graphics, the EAC probe campaign. |
| `tools/dev/` | Development scripts that drive a whole session (authenticated Rocket League launch, offline Free Play, mouse stress). |
| `tools/epic-launch/` | Builds an Epic launch plan with Obelisk's `epic-kit` (development only; Obelisk does this itself). |
| `docs/` | `V1.md`, and `journal/`, the research and integration journal (French). |

Apart from `guest/`, what runs inside the guest comes from elsewhere: voile-wine built for Linux x86-64, a Debian
x86-64 root, Neptune's Direct3D client DLLs; Voile builds and ships them.

## Building

```sh
git clone --recursive https://github.com/Zagrios/voile-lkd.git
brew install cmake ninja
./build.sh
```

Results: `lkd/build/lib/liblkd.a`, `fexdarwin/build/host/fexdarwin-run`, `wsd/bin/wsd`, `wsd/bin/wsdctl`, and with
zig (`ZIG=<path to zig>` or zig on `PATH`) `guest/bin/steam-hostpid.so`.
LKD's own tests: `lkd/check.sh`.

## Running a guest program

```sh
FEXDARWIN_LKD=1 FEXDARWIN_ROOT=<x86-64 Debian root> FEXDARWIN_ROOT_RO=1 \
FEXDARWIN_BIND="/opt/wine=<wine install>:ro;/home/user=<home>" \
fexdarwin/scripts/run-with-timeout.sh 60 fexdarwin/build/host/fexdarwin-run \
  --env HOME=/home/user /opt/wine/bin/wine cmd.exe
```

Always go through `run-with-timeout.sh` (or any supervisor that sets `FEXDARWIN_SUPERVISOR_PID`): every guest
process ends itself within half a second once its supervisor is gone. Runaway guests once exhausted the kernel's
VM map entries and panicked the Mac (`docs/V1.md`, safety).

The scripts in `tools/dev/` and most of `tests/` expect the development workspace they were written in (`EXP`: the
Debian root, the Wine build, a clone of the game, the evidence folders); they are kept as they ran for v1.
