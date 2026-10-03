#!/bin/bash
# run_a.sh - run every group-A variant under a_launch (so a refused exec is reported with its exact errno)
# and write one log per variant plus a matrix file into evidence/darwin-memory/.
set -u
DM="$(cd "$(dirname "$0")" && pwd)"
EV="$(cd "$DM/../../evidence/darwin-memory" && pwd)"
BIN="$DM/bin"
LAUNCH="$BIN/a_launch"
TO="$DM/dm_timeout.pl"
MATRIX="$EV/a_matrix.txt"
[ -z "${*:-}" ] && : > "$MATRIX"
ONLY="${*:-}"
for f in "$BIN"/a_arm64_* "$BIN"/a_x86_64_*; do
  case "$f" in *.dSYM) continue;; esac
  [ -x "$f" ] || continue
  n="$(basename "$f")"
  if [ -n "$ONLY" ]; then case " $ONLY " in *" $n "*) ;; *) continue;; esac; fi
  arch=arm64; case "$n" in a_x86_64_*) arch=x86_64;; esac
  echo "== $n"
  perl "$TO" 200 "$LAUNCH" --arch "$arch" 190 "$f" > "$EV/$n.log" 2>&1
  rc=$?
  l="$(/usr/bin/grep -E '^LAUNCH (posix_spawn|child)' "$EV/$n.log" | tr '\n' ' ')"
  s="$(/usr/bin/grep -E '^SUMMARY' "$EV/$n.log" | head -1)"
  echo "$n | $l | ${s:-no SUMMARY line}" >> "$MATRIX"
done
# the 4 KiB spawn attribute (macOS 26 SDK): same binaries, launched with --4k
for n in a_arm64_default a_x86_64_default a_x86_64_pz1000_nopie; do
  [ -x "$BIN/$n" ] || continue
  arch=arm64; case "$n" in a_x86_64_*) arch=x86_64;; esac
  if [ -n "$ONLY" ]; then case " $ONLY " in *" ${n}__4k "*) ;; *) continue;; esac; fi
  echo "== ${n}__4k"
  perl "$TO" 200 "$LAUNCH" --4k --arch "$arch" 190 "$BIN/$n" > "$EV/${n}__4k.log" 2>&1
  l="$(/usr/bin/grep -E '^LAUNCH (posix_spawn|child)' "$EV/${n}__4k.log" | tr '\n' ' ')"
  s="$(/usr/bin/grep -E '^SUMMARY' "$EV/${n}__4k.log" | head -1)"
  echo "${n}__4k | $l | ${s:-no SUMMARY line}" >> "$MATRIX"
done
echo "matrix written to $MATRIX"
