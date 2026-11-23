#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
QT_MANIFEST="${QT_MANIFEST:-$ROOT/tools/manifests/qt-webos.json}"
SDK_PARENT="${WEBOS_SDK_PARENT:-$ROOT/build/webos-sdk}"

manifest_toolchain_field() {
  local field="$1"
  python3 -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(data["toolchain"][sys.argv[2]])
' "$QT_MANIFEST" "$field"
}

sdk_layout="$(manifest_toolchain_field sdkLayout)"
triplet="$(manifest_toolchain_field triplet)"
default_url="$(manifest_toolchain_field archiveUrl)"
default_sha="$(manifest_toolchain_field archiveSha256)"
url="${WEBOS_TOOLCHAIN_ARCHIVE_URL:-$default_url}"
sha="${WEBOS_TOOLCHAIN_ARCHIVE_SHA256:-$default_sha}"
sdk_root="${WEBOS_SDK_ROOT:-$SDK_PARENT/$sdk_layout}"
gcc="$sdk_root/bin/$triplet-gcc"

if [[ -x "$gcc" ]]; then
  printf 'webOS toolchain already restored: %s\n' "$sdk_root"
  exit 0
fi

archive="$ROOT/build/downloads/$(basename "$url")"
download_verified "$url" "$sha" "$archive"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
tar -xzf "$archive" -C "$tmp"

rm -rf "$sdk_root"
mkdir -p "$SDK_PARENT"
if [[ -d "$tmp/$sdk_layout" ]]; then
  mv "$tmp/$sdk_layout" "$sdk_root"
elif [[ -d "$tmp/build/webos-sdk/$sdk_layout" ]]; then
  mv "$tmp/build/webos-sdk/$sdk_layout" "$sdk_root"
elif [[ -d "$tmp/webos-sdk/$sdk_layout" ]]; then
  mv "$tmp/webos-sdk/$sdk_layout" "$sdk_root"
else
  echo "error: archive did not contain $sdk_layout" >&2
  find "$tmp" -maxdepth 3 -type d | sort >&2
  exit 1
fi

if [[ -x "$sdk_root/relocate-sdk.sh" ]]; then
  (cd "$sdk_root" && ./relocate-sdk.sh)
fi

test -x "$gcc"
test -d "$sdk_root/$triplet/sysroot"
printf 'restored webOS toolchain: %s\n' "$sdk_root"
