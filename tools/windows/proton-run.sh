#!/usr/bin/env bash
set -euo pipefail

root=$(git rev-parse --show-toplevel)
state=${SPOOL_WINDOWS_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/spool/windows-proton}
stage=${SPOOL_WINDOWS_STAGE:-$root/build/windows-release/stage}
if [[ ! -f "$stage/spool.exe" ]]; then
    printf 'Windows staged payload missing: %s\nBuild it with: nix run .#windows-proton-build\n' "$stage/spool.exe" >&2
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
# SPOOL_PROTON_HDR=0 takes the layer, Proton's HDR and DXVK's HDR out
# together. An SDR run gets an R8G8B8A8_UNORM SRGB_NONLINEAR swapchain and
# correct colour, which is what says the purple cast belongs to this path. It
# does not recover the frame rate: SDR measured slower than HDR, with the GPU
# idle either way, so whatever blocks presentation is somewhere else.
if [[ ${SPOOL_PROTON_HDR:-1} == 0 ]]; then
    unset ENABLE_HDR_WSI PROTON_ENABLE_HDR DXVK_HDR
else
    export ENABLE_HDR_WSI=1 PROTON_ENABLE_HDR=1
    # DXVK negotiates EXTENDED_SRGB_LINEAR inside Wine and its presenter says
    # so, but the layer below still creates the surface PASS_THROUGH and calls
    # it untagged: the colour space is not surviving winevulkan, which also
    # reports extSwapchainColorSpace 0. Setting this does not fix that on its
    # own -- it is kept so the Wine side asks for the right thing.
    export DXVK_HDR=${DXVK_HDR:-1}
fi
# Proton's native Wayland backend is the newer WSI path and the one that
# changed under us. Keep it the default, but leave it switchable: exporting
# SPOOL_PROTON_WAYLAND=0 falls back to Xwayland without editing this file.
export PROTON_ENABLE_WAYLAND=${SPOOL_PROTON_WAYLAND:-1}
# Proton's logging channels include +unwind, which disassembles unwind info on
# every exception. It is measured in hundreds of megabytes and it dominates
# runtime, so state the quiet default rather than inheriting whatever is set.
export WINEDEBUG=${WINEDEBUG:--all}
export LD_PRELOAD=''
# D3D11 is the default because it is Qt's own on Windows, but the colour
# space does not survive winevulkan on that path: DXVK negotiates
# EXTENDED_SRGB_LINEAR and the layer below still gets PASS_THROUGH.
# SPOOL_RENDER_API=vulkan puts Qt and mpv on winevulkan directly, with no
# D3D11 translation in between for the colour space to be lost in.
export SPOOL_RENDER_API=${SPOOL_RENDER_API:-d3d11}
unset PROTON_USE_NTSYNC PROTON_NO_NTSYNC PROTON_NO_FSYNC PROTON_NO_ESYNC
if [[ ! -r /dev/ntsync || ! -w /dev/ntsync ]]; then
    export PROTON_NO_NTSYNC=1
fi
mkdir -p "$state/logs"
export DXVK_LOG_PATH="$state/logs"
# Keep DXVK at info: it states the swapchain format, colour space and
# present mode on every recreate, which is the only record of what the
# HDR path actually negotiated. It is a few kilobytes a run.
export DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info}
if [[ -d "$state/playback-profile" ]]; then
    profile="Z:${state//\//\\}\\playback-profile"
    export SPOOL_CREDENTIAL_STORE_DIR="$profile\\credentials"
fi
cd "$stage"
exec umu-run "$stage/spool.exe" "$@"
