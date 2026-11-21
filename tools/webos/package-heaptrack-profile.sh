#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
benchmark_only=0
case "${1:-}" in
  --benchmark-only) benchmark_only=1 ;;
  -h|--help)
    echo "Usage: $0 [--benchmark-only]"
    echo 'Builds a complete fresh IPK, then packages a separate instrumented copy.'
    echo 'No device is contacted; install the resulting IPK explicitly.'
    exit 0 ;;
  '') ;;
  *) echo "unknown option: $1" >&2; exit 2 ;;
esac
(( $# <= 1 )) || { echo 'unexpected extra arguments' >&2; exit 2; }
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
HEAPTRACK_INSTALL="${HEAPTRACK_INSTALL:-$ROOT/build/webos-heaptrack/install}"
SOURCE_STAGE="${WEBOS_STAGE_DIR:-$ROOT/build/webos/stage/app}"
INSTALL_DIR="${WEBOS_APP_BUILD_DIR:-$ROOT/build/webos/app}/install"
PROFILE_OUTPUT="${WEBOS_PROFILE_OUTPUT_DIR:-$ROOT/build/webos-profile}"
PRELOAD="$HEAPTRACK_INSTALL/lib/heaptrack/libheaptrack_preload.so"
HEAPTRACK_BIN="$HEAPTRACK_INSTALL/bin/heaptrack"
HEAPTRACK_ENV="$HEAPTRACK_INSTALL/lib/heaptrack/libexec/heaptrack_env"

if (( ! benchmark_only )); then
  for required in "$PRELOAD" "$HEAPTRACK_BIN" "$HEAPTRACK_ENV"; do
    [[ -f "$required" ]] || { printf 'missing heaptrack payload: %s\n' "$required" >&2; exit 1; }
  done
  export HEAPTRACK_UNWIND_FLAGS='-fasynchronous-unwind-tables -funwind-tables -fno-omit-frame-pointer -g'
fi
"$ROOT/build-ipk.sh"
[[ -x "$SOURCE_STAGE/bin/spool" ]] || { echo 'canonical webOS stage is missing' >&2; exit 1; }
mkdir -p "$PROFILE_OUTPUT"
APP_DIR="$(mktemp -d "$PROFILE_OUTPUT/stage.XXXXXX")"
trap 'rm -rf "$APP_DIR"' EXIT
cp -a "$SOURCE_STAGE/." "$APP_DIR/"
mv "$APP_DIR/bin/spool" "$APP_DIR/bin/spool.real"
install -m 0755 "$ROOT/tools/webos/heaptrack-launch-shim.sh" "$APP_DIR/bin/spool"
printf '1\n' >"$APP_DIR/bin/.spool-benchmark-v1"
if (( ! benchmark_only )); then
  mkdir -p "$APP_DIR/lib/heaptrack/libexec"
  install -m 0755 "$PRELOAD" "$APP_DIR/lib/heaptrack/libheaptrack_preload.so"
  install -m 0755 "$HEAPTRACK_BIN" "$APP_DIR/bin/heaptrack"
  install -m 0755 "$HEAPTRACK_ENV" "$APP_DIR/lib/heaptrack/libexec/heaptrack_env"
  patchelf --force-rpath --set-rpath '$ORIGIN/..' "$APP_DIR/lib/heaptrack/libheaptrack_preload.so"
  cp -f "$INSTALL_DIR/bin/spool" "$ROOT/build/spool.unstripped"
  "$SDK_ROOT/bin/arm-webos-linux-gnueabi-strip" --strip-unneeded \
    "$APP_DIR/lib/heaptrack/libheaptrack_preload.so" "$APP_DIR/bin/heaptrack" \
    "$APP_DIR/lib/heaptrack/libexec/heaptrack_env"
fi
app_id="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$APP_DIR/appinfo.json")"
rm -f "$PROFILE_OUTPUT/${app_id}_"*.ipk
npx -y -p @webos-tools/cli@3.2.3 ares-package "$APP_DIR" --outdir "$PROFILE_OUTPUT"
printf '%s\n' "$PROFILE_OUTPUT/${app_id}_"*.ipk
