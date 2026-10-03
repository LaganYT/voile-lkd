#!/bin/bash
# Relative-mouse stress for one eac-run.py run (started separately with FEXDARWIN_DUMP_DIR=$2): title, menu, Free Play
# (offline), then 60 s of raw mouse motion at 1000 Hz through wsd's debug input, like a real gaming mouse in relative
# mode. Captures by window id only; a guest thread dump if presents stop. $1 = tag, $2 = dump dir.
EXP="$(cd "$(dirname "$0")/.." && pwd)"; cd "$EXP/run"
TAG=${1:-ms}; D=$2; C=../src/wsd/bin/wsdctl; S=wsd-eac/wsd.sock
L="home-user-own/pfx-eac/drive_c/users/crossover/Documents/My Games/Rocket League/TAGame/Logs/Launch.log"
CAP="$EXP/evidence/wine-linux/play/$TAG"
G() { ls -td "$EXP"/evidence/eac-runs/2026* | head -1; }
log() { echo "$(date +%T) $*"; }
alive() { pgrep -x fexdarwin-run >/dev/null; }  # by process name: -f also matches shells that mention the script
presents() { grep -a "alive:" "$(G)/guest.raw.log" 2>/dev/null | tail -1 | awk '{print $6}'; }
waitfor() { for _ in $(seq 1 "$3"); do alive || exit 1; [ "$(grep -c "$1" "$L" 2>/dev/null)" -ge "$2" ] && return 0; sleep 1; done; return 1; }
key() { alive || exit 1; $C $S tap "$1"; }
click() { alive || exit 1; $C $S click "$1" "$2"; }
for _ in $(seq 1 120); do [ "$(grep -c '' "$L" 2>/dev/null || echo 0)" -lt 50 ] 2>/dev/null && break; sleep 1; done
waitfor "EULA sync response" 1 300 || { log "no menu"; exit 1; }
log "eula"; sleep 8; key 36; sleep 20
click 140 325; sleep 4; click 360 460; sleep 4; click 330 270; sleep 4; click 330 380; sleep 4; click 640 495
log "freeplay requested"
waitfor "Bringing World" 2 120 || { log "no freeplay"; exit 1; }
sleep 12; log "in freeplay, presents $(presents)"; "$EXP/run/cap-game.sh" "$CAP-fp0" >/dev/null
$C $S move 60000 1000 &
for i in $(seq 1 7); do sleep 10; log "mouse +$((i*10))s presents $(presents)"; done
wait
p1=$(presents); sleep 12; p2=$(presents); log "after mouse: presents $p1 -> $p2"
"$EXP/run/cap-game.sh" "$CAP-fp1" >/dev/null
if [ "$p1" = "$p2" ]; then log "FROZEN: dumping guest threads"; touch "$D/dump-request"; sleep 3; fi
log "done"
