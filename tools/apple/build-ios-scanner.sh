#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/lib/manifest-sources.sh"
[[ "$(uname -s)" == Darwin ]] || { echo 'iOS host tools require macOS and Xcode.' >&2; exit 1; }
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the pinned macOS Qt kit}"
version="$(toolchain_field "$ROOT" qt.version)"
[[ "$("$QT_HOST_PATH/bin/qtpaths" --query QT_VERSION)" == "$version" ]] || {
  echo "error: QT_HOST_PATH must supply Qt $version host tools" >&2; exit 1;
}
base_url="$(toolchain_qt_base_url "$ROOT" sourceArchive "$version")"
sha="$(toolchain_field "$ROOT" qt.sources.qtdeclarative)"
series="${version%.*}"
source_root="$ROOT/build/apple/qt-sources/$version/qtdeclarative"
archive="$ROOT/build/apple/downloads/qtdeclarative-everywhere-src-$version.tar.xz"
download_verified "$base_url/qtdeclarative-everywhere-src-$version.tar.xz" "$sha" "$archive"
extract_verified_source "$archive" "$sha" "$source_root"
# Use the same pinned scanner and subtree-exclusion fix as the tvOS build.
patch_file="$ROOT/tools/webos-native/patches/qtdeclarative-$series-qmlimportscanner-exclude-subtrees.patch"
if gpatch --dry-run --forward --silent -d "$source_root" -p1 <"$patch_file" >/dev/null 2>&1; then
  gpatch --forward -d "$source_root" -p1 <"$patch_file"
elif ! gpatch --dry-run --reverse --silent -d "$source_root" -p1 <"$patch_file" >/dev/null 2>&1; then
  echo 'error: pinned host scanner patch no longer applies' >&2; exit 1
fi
"$QT_HOST_PATH/bin/qt-cmake" -S "$ROOT/tools/apple/qmlimportscanner" \
  -B "$ROOT/build/apple/qmlimportscanner" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSPOOL_QTDECLARATIVE_SOURCE="$source_root"
cmake --build "$ROOT/build/apple/qmlimportscanner" --parallel "${BUILD_JOBS:-4}"
