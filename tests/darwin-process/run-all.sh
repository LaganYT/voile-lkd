#!/bin/sh
# Runs every witness with a hard timeout and stores one log per program in the evidence directory.
# Usage: run-all.sh [name ...]
set -u
W=$(cd "$(dirname "$0")" && pwd)
E=${DP_EVIDENCE:-"$W/../../evidence/darwin-process"}
mkdir -p "$E"
E=$(cd "$E" && pwd)
export DP_TMPDIR=${DP_TMPDIR:-${TMPDIR:-/private/tmp}}
if [ "$#" -gt 0 ]; then names="$*"; else names=$(cd "$W/bin" && ls | grep -v '\.noent$' | sort); fi
for name in $names; do
    case "$name" in *.sh) continue ;; esac
    budget=${DP_BUDGET:-240}
    log="$E/$name.log"
    {
        echo "# $name started $(date '+%Y-%m-%d %H:%M:%S %z') budget=${budget}s"
        perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$budget" "$W/bin/$name"
        echo "# $name exit status $?"
    } > "$log" 2>&1
    sum=$(grep -E '^SUMMARY' "$log" | tail -1)
    echo "$name: ${sum:-no summary} ($(grep -c '^FAIL' "$log") FAIL lines)"
done
