#!/bin/bash
# Runs a command in the guest (LKD mode): Debian root read-only, GE-Proton bound at /opt/ge-proton, private home.
# Usage: run/wine-lkd.sh TIMEOUT_SECONDS guest-command [args...]     env: HOSTRUN (host binary), EXTRA_ENV (--env pairs)
set -u
EXP="$(cd "$(dirname "$0")/.." && pwd)"
HL="$EXP/../rocket-hyperlinux-20260928"
RUN="${HOSTRUN:-$EXP/src/fexdarwin/build/host/fexdarwin-run}"
T="$1"; shift
export FEXDARWIN_LKD=1 FEXDARWIN_ROOT="$HL/linux-root/rootfs" FEXDARWIN_ROOT_RO=1
export FEXDARWIN_BIND="/opt/ge-proton=$HL/ge-proton/GE-Proton11-7-x86_64:ro;/home/user=$EXP/run/home-user"
exec "$EXP/src/fexdarwin/scripts/run-with-timeout.sh" "$T" "$RUN" \
  --env HOME=/home/user --env USER=user --env LOGNAME=user --env PATH=/opt/ge-proton/files/bin:/usr/bin:/bin \
  --env LANG=C --env WINEPREFIX=/home/user/pfx --env WINEARCH=win64 --env WINEDEBUG="${WINEDEBUG:--all,+err}" \
  --env WINELOADERNOEXEC=0 ${EXTRA_ENV:-} "$@"
