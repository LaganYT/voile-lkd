#!/bin/bash
# Runs witnesses through fexdarwin-run with a timeout and archives the raw output under evidence/fexcore-darwin/runs/.
# Usage: run.sh [-x EXPECT_EXIT] [--hostopt ...] name [name...]   (name = binary in bin/)
set -uo pipefail
W="$(cd "$(dirname "$0")" && pwd)"
EXP="$(cd "$W/../.." && pwd)"
RUN="$EXP/src/fexdarwin/build/host/fexdarwin-run"
TO="$EXP/src/fexdarwin/scripts/run-with-timeout.sh"
OUT="$EXP/evidence/fexcore-darwin/runs"
mkdir -p "$OUT"
TIMEOUT=${TIMEOUT:-60}
EXTRA=()
EXPECT=0
while [ $# -gt 0 ]; do
  case "$1" in
    -x) EXPECT="$2"; shift 2;;
    --hostopt) EXTRA+=("$2"); shift 2;;
    *) break;;
  esac
done
rc_all=0
for name in "$@"; do
  stamp=$(date +%H%M%S)
  tag=$(printf '%s' "${EXTRA[*]:-}" | tr -c 'A-Za-z0-9=\n' '_' | sed 's/^_*//; s/_*$//')
  log="$OUT/$name${tag:+-$tag}-$stamp.log"
  {
    echo "# witness $name"
    echo "# date    $(date '+%F %T %Z')"
    echo "# binary  $(shasum -a 256 "$W/bin/$name" | cut -d' ' -f1)"
    echo "# host    $(shasum -a 256 "$RUN" | cut -d' ' -f1)"
    echo "# command ${EXTRA[*]:-} $RUN ${EXTRA[*]:-} bin/$name"
    echo "# ---- output (stdout and stderr interleaved) ----"
  } > "$log"
  "$TO" "$TIMEOUT" "$RUN" ${EXTRA[@]:-} "$W/bin/$name" >> "$log" 2>&1
  rc=$?
  echo "# ---- exit status $rc (expected $EXPECT) ----" >> "$log"
  if [ "$rc" = "$EXPECT" ]; then echo "[$name] exit $rc as expected -> $log"; else echo "[$name] exit $rc (expected $EXPECT) -> $log"; rc_all=1; fi
done
exit $rc_all
