#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
APP_VERSION="$(read_project_version "$APP_ROOT")"

if (( $# < 2 || $# > 3 )); then
  echo 'usage: package-tvos.sh APP_BUNDLE OUTPUT_DIR [VERSION]' >&2
  exit 1
fi
APP_BUNDLE="$1"
ARTIFACT_DIR="$2"
if [[ "${3:-$APP_VERSION}" != "$APP_VERSION" ]]; then
  printf 'error: requested version %s does not match VERSION %s\n' "$3" "$APP_VERSION" >&2
  exit 1
fi
if [[ ! -d "$APP_BUNDLE" || -L "$APP_BUNDLE" || "$(basename "$APP_BUNDLE")" != Spool.app ]]; then
  echo 'error: supply the real device Spool.app bundle, not a simulator or placeholder' >&2
  exit 1
fi
APP_BUNDLE="$(cd "$APP_BUNDLE" && pwd -P)"
for tool in python3 zip; do
  command -v "$tool" >/dev/null || { echo "error: missing $tool" >&2; exit 1; }
done

ARTIFACT_DIR="$(python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$ARTIFACT_DIR")"
if [[ "$ARTIFACT_DIR" == "$APP_BUNDLE" || "$ARTIFACT_DIR" == "$APP_BUNDLE/"* ]]; then
  echo 'error: output directory must be outside Spool.app' >&2
  exit 1
fi
mkdir -p "$ARTIFACT_DIR"
# Stage on the destination filesystem: a failed validation never publishes an
# IPA, and the final rename cannot expose a partially written release asset.
stage="$(mktemp -d "$ARTIFACT_DIR/.spool-tvos-XXXXXX")"
trap 'rm -rf "$stage"' EXIT
mkdir "$stage/Payload"
cp -R "$APP_BUNDLE" "$stage/Payload/Spool.app"
archive="$stage/Spool-${APP_VERSION}-tvOS-arm64.ipa"
(
  cd "$stage"
  zip -q -r -y "$archive" Payload
)
# The same canonical archive gate is used by the release scanner. It checks
# bundle versions, actual Mach-O device ARM64/platform, and safe archive paths.
python3 "$APP_ROOT/tools/package-audit.py" tvos-ipa "$archive" --version "$APP_VERSION"
mv -f "$archive" "$ARTIFACT_DIR/Spool-${APP_VERSION}-tvOS-arm64.ipa"
printf '%s\n' "$ARTIFACT_DIR/Spool-${APP_VERSION}-tvOS-arm64.ipa"
printf '%s\n' 'tvOS IPA requires your Apple signing identity and provisioning profile before installation; no signing is performed by this packager.' >&2
