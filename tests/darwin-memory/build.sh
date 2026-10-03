#!/bin/bash
# build.sh - build every darwin-memory witness (groups a_, b_, e_) and its signing variants.
# Usage: build.sh            (builds into ./bin)
# Nothing here needs root.  codesign uses the ad-hoc identity "-".
set -u
DM="$(cd "$(dirname "$0")" && pwd)"
SRC="$DM/src"; BIN="$DM/bin"; ENT="$DM/entitlements"; LOG="$DM/bin/build.log"
mkdir -p "$BIN"
: > "$LOG"
CFLAGS="-O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable"

build() { # name arch source extra-ld-flags...
  local name="$1" arch="$2" src="$3"; shift 3
  if clang $CFLAGS -arch "$arch" "$@" -o "$BIN/$name" "$SRC/$src" >>"$LOG" 2>&1; then
    echo "built $name ($arch)" | tee -a "$LOG"
  else
    echo "BUILD FAILED $name ($arch)" | tee -a "$LOG"
  fi
}
sign() { # name suffix [codesign args...]  -> copies bin/name to bin/name_suffix and re-signs
  local name="$1" suffix="$2"; shift 2
  cp "$BIN/$name" "$BIN/${name}_${suffix}"
  codesign -f -s - "$@" "$BIN/${name}_${suffix}" >>"$LOG" 2>&1 && echo "signed ${name}_${suffix} [$*]" | tee -a "$LOG" || echo "SIGN FAILED ${name}_${suffix}" | tee -a "$LOG"
}

# launcher (arm64 only)
build a_launch arm64 a_launch.c

# ---- group A: address space ----
for arch in arm64 x86_64; do
  p=a_${arch}
  build ${p}_default $arch a_addrspace.c
  build ${p}_pz4000 $arch a_addrspace.c -Wl,-pagezero_size,0x4000
  build ${p}_pz1000 $arch a_addrspace.c -Wl,-pagezero_size,0x1000
  build ${p}_pz1000_sa1000 $arch a_addrspace.c -Wl,-pagezero_size,0x1000 -Wl,-segalign,0x1000
  build ${p}_pz0 $arch a_addrspace.c -Wl,-pagezero_size,0
  build ${p}_pzFFFF0000 $arch a_addrspace.c -Wl,-pagezero_size,0xFFFF0000
  build ${p}_pz200000000 $arch a_addrspace.c -Wl,-pagezero_size,0x200000000
done
# x86_64 (Rosetta) only: non-PIE variants, the linker honours -no_pie for x86_64 and the image is not slid
for pz in 0x1000 0x4000 0x10000; do
  build a_x86_64_pz${pz#0x}_nopie x86_64 a_addrspace.c -Wl,-pagezero_size,$pz -Wl,-no_pie
done
build a_x86_64_default_nopie x86_64 a_addrspace.c -Wl,-no_pie
# entitlement and hardened-runtime variants of the default arm64 build
B=a_arm64_default
sign $B adhoc
sign $B hr --options runtime
sign $B extva --entitlements "$ENT/ent_extva.plist"
sign $B extva_hr --options runtime --entitlements "$ENT/ent_extva.plist"
sign $B incmem --entitlements "$ENT/ent_incmem.plist"
sign $B incmem_hr --options runtime --entitlements "$ENT/ent_incmem.plist"
sign $B both --entitlements "$ENT/ent_both.plist"
sign $B both_hr --options runtime --entitlements "$ENT/ent_both.plist"
sign $B jit_hr --options runtime --entitlements "$ENT/ent_jit.plist"
# the same entitlement variants for the x86_64 default build (Rosetta)
B=a_x86_64_default
sign $B adhoc
sign $B hr --options runtime
sign $B both --entitlements "$ENT/ent_both.plist"

# ---- group B: page size ----
for arch in arm64 x86_64; do
  build b_pagesize_${arch} $arch b_pagesize.c
done
build b_pagesize_arm64_pz4000_sa1000 arm64 b_pagesize.c -Wl,-pagezero_size,0x4000 -Wl,-segalign,0x1000

# ---- group E: CPU features ----
for arch in arm64 x86_64; do
  build e_cpufeat_${arch} $arch e_cpufeat.c
done
sign e_cpufeat_arm64 hr --options runtime
echo "build finished" | tee -a "$LOG"
