#!/bin/sh
# Build the owned vtest witness (native arm64, ad-hoc signed).
set -eu
W=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W/bin"
clang -arch arm64 -O2 -Wall -Wextra -o "$W/bin/vtest_client" "$W/vtest_client.c"
codesign -s - -f "$W/bin/vtest_client"
shasum -a 256 "$W/vtest_client.c" "$W/bin/vtest_client"
