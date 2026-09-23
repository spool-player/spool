#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
ensure_native_shell "$APP_ROOT" "$APP_ROOT/tools/package-macos-dmg.sh" "$@"
APP_VERSION="$(read_project_version "$APP_ROOT")"
BUILD_ROOT="${BUILD_ROOT:-$APP_ROOT/build/macos}"
APP_BUNDLE="${APP_BUNDLE:-$BUILD_ROOT/install/Spool.app}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$APP_ROOT/dist}"

if [[ ! -d "$APP_BUNDLE" ]]; then
  echo "error: app bundle not found at $APP_BUNDLE" >&2
  exit 1
fi

# Apple Silicon and Intel DMGs ship side by side in one release, so name each
# for the architecture its binary actually carries rather than for the host
# that happened to build it.
APP_BINARY="$APP_BUNDLE/Contents/MacOS/Spool"
if [[ -z "${APP_ARCH:-}" ]]; then
  APP_ARCH="$(lipo -archs "$APP_BINARY" 2>/dev/null | tr ' ' '-')"
fi
if [[ -z "$APP_ARCH" ]]; then
  echo "error: could not determine the architecture of $APP_BINARY" >&2
  exit 1
fi
DMG_PATH="$ARTIFACT_DIR/Spool-${APP_VERSION}-macOS-${APP_ARCH}.dmg"

mkdir -p "$ARTIFACT_DIR"
rm -f "$DMG_PATH"

command -v create-dmg >/dev/null 2>&1 || {
  echo "error: create-dmg is required for macOS release packaging" >&2
  exit 1
}
create_dmg_args=(
  --volname "Spool"
  --window-pos 200 120
  --window-size 640 420
  --icon-size 96
  --app-drop-link 460 185
  --format "${DMG_FORMAT:-ULMO}"
)
if [[ "${CI:-}" == true ]]; then
  # Finder can retain the temporary image until hdiutil's two-minute detach
  # timeout expires on hosted runners. CI needs a reliable image, not Finder
  # window metadata.
  create_dmg_args+=(--skip-jenkins)
fi
# create-dmg reports failure when Finder still holds the scratch image at
# detach time -- "couldn't unmount: Resource busy" -- which happens after the
# image itself is finished, and ejects a moment later anyway. --skip-jenkins
# above narrows the window without closing it. Rather than trust the exit code
# either way, ask whether there is a valid image: a detach race then costs a
# warning, and a real packaging failure still fails.
set +e
create-dmg "${create_dmg_args[@]}" "$DMG_PATH" "$APP_BUNDLE"
create_dmg_status=$?
set -e
if (( create_dmg_status != 0 )); then
  if [[ -f "$DMG_PATH" ]] && hdiutil verify "$DMG_PATH" >/dev/null 2>&1; then
    printf 'warning: create-dmg exited %d but the image verifies; continuing\n' \
      "$create_dmg_status" >&2
  else
    printf 'error: create-dmg failed with %d and produced no valid image\n' \
      "$create_dmg_status" >&2
    exit "$create_dmg_status"
  fi
fi

printf '%s\n' "$DMG_PATH"
