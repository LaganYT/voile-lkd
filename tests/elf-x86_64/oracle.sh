#!/bin/bash
# Builds a witness as a macOS x86_64 program (-DWITNESS_HOSTED) and runs it under Rosetta 2. Rosetta is only used as an
# independent oracle for the expected values; it is not part of the fexdarwin solution.
# Usage: oracle.sh name [extra clang flags]
set -uo pipefail
W="$(cd "$(dirname "$0")" && pwd)"
EXP="$(cd "$W/../.." && pwd)"
name="$1"; shift
OUT="$EXP/evidence/fexcore-darwin/oracle"
mkdir -p "$W/oracle" "$OUT"
clang -arch x86_64 -O1 -DWITNESS_HOSTED -fno-builtin -I"$W/src" "$@" -o "$W/oracle/$name" "$W/src/$name.c" 2>&1 | grep -v "warning\|note:\|^ *[0-9]* |\|^ *|\|\^" 
log="$OUT/$name-rosetta-$(date +%H%M%S).log"
{
  echo "# oracle $name (macOS x86_64 under Rosetta 2) $(date '+%F %T')"
  echo "# binary sha256 $(shasum -a 256 "$W/oracle/$name" | cut -d' ' -f1)"
  "$EXP/src/fexdarwin/scripts/run-with-timeout.sh" 120 arch -x86_64 "$W/oracle/$name"
  echo "# exit status $?"
} > "$log" 2>&1
tail -5 "$log"; echo "-> $log"
