#!/bin/sh
# Configure and build LKD into ./build with the private cmake and ninja.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
CMAKE="${CMAKE:-cmake}"
NINJA="${NINJA:-$(command -v ninja)}"
build="$here/build"
"$CMAKE" -S "$here" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" >"$build.configure.log" 2>&1 || { cat "$build.configure.log"; exit 1; }
# Remove test binaries whose source file is gone, so check.sh never runs stale programs.
if [ -d "$build/tests" ]; then
  for f in "$build"/tests/test_*; do
    [ -e "$f" ] || continue
    n="$(basename "$f")"; n="${n%.log}"; n="${n%.dSYM}"
    [ -f "$here/tests/$n.c" ] || rm -rf "$f"
  done
fi
"$CMAKE" --build "$build" -- -j "${JOBS:-4}" "$@"
