#!/bin/bash
# Builds the x86-64 Linux witnesses with the private zig toolchain (pip install ziglang in tools/venv).
# Every ELF is linked at a 16 KiB aligned address inside the fexdarwin guest window and with 16 KiB segment alignment.
set -euo pipefail
W="$(cd "$(dirname "$0")" && pwd)"
EXP="$(cd "$W/../.." && pwd)"
PY="$EXP/tools/venv/bin/python"
BASE=${BASE:-0x600000400000}
CC=("$PY" -m ziglang cc -target x86_64-linux-musl)
COMMON=(-static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -fPIE -O1 -g0
        -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,-z,norelro -Wl,--image-base=$BASE -Wl,--build-id=none)
# EXEC=1: fixed-address ET_EXEC at 0x400000 into bin-exec/ (what a Linux kernel runs without any self-relocation; LKD mode).
# Default: static PIE into bin/ (the old host relocates it itself).
OUT=bin
if [ "${EXEC:-0}" = 1 ]; then
  OUT=bin-exec
  COMMON=("${COMMON[@]/-fPIE/-fno-pie}" -no-pie)
  COMMON=("${COMMON[@]/--image-base=*/--image-base=0x400000}")
fi
mkdir -p "$W/$OUT"
LOG="$EXP/evidence/fexcore-darwin/witness-build.log"
echo "# build $(date '+%F %T') zig $("$PY" -m ziglang version) base=$BASE" | tee -a "$LOG"
for src in "$@"; do
  name=$(basename "$src" .c)
  extra=()
  [ -f "$W/src/$name.flags" ] && read -r -a extra < "$W/src/$name.flags"
  echo "+ ${CC[*]} ${COMMON[*]} ${extra[*]:-} -o $OUT/$name src/$name.c" | tee -a "$LOG"
  "${CC[@]}" "${COMMON[@]}" ${extra[@]:-} -o "$W/$OUT/$name" "$W/src/$name.c" 2>&1 | tee -a "$LOG"
  shasum -a 256 "$W/$OUT/$name" | tee -a "$LOG"
done
