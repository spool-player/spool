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
result="$SPOOL_WINDOWS_HOME/logs/build-result.txt"
rm -f "$result"
# Z: exposes the host filesystem in the dedicated prefix. Keep each argument
# separate: paths may contain spaces, and PowerShell receives Windows paths.
windows_root="Z:${root//\//\\}"
cd "$root"
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
