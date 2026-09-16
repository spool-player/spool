#!/usr/bin/env bash
set -euo pipefail

# Repackage the AppDir that tools/package-appimage.sh already built and audited
# as a plain tarball. Same payload as the AppImage, without the runtime: a
# distro package extracts this once, so the app then starts from ordinary files
# instead of mounting a compressed image on every launch. Run it after
# tools/package-appimage.sh, which leaves the AppDir in place.

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
ensure_native_shell "$APP_ROOT" "$APP_ROOT/tools/package-linux-bundle.sh" "$@"

APP_VERSION="$(read_project_version "$APP_ROOT")"
APPDIR="${APPDIR:-$APP_ROOT/build/appimage/AppDir}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$APP_ROOT/dist}"
BUNDLE="Spool-for-Jellyfin-${APP_VERSION}-linux-x86_64"
OUTPUT="$ARTIFACT_DIR/$BUNDLE.tar.zst"

if [[ ! -x "$APPDIR/AppRun" ]]; then
  echo "error: no packaged AppDir at $APPDIR; run tools/package-appimage.sh first" >&2
  exit 1
fi

mkdir -p "$ARTIFACT_DIR"
stage="$(mktemp -d)"
trap 'chmod -R u+w "$stage" 2>/dev/null || true; rm -rf "$stage"' EXIT
cp -a "$APPDIR" "$stage/$BUNDLE"

# zstd rather than xz: the download is a little larger, but makepkg and pacman
# spend a fraction of the time unpacking it, which is the whole point here.
rm -f "$OUTPUT" "$OUTPUT.tmp"
tar \
  --sort=name \
  --mtime="@${SOURCE_DATE_EPOCH:-0}" \
  --owner=0 --group=0 --numeric-owner \
  --use-compress-program='zstd -T0 -15' \
  -cf "$OUTPUT.tmp" -C "$stage" "$BUNDLE"
mv "$OUTPUT.tmp" "$OUTPUT"
printf '%s\n' "$OUTPUT"
