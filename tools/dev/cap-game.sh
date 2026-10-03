#!/bin/bash
# Captures the game window (height >= 200, width < 2000 px) of the wsd-eac server by window id. $1 = output file base
EXP="$(cd "$(dirname "$0")/.." && pwd)"
for W in $(pgrep -x wsd); do
  ps -o args= -p $W | grep -q "wsd-eac" || continue
  for id in $("$EXP/src/wine-linux/tools/winid/winid" $W); do
    f="$1-win$id.png"; screencapture -x -o -l$id "$f" 2>/dev/null
    h=$(sips -g pixelHeight "$f" 2>/dev/null | tail -1 | awk '{print $2}'); w=$(sips -g pixelWidth "$f" 2>/dev/null | tail -1 | awk '{print $2}')
    { [ "${h:-0}" -ge 200 ] && [ "${w:-0}" -lt 2000 ]; } && echo "$f" || rm -f "$f"
  done
done
