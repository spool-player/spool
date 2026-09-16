#!/usr/bin/env bash
set -euo pipefail

root=$(git rev-parse --show-toplevel)
export SPOOL_WINDOWS_HOME=${SPOOL_WINDOWS_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/spool/windows-proton}
mkdir -p "$SPOOL_WINDOWS_HOME/logs"
exec 9>"$SPOOL_WINDOWS_HOME/build.lock"
flock 9
python3 "$root/tools/windows/proton-bootstrap.py"
image=$(jq -r .buildImage "$root/tools/manifests/windows-proton.json")
podman_args=(--root "$SPOOL_WINDOWS_HOME/containers"
    --runroot "${XDG_RUNTIME_DIR:?}/spool-windows-containers")
logs="$SPOOL_WINDOWS_HOME/logs"
result="$logs/build-result.txt"
rm -f "$result"
# Z: exposes the host filesystem in the dedicated prefix. Keep each argument
# separate: paths may contain spaces, and PowerShell receives Windows paths.
windows_root="Z:${root//\//\\}"
cd "$root"
# The build writes to its logs and nothing else: proton-build.ps1 wraps the
# whole run in Start-Transcript, and `wine start /wait` hands the console back
# immediately, so without this the terminal sits silent for the length of an
# FFmpeg and mpv build with no way to tell work from a hang. Follow the logs
# for as long as the container runs. SPOOL_WINDOWS_QUIET=1 restores silence.
: > "$logs/build.log"
: > "$logs/application-build.log"
tail_pid=""
if [[ ${SPOOL_WINDOWS_QUIET:-0} != 1 ]]; then
    tail -n0 -F "$logs/build.log" "$logs/application-build.log" 2>/dev/null &
    tail_pid=$!
fi
# Stop following however this exits, including Ctrl-C: a tail left running
# would keep printing into the next thing the terminal does.
cleanup() { if [[ -n $tail_pid ]]; then kill "$tail_pid" 2>/dev/null || true; fi; }
trap cleanup EXIT INT TERM
podman "${podman_args[@]}" run --rm --network host \
    -v "$SPOOL_WINDOWS_HOME:/spool-tools" \
    -v "$SPOOL_WINDOWS_HOME:$SPOOL_WINDOWS_HOME" \
    -v "$root:$root" \
    "$image" bash "$root/tools/windows/proton-container-build.sh" \
    "$windows_root\\tools\\windows\\proton-build.ps1" || status=$?
if [[ ! -f "$result" ]]; then
    printf 'Windows build did not report completion (launcher status %s). See %s/logs/build.log\n' "${status:-0}" "$SPOOL_WINDOWS_HOME" >&2
    exit 1
fi
if [[ $(tr -d '\r\n' < "$result") != 0 ]]; then
    printf 'Windows build failed. See %s/logs/build.log\n' "$SPOOL_WINDOWS_HOME" >&2
    exit 1
fi
printf 'Windows payload staged. Launch without rebuilding: nix run .#windows-proton-run\n'
