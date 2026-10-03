#!/bin/bash
# Runs witnesses through fexdarwin-run in LKD mode (FEXDARWIN_LKD=1): the guest root is this directory, so a witness is
# the guest path /bin/<name>. Raw output goes to evidence/fexcore-darwin/runs-lkd/.
# Usage: run-lkd.sh [-x EXPECT_EXIT] name [name...]      env: TIMEOUT (s), LKD_TRACE=1 for the syscall trace
set -uo pipefail
W="$(cd "$(dirname "$0")" && pwd)"
EXP="$(cd "$W/../.." && pwd)"
RUN="${RUN:-$EXP/src/fexdarwin/build/host/fexdarwin-run}"
TO="$EXP/src/fexdarwin/scripts/run-with-timeout.sh"
OUT="$EXP/evidence/fexcore-darwin/runs-lkd"
mkdir -p "$OUT"
TIMEOUT=${TIMEOUT:-60}
EXPECT=0
if [ "${1:-}" = "-x" ]; then EXPECT="$2"; shift 2; fi
rc_all=0
for name in "$@"; do
  log="$OUT/$name-$(date +%H%M%S).log"
  {
    echo "# witness $name (LKD mode)"
    echo "# date    $(date '+%F %T %Z')"
    echo "# binary  $(shasum -a 256 "$W/bin-exec/$name" | cut -d' ' -f1)"
    echo "# host    $(shasum -a 256 "$RUN" | cut -d' ' -f1)"
    echo "# lib     $(shasum -a 256 "$EXP/src/lkd/build/lib/liblkd.a" 2>/dev/null | cut -d' ' -f1)"
    echo "# ---- output (stdout and stderr interleaved) ----"
  } > "$log"
  FEXDARWIN_LKD=1 FEXDARWIN_ROOT="$W" "$TO" "$TIMEOUT" "$RUN" ${HOSTOPT:-} "/bin-exec/$name" >> "$log" 2>&1
  rc=$?
  echo "# ---- exit status $rc (expected $EXPECT) ----" >> "$log"
  if [ "$rc" = "$EXPECT" ]; then echo "[$name] exit $rc as expected -> $log"; else echo "[$name] exit $rc (expected $EXPECT) -> $log"; rc_all=1; fi
done
exit $rc_all
