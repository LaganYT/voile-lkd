#!/bin/bash
# Builds the host pieces into their build/ folders: LKD (liblkd.a), fexdarwin-run (FEXCore + LKD), wsd and the
# input service (obelisk-input, which fetches SDL3 once), and with zig (ZIG=<path to zig>, or zig on PATH) the guest
# pieces guest/bin/steam-hostpid.so and guest/bin/xdg-open. Needs Xcode's clang, cmake and ninja (brew install cmake
# ninja) and the fex submodule (git submodule update --init --recursive). LKD builds in Release; BUILD_TYPE=Debug
# builds it unoptimized, for a debugger.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/fex/CMakeLists.txt" ] || { echo "fex submodule missing: git submodule update --init --recursive" >&2; exit 1; }
"$HERE/lkd/build.sh" lkd
"$HERE/fexdarwin/scripts/build.sh"
"$HERE/wsd/build.sh"
"$HERE/input/build.sh"
clang -arch arm64 -O2 -Wall -o "$HERE/wsd/bin/wsdctl" "$HERE/wsd/wsdctl.c"
if [ -n "${ZIG:-}" ] || command -v zig >/dev/null; then
    "$HERE/guest/build.sh"
    ls -l "$HERE/guest/bin/steam-hostpid.so" "$HERE/guest/bin/xdg-open"
else
    echo "zig not found: guest/bin/steam-hostpid.so and guest/bin/xdg-open not built (set ZIG=<path to zig>)" >&2
fi
ls -l "$HERE/fexdarwin/build/host/fexdarwin-run" "$HERE/wsd/bin/wsd" "$HERE/wsd/bin/wsdctl" "$HERE/input/bin/obelisk-input"
