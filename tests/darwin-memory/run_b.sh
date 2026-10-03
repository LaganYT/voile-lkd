#!/bin/bash
# run_b.sh - page-size battery: native arm64, Rosetta default, Rosetta with the 4K spawn attribute,
# plus the refused combinations.  Logs go to evidence/darwin-memory/b_*.log
set -u
DM="$(cd "$(dirname "$0")" && pwd)"
EV="$(cd "$DM/../../evidence/darwin-memory" && pwd)"
BIN="$DM/bin"; LAUNCH="$BIN/a_launch"; TO="$DM/dm_timeout.pl"
run() { # logname launcher-args... path
  local log="$1"; shift
  echo "== $log"
  perl "$TO" 280 "$LAUNCH" "$@" > "$EV/$log.log" 2>&1
}
run b_arm64_native        --arch arm64  270 "$BIN/b_pagesize_arm64"
run b_x86_64_rosetta      --arch x86_64 270 "$BIN/b_pagesize_x86_64"
run b_x86_64_rosetta_4k   --4k --arch x86_64 270 "$BIN/b_pagesize_x86_64"
run b_arm64_native_4k     --4k --arch arm64 270 "$BIN/b_pagesize_arm64"
run b_arm64_4kaligned     --arch arm64 270 "$BIN/b_pagesize_arm64_pz4000_sa1000"
run b_arm64_4kaligned_4k  --4k --arch arm64 270 "$BIN/b_pagesize_arm64_pz4000_sa1000"
echo done
