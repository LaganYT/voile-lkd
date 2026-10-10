#!/bin/sh
# Build the guest-side pieces, x86-64 Linux libraries that run inside the engine: bin/steam-hostpid.so (getpid() of
# steamclient.so returns the macOS pid, see steam-hostpid.c). Needs zig (ZIG=<path to the zig binary>, or zig on PATH),
# the compiler Voile builds the guest Wine with; the glibc 2.36 ABI matches the runtime's Debian root.
set -eu
S=$(cd "$(dirname "$0")" && pwd)
ZIG="${ZIG:-$(command -v zig || true)}"
[ -x "$ZIG" ] || { echo "guest/build.sh: zig not found (set ZIG=<path to zig>)" >&2; exit 1; }
mkdir -p "$S/bin"
"$ZIG" cc -target x86_64-linux-gnu.2.36 -shared -fPIC -O2 -g0 -Wall -Wextra -Werror \
  -Wl,-soname,steam-hostpid.so -o "$S/bin/steam-hostpid.so" "$S/steam-hostpid.c"
