#!/bin/bash
# Runs a command in the guest (LKD mode): Debian root read-only, own Wine build (src/wine-linux/install) bound at /opt/wine, private home.
# Usage: run/wine-lkd.sh TIMEOUT_SECONDS guest-command [args...]     env: HOSTRUN (host binary), EXTRA_ENV (--env pairs)
set -u
EXP="$(cd "$(dirname "$0")/.." && pwd)"
HL="$EXP/../rocket-hyperlinux-20260928"
RUN="${HOSTRUN:-$EXP/build-lead/host/fexdarwin-run}"
T="$1"; shift
export FEXDARWIN_LKD=1 FEXDARWIN_ROOT="$HL/linux-root/rootfs" FEXDARWIN_ROOT_RO=1
export FEXDARWIN_BIND="/opt/wine=$EXP/src/wine-linux/install:ro;/home/user=$EXP/run/home-user-own${EXTRA_BIND:+;$EXTRA_BIND}"
exec "$EXP/src/fexdarwin/scripts/run-with-timeout.sh" "$T" "$RUN" \
  --env HOME=/home/user --env USER=user --env LOGNAME=user --env PATH=/opt/wine/bin:/usr/bin:/bin \
  --env LANG=C --env WINEPREFIX=${GUEST_PREFIX:-/home/user/pfx} --env WINEARCH=win64 --env WINEDEBUG="${WINEDEBUG:--all,+err}" \
  --env WINELOADERNOEXEC=0 ${EXTRA_ENV:-} \
  /bin/sh -c '"$0" "$@"; rc=$?; /opt/wine/bin/wineserver -w; exit $rc' "$@"
# The wrapper waits for wineserver to exit, so it saves the registry before run-with-timeout kills the group
# (wineserver writes system.reg/user.reg only on exit, about 3 s after its last client).
