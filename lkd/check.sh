#!/bin/sh
# Build everything and run every tests/test_*.c under a timeout.
# Prints PASS/FAIL per test program and a final count; exit 1 if any test fails or the build fails.
here="$(cd "$(dirname "$0")" && pwd)"
python3 "$here/tools/owners_check.py" || { echo "FAIL owners_check"; exit 1; }
"$here/build.sh" || { echo "FAIL build"; exit 1; }
"$here/tools/run_tests.sh" "$here/build/tests"
