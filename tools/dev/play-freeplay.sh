#!/bin/bash
# Offline Free Play script for one eac-run.py run (started separately): title, menu probes, Free Play, probes, 60 s
# straight drive with samples, exit through the menus. Input via wsdctl (WSD_DEBUG_INPUT), captures by window id only.
# $1 = tag for captures and samples.
EXP="$(cd "$(dirname "$0")/.." && pwd)"; cd "$EXP/run"
TAG=${1:-run}; C=../src/wsd/bin/wsdctl; S=wsd-eac/wsd.sock
L="home-user-own/pfx-eac/drive_c/users/crossover/Documents/My Games/Rocket League/TAGame/Logs/Launch.log"
CAP="$EXP/evidence/wine-linux/play/$TAG"; SMP="$EXP/logs/wine-linux-samples"; mkdir -p "$(dirname "$CAP")" "$SMP"
log() { echo "$(date +%T) $*"; }
alive() { pgrep -x fexdarwin-run >/dev/null; }  # by process name: -f also matches shells that mention the script
waitfor() { # pattern count timeout
  for _ in $(seq 1 "$3"); do alive || exit 1; [ "$(grep -c "$1" "$L" 2>/dev/null)" -ge "$2" ] && return 0; sleep 1; done; return 1; }
key() { alive || exit 1; $C $S tap "$1"; }
click() { alive || exit 1; $C $S click "$1" "$2"; }
for _ in $(seq 1 120); do [ "$(grep -c '' "$L" 2>/dev/null || echo 0)" -lt 50 ] 2>/dev/null && break; sleep 1; done
waitfor "EULA sync response" 1 300 || { log "no menu"; exit 1; }
log "eula"; sleep 8; key 36; sleep 20; "$EXP/run/cap-game.sh" "$CAP-menu0" >/dev/null; log "menu probes"
for k in 125 125 125 126 126 126; do key $k; sleep 2; done
"$EXP/run/cap-game.sh" "$CAP-menu1" >/dev/null
click 140 325; sleep 4; click 360 460; sleep 4; click 330 270; sleep 4; click 330 380; sleep 4; click 640 495
log "freeplay requested"
waitfor "Bringing World" 2 120 || { log "no freeplay"; exit 1; }
sleep 10; log "pause probes"
for i in 1 2 3; do key 53; sleep 3; key 53; sleep 3; done
log "drive"; $C $S key 13 down; sleep 10
G=$(ps -axo pid=,rss=,command= | grep "[f]exdarwin-run" | sort -k2 -n -r | head -1 | awk '{print $1}')
R=$(ps -axo pid=,rss=,comm= | grep "[v]irgl_render_server" | sort -k2 -n -r | head -1 | awk '{print $1}')
sample "$G" 10 -file "$SMP/game-freeplay-$TAG.txt" >/dev/null 2>&1 &
sample "$R" 10 -file "$SMP/renderer-freeplay-$TAG.txt" >/dev/null 2>&1 &
wait; "$EXP/run/cap-game.sh" "$CAP-drive" >/dev/null; sleep 38; $C $S key 13 up; log "drive end"; sleep 3
key 53; sleep 3; click 643 452; sleep 3; click 570 437
waitfor "Bringing World" 3 120; sleep 8; "$EXP/run/cap-game.sh" "$CAP-menu2" >/dev/null
click 140 570; sleep 3; "$EXP/run/cap-game.sh" "$CAP-quit" >/dev/null; click 570 437; log "quit requested"
