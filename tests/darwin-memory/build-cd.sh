#!/bin/sh
# Builds the C and D witnesses (sources c_*.c and d_*.c in src/) into bin/ and
# ad-hoc signs each one in five variants:
#   <name>.plain   ad-hoc, no hardened runtime, no entitlement
#   <name>.rt      ad-hoc, hardened runtime (-o runtime), no entitlement
#   <name>.rtjit   hardened runtime + com.apple.security.cs.allow-jit
#   <name>.rtjitx  hardened runtime + allow-jit + allow-unsigned-executable-memory
#   <name>.jit     no hardened runtime + allow-jit
# Usage: build-cd.sh [name ...]      (default: every c_*.c and d_*.c)
set -eu
W=$(cd "$(dirname "$0")" && pwd)
SRC="$W/src"
OUT="$W/bin"
ENT="$W/entitlements"
mkdir -p "$OUT"
CC=${CC:-clang}
CFLAGS="-O1 -g -std=gnu11 -arch arm64 -mmacosx-version-min=27.0 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Wno-gnu-zero-variadic-macro-arguments -Wno-sign-compare -Wno-unused-function"

if [ "$#" -gt 0 ]; then
    names="$*"
else
    names=$(cd "$SRC" && ls c_*.c d_*.c 2>/dev/null | sed 's/\.c$//')
fi
for name in $names; do
    src="$SRC/$name.c"
    [ -f "$src" ] || { echo "no such source: $src" >&2; exit 1; }
    base="$OUT/$name.unsigned"
    # shellcheck disable=SC2086
    $CC $CFLAGS -I"$SRC" -o "$base" "$src" -lpthread
    for v in plain rt rtjit rtjitx jit; do
        cp "$base" "$OUT/$name.$v"
        case "$v" in
            plain)  codesign -s - -f "$OUT/$name.$v" ;;
            rt)     codesign -s - -f -o runtime "$OUT/$name.$v" ;;
            rtjit)  codesign -s - -f -o runtime --entitlements "$ENT/cd-allow-jit.plist" "$OUT/$name.$v" ;;
            rtjitx) codesign -s - -f -o runtime --entitlements "$ENT/cd-allow-jit-unsigned.plist" "$OUT/$name.$v" ;;
            jit)    codesign -s - -f --entitlements "$ENT/cd-allow-jit.plist" "$OUT/$name.$v" ;;
        esac 2>/dev/null
    done
    rm -f "$base"
    echo "built $name (plain rt rtjit rtjitx jit)"
done
