#!/usr/bin/env bash
# The toolchain versions, read from the one manifest that sets them.
#
#   tools/toolchain-versions.sh            # KEY=value lines, for $GITHUB_OUTPUT
#   tools/toolchain-versions.sh qt.version # one field
#   tools/toolchain-versions.sh --check    # the manifests agree with the pin
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"

check_versions() {
  local qt_version qt_series status=0
  qt_version="$(toolchain_field "$ROOT" qt.version)"
  qt_series="$(toolchain_field "$ROOT" qt.series)"

  local manifest declared
  for manifest in qt-webos-6.11 qt-android-6.11; do
    declared="$(manifest_qt_field "$ROOT/tools/manifests/$manifest.json" qtVersion)"
    [[ "$declared" == "$qt_version" ]] || {
      printf '%s.json pins Qt %s, toolchain.json pins %s\n' "$manifest" "$declared" "$qt_version" >&2
      status=1
    }
    declared="$(manifest_qt_field "$ROOT/tools/manifests/$manifest.json" series)"
    [[ "$declared" == "$qt_series" ]] || {
      printf '%s.json is series %s, toolchain.json is %s\n' "$manifest" "$declared" "$qt_series" >&2
      status=1
    }
  done

  declared="$(manifest_qt_field "$ROOT/tools/manifests/qt-windows-6.11.json" version)"
  [[ "$declared" == "$qt_version" ]] || {
    printf 'qt-windows-6.11.json pins Qt %s, toolchain.json pins %s\n' "$declared" "$qt_version" >&2
    status=1
  }

  # FFmpeg is fetched from the pin everywhere it is built from source, so the
  # per-platform manifests must not carry one of their own any more.
  for manifest in webos-third-party android-third-party; do
    if grep -q '"ffmpeg"' "$ROOT/tools/manifests/$manifest.json"; then
      printf '%s.json still pins FFmpeg; toolchain.json owns it\n' "$manifest" >&2
      status=1
    fi
  done

  if [[ $status -eq 0 ]]; then
    printf 'toolchain: Qt %s, FFmpeg %s\n' \
      "$qt_version" "$(toolchain_field "$ROOT" ffmpeg.version)"
  fi
  return "$status"
}

case "${1:-}" in
  --check)
    check_versions
    ;;
  "")
    printf 'qt_version=%s\n' "$(toolchain_field "$ROOT" qt.version)"
    printf 'qt_series=%s\n' "$(toolchain_field "$ROOT" qt.series)"
    printf 'qt_windows_kit=%s\n' "$(toolchain_field "$ROOT" qt.windowsKit)"
    printf 'ffmpeg_version=%s\n' "$(toolchain_field "$ROOT" ffmpeg.version)"
    printf 'qcoro_revision=%s\n' "$(toolchain_field "$ROOT" qcoro.revision)"
    printf 'qcoro_windows_prefix=%s\n' "$(toolchain_field "$ROOT" qcoro.windowsPrefix)"
    ;;
  *)
    toolchain_field "$ROOT" "$1"
    ;;
esac
