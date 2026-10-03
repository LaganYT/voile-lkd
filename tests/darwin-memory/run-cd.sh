#!/bin/sh
# Runs the C and D witnesses with a hard timeout and stores one log per
# program and variant in evidence/darwin-memory/.
# Usage: run-cd.sh <variant|all> [name ...]      e.g. run-cd.sh plain c_jit_basic
set -u
W=$(cd "$(dirname "$0")" && pwd)
E=${CD_EVIDENCE:-"$W/../../evidence/darwin-memory"}
mkdir -p "$E"
E=$(cd "$E" && pwd)
variant=${1:-plain}
[ "$#" -gt 0 ] && shift
if [ "$variant" = all ]; then variants="plain rt rtjit rtjitx jit"; else variants="$variant"; fi
if [ "$#" -gt 0 ]; then names="$*"; else names=$(cd "$W/src" && ls c_*.c d_*.c | sed 's/\.c$//'); fi
budget=${CD_BUDGET:-240}
for v in $variants; do
  for name in $names; do
    bin="$W/bin/$name.$v"
    [ -x "$bin" ] || { echo "missing $bin" >&2; continue; }
    log="$E/$name.$v.log"
    {
        echo "# $name.$v started $(date '+%Y-%m-%d %H:%M:%S %z') budget=${budget}s"
        perl -e 'my $t=shift; my $pid=fork(); if(!$pid){ exec @ARGV or die "exec: $!"; } local $SIG{ALRM}=sub{ kill 9,$pid; print "# HARNESS TIMEOUT: killed\n"; }; alarm $t; waitpid($pid,0); my $s=$?; print "# exit status raw=$s", ($s & 127 ? " signal=".($s&127) : " code=".($s>>8)), "\n";' "$budget" "$bin"
    } > "$log" 2>&1
    echo "$name.$v: $(grep -c '^PASS' "$log") PASS, $(grep -c '^FAIL' "$log") FAIL, $(grep -c '^INFO' "$log") INFO, $(grep -c '^NOTTESTABLE' "$log") NOTTESTABLE"
  done
done
