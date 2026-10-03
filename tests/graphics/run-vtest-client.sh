#!/bin/sh
# Start the annex-A virgl_test_server (render server, NPT_BACKEND=dxmt) on a private socket,
# run vtest_client against it, stop the server. Every process runs under a timeout.
# Usage: run-vtest-client.sh [dxmt-native dylib] [fence_iters] [blob_iters]
set -u
W=$(cd "$(dirname "$0")" && pwd)
EXP=$(cd "$W/../.." && pwd)
G="$EXP/src/graphics-spike"
TO="$EXP/src/fexdarwin/scripts/run-with-timeout.sh"
DXMT="${1:-$G/third_party/utm-5.0.5/dxmt-native.framework/Versions/A/dxmt-native}"
D=$(mktemp -d "${TMPDIR:-/tmp}/gvt.XXXXXX")   # mktemp -d creates 0700
SOCK="$D/s"
echo "socket=$SOCK len=$(printf %s "$SOCK" | wc -c | tr -d ' ') dir_mode=$(stat -f %Lp "$D")"
echo "dxmt=$DXMT sha256=$(shasum -a 256 "$DXMT" | cut -d' ' -f1)"
env RENDER_SERVER_EXEC_PATH="$G/prefix/libexec/virgl_render_server" NPT_BACKEND=dxmt \
    NPT_D3D11_LIBRARY_PATH="$DXMT" NPT_DXGI_LIBRARY_PATH="$DXMT" NPT_D3D12_LIBRARY_PATH="$DXMT" \
    "$TO" 120 "$G/prefix/bin/virgl_test_server" --no-virgl --no-fork --socket-path "$SOCK" \
    > "$D/server.log" 2>&1 &
SPID=$!
i=0; while [ ! -S "$SOCK" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
"$TO" 90 "$W/bin/vtest_client" "$SOCK" "${2:-200}" "${3:-50}"
RC=$?
sleep 0.3
kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null
echo "## server log (npt lines)"; grep -E "npt|error|fail" "$D/server.log" | cut -c1-400 | tail -40
# The client cannot see whether the worker loaded the backend: require it from the server log.
if grep -q "failed to open NPT_" "$D/server.log"; then echo "BACKEND_LOAD=FAIL"; RC=1; else echo "BACKEND_LOAD=no failure logged"; fi
echo "## leftover worker processes:"; pgrep -fl virgl_render_server || echo none
rm -rf "$D"
echo "client_rc=$RC"
exit $RC
