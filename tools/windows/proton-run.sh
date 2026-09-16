#!/usr/bin/env bash
set -euo pipefail

root=$(git rev-parse --show-toplevel)
state=${SPOOL_WINDOWS_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/spool/windows-proton}
stage=${SPOOL_WINDOWS_STAGE:-$root/build/windows-release/stage}
if [[ ! -f "$stage/jellyfin-native.exe" ]]; then
    printf 'Windows staged payload missing: %s\nBuild it with: nix run .#windows-proton-build\n' "$stage/jellyfin-native.exe" >&2
    exit 1
fi
proton=${SPOOL_PROTON_PATH:-$state/runtimes/GE-Proton11-6-x86_64}
if [[ ! -x "$proton/proton" ]]; then
    printf 'Pinned GE-Proton runtime missing: %s\nPrepare it with: nix run .#windows-proton-build\n' "$proton" >&2
    exit 1
fi
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH
unset QT_QPA_PLATFORM QT_QUICK_BACKEND QSG_RHI_BACKEND LD_LIBRARY_PATH
unset PROTON_USE_WINED3D PROTON_NO_D3D11 PROTON_NO_D3D10
export WINEPREFIX="$state/prefix" PROTONPATH="$proton" GAMEID=umu-default
export ENABLE_HDR_WSI=1 PROTON_ENABLE_WAYLAND=1 PROTON_ENABLE_HDR=1
export LD_PRELOAD='' SPOOL_RENDER_API=d3d11
unset PROTON_USE_NTSYNC PROTON_NO_NTSYNC PROTON_NO_FSYNC PROTON_NO_ESYNC
if [[ ! -r /dev/ntsync || ! -w /dev/ntsync ]]; then
    export PROTON_NO_NTSYNC=1
fi
mkdir -p "$state/logs"
export DXVK_LOG_PATH="$state/logs"
if [[ -d "$state/playback-profile" ]]; then
    profile="Z:${state//\//\\}\\playback-profile"
    export JELLYFIN_CREDENTIAL_STORE_DIR="$profile\\credentials"
fi
cd "$stage"
exec umu-run "$stage/jellyfin-native.exe" "$@"
