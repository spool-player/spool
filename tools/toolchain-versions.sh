#!/usr/bin/env bash
# The toolchain versions, read from the one manifest that sets them.
#
#   tools/toolchain-versions.sh            # KEY=value lines, for $GITHUB_OUTPUT
#   tools/toolchain-versions.sh qt.version # one field
#   tools/toolchain-versions.sh --check    # validate platform references and locks
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"

check_versions() {
  "$MANIFEST_PYTHON" - "$ROOT" <<'PYTHON'
import json
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
manifests = root / "tools/manifests"


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


toolchain = read(manifests / "toolchain.json")
qt_version = toolchain["qt"]["version"]
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", qt_version):
    raise SystemExit(f"invalid Qt version: {qt_version}")

modules = read(manifests / "qt-android.json")["modules"]
for module in modules:
    sha256 = toolchain["qt"]["sources"].get(module, "")
    if not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise SystemExit(f"Qt module {module} has no valid source hash in toolchain.json")

for platform in ("android", "webos"):
    if "ffmpeg" in read(manifests / f"{platform}-third-party.json")["sources"]:
        raise SystemExit(f"{platform}-third-party.json still pins FFmpeg")

qcoro = toolchain["qcoro"]["revision"]
if read(root / "flake.lock")["nodes"]["qcoro-src"]["locked"]["rev"] != qcoro:
    raise SystemExit("QCoro pin in flake.lock differs from toolchain.json")

print(f'toolchain: Qt {qt_version}, FFmpeg {toolchain["ffmpeg"]["version"]}')
PYTHON
}

case "${1:-}" in
  --check)
    check_versions
    ;;
  "")
    qt_version="$(toolchain_field "$ROOT" qt.version)"
    qt_series="${qt_version%.*}"
    printf 'qt_version=%s\n' "$qt_version"
    printf 'qt_series=%s\n' "$qt_series"
    printf 'qt_build_tag=%s\n' "${qt_series//./}"
    printf 'qt_windows_kit=%s\n' "$(toolchain_field "$ROOT" qt.windowsKit)"
    printf 'ffmpeg_version=%s\n' "$(toolchain_field "$ROOT" ffmpeg.version)"
    printf 'qcoro_revision=%s\n' "$(toolchain_field "$ROOT" qcoro.revision)"
    printf 'qcoro_windows_prefix=%s\n' "$(toolchain_field "$ROOT" qcoro.windowsPrefix)"
    ;;
  qt.series)
    qt_version="$(toolchain_field "$ROOT" qt.version)"
    printf '%s\n' "${qt_version%.*}"
    ;;
  *)
    toolchain_field "$ROOT" "$1"
    ;;
esac
