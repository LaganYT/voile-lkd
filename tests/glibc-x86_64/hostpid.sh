#!/bin/bash
# Builds and runs w25_hostpid in the engine, and checks each host pid it reports against macOS before letting it go
# on: the pid is a live fexdarwin-run process, threads and exec keep their process's, fork and posix_spawn get a new
# one whose macOS parent is the guest parent's host process. Also checks guest/steam-hostpid.so: getpid() from a
# library named steamclient.so gives the host pid, from anywhere else the guest pid.
# Usage: ROOT=<x86-64 Debian root> hostpid.sh
# env: ZIG (zig binary, else zig on PATH), RUN (fexdarwin-run, default this tree's build), KEEP=1 keeps the work folder.
set -uo pipefail
W="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$W/../.." && pwd)"
ZIG="${ZIG:-$(command -v zig || true)}"
ROOT="${ROOT:?ROOT=<x86-64 Debian root>}"
RUN="${RUN:-$REPO/fexdarwin/build/host/fexdarwin-run}"
TO="$REPO/fexdarwin/scripts/run-with-timeout.sh"
[ -x "$ZIG" ] || { echo "zig not found (set ZIG)" >&2; exit 2; }
work="$(mktemp -d "${TMPDIR:-/tmp}/w25.XXXXXX")"
mkdir "$work/bin" "$work/ack"
ZIG="$ZIG" "$REPO/guest/build.sh" || exit 2
cp "$REPO/guest/bin/steam-hostpid.so" "$work/bin/"
"$ZIG" cc -target x86_64-linux-gnu.2.36 -O1 -pthread -o "$work/bin/w25_hostpid" "$W/src/w25_hostpid.c" || exit 2
"$ZIG" cc -target x86_64-linux-gnu.2.36 -O1 -shared -fPIC -o "$work/bin/steamclient.so" "$W/src/w25_steamclient.c" ||
    exit 2

mkfifo "$work/out"
FEXDARWIN_LKD=1 FEXDARWIN_ROOT="$ROOT" FEXDARWIN_ROOT_RO=1 FEXDARWIN_BIND="/w=$work/bin:ro;/ack=$work/ack:ro" \
    "$TO" 60 "$RUN" --env W25_ACK=1 /w/w25_hostpid /w > "$work/out" 2> "$work/stderr.txt" &
runner=$!

fail=0 pass=0 seen=""
main="" fork=""
bad() { echo "FAIL $*"; fail=1; }
while IFS= read -r line; do
    echo "$line"
    case "$line" in
    "W25 PASS") pass=1; continue ;;
    "W25 FAIL"*) fail=1; continue ;;
    "W25 "*) ;;
    *) continue ;;
    esac
    set -- $line
    stepname=$2 host=${4#host=}
    comm="$(ps -o comm= -p "$host" 2>/dev/null)"
    ppid="$(ps -o ppid= -p "$host" 2>/dev/null | tr -d ' ')"
    echo "    macOS: pid $host is ${comm:-(no process)}, parent $ppid"
    case "$comm" in */fexdarwin-run) ;; *) bad "$stepname: $host is not a live fexdarwin-run" ;; esac
    case "$stepname" in
    main) main=$host
        case "$(ps -o comm= -p "$ppid" 2>/dev/null)" in *perl*) ;; *) bad "main: parent is not run-with-timeout" ;; esac ;;
    thread) [ "$host" = "$main" ] || bad "thread: $host, process $main" ;;
    fork) fork=$host; [ "$ppid" = "$main" ] && [ "$host" != "$main" ] || bad "fork: $host parent $ppid, main $main" ;;
    exec|shim|shim-thread) [ "$host" = "$fork" ] || bad "$stepname: $host, before the exec $fork" ;;
    shim-fork) [ "$ppid" = "$fork" ] && [ "$host" != "$fork" ] || bad "shim-fork: $host parent $ppid, exec $fork" ;;
    spawn) [ "$ppid" = "$main" ] && [ "$host" != "$main" ] || bad "spawn: $host parent $ppid, main $main" ;;
    *) bad "unknown step $stepname" ;;
    esac
    seen="$seen $stepname"
    touch "$work/ack/$host-$stepname"
done < "$work/out"
wait "$runner"
rc=$?
[ "$rc" = 0 ] || bad "exit status $rc (stderr: $work/stderr.txt)"
[ "$pass" = 1 ] || bad "no W25 PASS"
for s in main thread fork exec shim shim-thread shim-fork spawn; do
    case " $seen " in *" $s "*) ;; *) bad "step $s missing" ;; esac
done
if [ "$fail" = 0 ]; then
    echo "hostpid: PASS"
    [ -n "${KEEP:-}" ] || rm -rf "$work"
else
    echo "hostpid: FAIL (work folder $work)"
fi
exit "$fail"
