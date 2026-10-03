#!/bin/sh
# Builds every witness into ./bin and ad-hoc signs it.
#   normal binaries get com.apple.security.get-task-allow (so the H tests can use them as debuggees)
#   <name>.noent copies are signed with no entitlement at all (for comparisons)
# Usage: build.sh [name ...]   (default: all *.c)
set -eu
W=$(cd "$(dirname "$0")" && pwd)
OUT="$W/bin"
mkdir -p "$OUT"
CC=${CC:-clang}
CFLAGS="-O1 -g -std=gnu11 -arch arm64 -mmacosx-version-min=27.0 -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-gnu-zero-variadic-macro-arguments -Wno-sign-compare"

extra_flags() {
    case "$1" in
        g2_kqueue)        echo "-framework CoreServices -framework CoreFoundation" ;;
        f6_lowaddr_probe) echo "-Wl,-pagezero_size,0x4000" ;;
        *)                echo "" ;;
    esac
}

if [ "$#" -gt 0 ]; then names="$*"; else names=$(cd "$W" && ls *.c | sed 's/\.c$//'); fi
for name in $names; do
    src="$W/$name.c"
    [ -f "$src" ] || { echo "no such source: $src" >&2; exit 1; }
    # shellcheck disable=SC2046
    $CC $CFLAGS -o "$OUT/$name" "$src" $(extra_flags "$name") -lpthread
    codesign -s - -f --entitlements "$W/entitlements-debuggee.plist" "$OUT/$name" 2>/dev/null
    cp "$OUT/$name" "$OUT/$name.noent"
    codesign -s - -f "$OUT/$name.noent" 2>/dev/null
    echo "built $name"
done
