#!/bin/bash
# Builds the three host pieces into their build/ folders: LKD (liblkd.a), fexdarwin-run (FEXCore + LKD) and wsd.
# Needs Xcode's clang, cmake and ninja (brew install cmake ninja) and the fex submodule
# (git submodule update --init --recursive). BUILD_TYPE=Release builds LKD without debug checks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/fex/CMakeLists.txt" ] || { echo "fex submodule missing: git submodule update --init --recursive" >&2; exit 1; }
"$HERE/lkd/build.sh" lkd
"$HERE/fexdarwin/scripts/build.sh"
"$HERE/wsd/build.sh"
clang -arch arm64 -O2 -Wall -o "$HERE/wsd/bin/wsdctl" "$HERE/wsd/wsdctl.c"
ls -l "$HERE/fexdarwin/build/host/fexdarwin-run" "$HERE/wsd/bin/wsd" "$HERE/wsd/bin/wsdctl"
