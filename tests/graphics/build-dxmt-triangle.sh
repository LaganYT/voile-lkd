#!/bin/sh
# Build the DXMT triangle witness (Objective-C++, arm64, ad-hoc signed). Headers: DXMT's MinGW D3D11 headers.
set -eu
W=$(cd "$(dirname "$0")" && pwd)
DX="$W/../../src/graphics-spike/dxmt/include/native"
mkdir -p "$W/bin"
clang++ -arch arm64 -x objective-c++ -std=c++20 -fobjc-arc -O1 -Wno-everything \
  -I"$W" -I"$DX/windows" -I"$DX/directx" "$W/dxmt_triangle.m" -o "$W/bin/dxmt_triangle" -framework Foundation
codesign -s - -f "$W/bin/dxmt_triangle"
shasum -a 256 "$W/dxmt_triangle.m" "$W/bin/dxmt_triangle"
