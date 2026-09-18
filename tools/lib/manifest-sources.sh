#!/usr/bin/env bash

# Windows ships the interpreter as python only; every other platform here has
# python3. Resolve it once rather than assuming, and let the caller override.
MANIFEST_PYTHON="${MANIFEST_PYTHON:-$(command -v python3 || command -v python || true)}"
if [ -z "$MANIFEST_PYTHON" ]; then
  echo "error: python3 is required to read the build manifests" >&2
  exit 1
fi

manifest_source_field() {
  local manifest="$1"
  local source_name="$2"
  local field="$3"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
value = data["sources"][sys.argv[2]][sys.argv[3]]
print(value)
' "$manifest" "$source_name" "$field"
}

manifest_qt_field() {
  local manifest="$1"
  local field="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(data[sys.argv[2]])
' "$manifest" "$field"
}

manifest_qt_module_sha256() {
  local manifest="$1"
  local module="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for module in data["modules"]:
    if module["name"] == sys.argv[2]:
        print(module["sha256"])
        break
else:
    raise SystemExit(f"module not found in manifest: {sys.argv[2]}")
' "$manifest" "$module"
}

manifest_tool_field() {
  local manifest="$1"
  local tool_name="$2"
  local field="$3"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
print(data["tools"][sys.argv[2]][sys.argv[3]])
' "$manifest" "$tool_name" "$field"
}

manifest_json_array() {
  local manifest="$1"
  local key="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for value in data[sys.argv[2]]:
    print(value)
' "$manifest" "$key"
}

manifest_source_patches() {
  local manifest="$1"
  local source_name="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
for patch in data["sources"][sys.argv[2]].get("patches", []):
    print(patch)
' "$manifest" "$source_name"
}

manifest_vendored_sources() {
  local manifest="$1"
  local source_name="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
sources = data["sources"][sys.argv[2]].get("vendoredSources", {})
for path, source in sources.items():
    print(path, source["url"], source["sha256"], sep="\t")
' "$manifest" "$source_name"
}

verify_sha256() {
  local file="$1"
  local expected="$2"
  local actual
  actual="$(sha256sum "$file" | cut -d' ' -f1)"
  if [[ "$actual" != "$expected" ]]; then
    echo "error: checksum mismatch for $file" >&2
    echo "  expected: $expected" >&2
    echo "  actual:   $actual" >&2
    return 1
  fi
}

download_verified() {
  local url="$1"
  local expected="$2"
  local destination="$3"

  mkdir -p "$(dirname "$destination")"
  if [[ -f "$destination" ]] \
      && verify_sha256 "$destination" "$expected" >/dev/null 2>&1; then
    return 0
  fi

  rm -f "$destination"
  curl -L --fail --retry 3 --retry-all-errors --retry-delay 2 -o "$destination" "$url"
  verify_sha256 "$destination" "$expected"
}

extract_verified_source() {
  local archive="$1"
  local expected="$2"
  local destination="$3"
  local marker="$destination/.jellyfin-source-sha256"

  if [[ -f "$marker" ]] && [[ "$(<"$marker")" == "$expected" ]]; then
    return 0
  fi

  rm -rf "$destination"
  mkdir -p "$destination"
  tar -xf "$archive" -C "$destination" --strip-components=1
  printf '%s\n' "$expected" >"$marker"
}

prepare_manifest_source() {
  local root="$1"
  local manifest="$2"
  local source_name="$3"
  local destination="$4"
  local url sha archive patch

  url="$(manifest_source_field "$manifest" "$source_name" url)"
  sha="$(manifest_source_field "$manifest" "$source_name" sha256)"
  archive="$root/build/downloads/$source_name-$sha.archive"

  download_verified "$url" "$sha" "$archive"
  extract_verified_source "$archive" "$sha" "$destination"

  while IFS= read -r patch; do
    [[ -n "$patch" ]] || continue
    if ! patch -d "$destination" -p1 --dry-run --reverse --silent \
        <"$root/$patch" >/dev/null 2>&1; then
      patch -d "$destination" -p1 <"$root/$patch"
    fi
  done < <(manifest_source_patches "$manifest" "$source_name")
}

# The toolchain manifest is the single source of truth for the Qt and FFmpeg
# versions every platform builds against. Read a dotted path out of it, e.g.
# `toolchain_field "$ROOT" ffmpeg.url`.
toolchain_field() {
  local root="$1"
  local path="$2"
  "$MANIFEST_PYTHON" -c '
import json
import sys

with open(sys.argv[1] + "/tools/manifests/toolchain.json", encoding="utf-8") as handle:
    value = json.load(handle)
for key in sys.argv[2].split("."):
    value = value[key]
print(value)
' "$root" "$path"
}

# FFmpeg is pinned centrally rather than repeated per platform, so the cross
# builds fetch it straight from the toolchain manifest.
prepare_toolchain_ffmpeg() {
  local root="$1"
  local destination="$2"
  local url sha archive

  url="$(toolchain_field "$root" ffmpeg.url)"
  sha="$(toolchain_field "$root" ffmpeg.sha256)"
  archive="$root/build/downloads/ffmpeg-$sha.archive"

  download_verified "$url" "$sha" "$archive"
  extract_verified_source "$archive" "$sha" "$destination"
}
