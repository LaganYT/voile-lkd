#!/bin/sh
# Builds the f_ and g_ witnesses into ./bin and ad-hoc signs them (same flags as build.sh, own per-program flag table).
# Usage: fg_build.sh [name ...]   (default: every f*.c and g*.c)
# No file is shared with build.sh except common.h and entitlements-debuggee.plist (read only).
set -eu
W=$(cd "$(dirname "$0")" && pwd)
OUT="$W/bin"
mkdir -p "$OUT"
CC=${CC:-clang}
CFLAGS="-O1 -g -std=gnu11 -arch arm64 -mmacosx-version-min=27.0 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-gnu-zero-variadic-macro-arguments -Wno-sign-compare"

extra_flags() {
    case "$1" in
        g2_kqueue) echo "-framework CoreServices -framework CoreFoundation" ;;
        *)         echo "" ;;
    esac
}

if [ "$#" -gt 0 ]; then names="$*"; else names=$(cd "$W" && ls f*.c g*.c 2>/dev/null | sed 's/\.c$//'); fi
for name in $names; do
    src="$W/$name.c"
    [ -f "$src" ] || { echo "no such source: $src" >&2; exit 1; }
    # shellcheck disable=SC2046
    $CC $CFLAGS -o "$OUT/$name" "$src" $(extra_flags "$name") -lpthread
    codesign -s - -f --entitlements "$W/entitlements-debuggee.plist" "$OUT/$name" 2>/dev/null
    echo "built $name"
done
