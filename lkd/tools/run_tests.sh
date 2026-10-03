#!/bin/sh
# usage: run_tests.sh <dir-with-test-binaries>
# Prints one PASS/FAIL line per test program and a final count. Exit 1 if any fails.
dir="$1"
here="$(cd "$(dirname "$0")" && pwd)"
limit="${LKD_TEST_TIMEOUT:-120}"
pass=0; fail=0; failed=""
for t in "$dir"/test_*; do
  [ -x "$t" ] || continue
  case "$t" in *.dSYM) continue;; esac
  name="$(basename "$t")"
  log="$dir/$name.log"
  "$here/runtest.sh" "$limit" "$t" >"$log" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then
    echo "PASS $name ($(grep '^SUMMARY' "$log" | tail -1 | sed 's/^SUMMARY //'))"
    pass=$((pass+1))
  else
    echo "FAIL $name (exit $rc, log $log)"
    grep '^FAIL ' "$log" | head -5
    fail=$((fail+1)); failed="$failed $name"
  fi
done
echo "TOTAL: $pass passed, $fail failed"
[ $fail -eq 0 ]
