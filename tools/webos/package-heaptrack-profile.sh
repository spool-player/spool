#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
HEAPTRACK_INSTALL="${HEAPTRACK_INSTALL:-$ROOT/build/webos-heaptrack/install}"
APP_DIR="${WEBOS_STAGE_DIR:-$ROOT/build/webos/stage/app}"
INSTALL_DIR="${WEBOS_APP_BUILD_DIR:-$ROOT/build/webos/app}/install"
PROFILE_OUTPUT="${WEBOS_PROFILE_OUTPUT_DIR:-$ROOT/build/webos-profile}"
PRELOAD="$HEAPTRACK_INSTALL/lib/heaptrack/libheaptrack_preload.so"
HEAPTRACK_BIN="$HEAPTRACK_INSTALL/bin/heaptrack"
HEAPTRACK_ENV="$HEAPTRACK_INSTALL/lib/heaptrack/libexec/heaptrack_env"

for required in "$PRELOAD" "$HEAPTRACK_BIN" "$HEAPTRACK_ENV"; do
  [[ -f "$required" ]] || {
    printf 'error: heaptrack profiling payload is missing: %s\n' "$required" >&2
    exit 1
  }
done

HEAPTRACK_UNWIND_FLAGS="-fasynchronous-unwind-tables -funwind-tables -fno-omit-frame-pointer -g" \
  "$ROOT/build-ipk.sh"

[[ -x "$APP_DIR/bin/spool" ]] || {
  printf 'error: canonical webOS stage is missing: %s\n' "$APP_DIR/bin/spool" >&2
  exit 1
}
mv "$APP_DIR/bin/spool" "$APP_DIR/bin/spool.real"
install -m 0755 "$ROOT/tools/webos/heaptrack-launch-shim.sh" "$APP_DIR/bin/spool"
mkdir -p "$APP_DIR/lib/heaptrack/libexec" "$PROFILE_OUTPUT"
install -m 0755 "$PRELOAD" "$APP_DIR/lib/heaptrack/libheaptrack_preload.so"
install -m 0755 "$HEAPTRACK_BIN" "$APP_DIR/bin/heaptrack"
install -m 0755 "$HEAPTRACK_ENV" "$APP_DIR/lib/heaptrack/libexec/heaptrack_env"
patchelf --force-rpath --set-rpath '$ORIGIN/..' "$APP_DIR/lib/heaptrack/libheaptrack_preload.so"
cp -f "$INSTALL_DIR/bin/spool" "$ROOT/build/spool.unstripped"
"$SDK_ROOT/bin/arm-webos-linux-gnueabi-strip" --strip-unneeded \
  "$APP_DIR/lib/heaptrack/libheaptrack_preload.so" "$APP_DIR/bin/heaptrack" \
  "$APP_DIR/lib/heaptrack/libexec/heaptrack_env"
rm -f "$PROFILE_OUTPUT"/*.ipk
npx -y -p @webos-tools/cli@3.2.3 ares-package "$APP_DIR" --outdir "$PROFILE_OUTPUT"
printf '%s\n' "$PROFILE_OUTPUT"/*.ipk
