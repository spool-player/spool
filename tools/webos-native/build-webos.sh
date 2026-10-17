#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
usage() {
  echo "Usage: $0 [all|fetch|qt-host|qt-target|dependencies|app|stage|package]"
}
if (( $# > 1 )); then
  usage >&2
  exit 2
fi
PHASE="${1:-all}"
case "$PHASE" in
  -h|--help) usage; exit 0 ;;
  all|fetch|qt-host|qt-target|dependencies|app|stage|package) ;;
  *) usage >&2; exit 2 ;;
esac
TOOLS_ROOT="${WEBOS_TOOLS_ROOT:-$ROOT/tools/webos-native}"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"


fetch_toolchain_ffmpeg() {
  local version url sha
  version="$(toolchain_field "$ROOT" ffmpeg.version)"
  url="$(toolchain_field "$ROOT" ffmpeg.url)"
  sha="$(toolchain_field "$ROOT" ffmpeg.sha256)"
  download_verified "$url" "$sha" "$ROOT/build/downloads/ffmpeg-$version.tar.xz"
}

run_phase() {
  local phase="$1"
  case "$phase" in
    fetch)
      "$TOOLS_ROOT/build-qt6-611.sh" fetch
      "$TOOLS_ROOT/build-third-party.sh" fetch
      "$TOOLS_ROOT/build-curl.sh" fetch
      "$TOOLS_ROOT/build-qcoro.sh" fetch
      fetch_toolchain_ffmpeg
      ;;
    qt-host)
      "$TOOLS_ROOT/build-qt6-611.sh" host
      ;;
    qt-target)
      WEBOS_SDK_ROOT="$SDK_ROOT" "$TOOLS_ROOT/restore-toolchain.sh"
      QT_STATIC="${QT_STATIC:-1}" "$TOOLS_ROOT/build-qt6-611.sh" target
      ;;
    dependencies)
      WEBOS_SDK_ROOT="$SDK_ROOT" "$TOOLS_ROOT/restore-toolchain.sh"
      "$TOOLS_ROOT/build-third-party.sh" build
      "$TOOLS_ROOT/build-curl.sh" build
      QT_STATIC="${QT_STATIC:-1}" "$TOOLS_ROOT/build-qcoro.sh" build
      "$TOOLS_ROOT/build-ffmpeg.sh"
      ;;
    app|stage|package)
      "$ROOT/build-ipk.sh" "$phase"
      ;;
    *)
      echo "unknown webOS build phase: $phase" >&2
      exit 2
      ;;
  esac
}

if [[ "$PHASE" == "all" ]]; then
  for phase in fetch qt-host qt-target dependencies app stage package; do
    echo
    echo ">>> webOS phase: $phase"
    run_phase "$phase"
  done
else
  run_phase "$PHASE"
fi
