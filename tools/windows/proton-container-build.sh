#!/usr/bin/env bash
set -euo pipefail
export WINEPREFIX=/spool-tools/build-wine-prefix WINEDEBUG=-all
export WINEDLLOVERRIDES='mshtml=d;mscoree=b'
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
if [[ ! -f "$WINEPREFIX/system.reg" ]]; then
    wineboot --init
    wineserver --wait
fi
ln -sfn /spool-tools "$WINEPREFIX/dosdevices/t:"
# PowerShell initializes System.Console even in noninteractive mode. A real
# Windows console is required; Xvfb confines it to the build container.
xvfb-run -a wine start /wait 'T:\powershell-7.4.13\pwsh.exe' \
    -NoLogo -NoProfile -NonInteractive -File "$1" -ToolsRoot 'T:\'
wineserver --wait
