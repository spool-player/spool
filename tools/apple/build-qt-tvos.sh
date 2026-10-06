#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/lib/manifest-sources.sh"

if [[ "$(uname -s)" != Darwin ]]; then
  echo 'error: tvOS Qt requires macOS with Xcode and the Apple TV SDK; Linux Qt is not a tvOS kit' >&2
  exit 1
fi
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the pinned macOS Qt host-tools prefix}"
APPLE_SDK="${APPLE_SDK:-appletvsimulator}"
APPLE_ARCH="${APPLE_ARCH:-arm64}"
APPLE_DEPLOYMENT_TARGET="${APPLE_DEPLOYMENT_TARGET:-16.0}"
case "$APPLE_SDK:$APPLE_ARCH" in
  appletvos:arm64|appletvsimulator:arm64|appletvsimulator:x86_64) ;;
  *) echo "error: unsupported tvOS SDK/architecture: $APPLE_SDK/$APPLE_ARCH" >&2; exit 1 ;;
esac
xcrun --sdk "$APPLE_SDK" --show-sdk-path >/dev/null
for tool in cmake ninja curl python3; do
  command -v "$tool" >/dev/null || { echo "error: missing $tool" >&2; exit 1; }
done
manifest="$ROOT/tools/manifests/toolchain.json"
version="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["qt"]["version"])' "$manifest")"
base_url="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["qt"]["baseUrl"])' "$manifest")"
if [[ "$("$QT_HOST_PATH/bin/qtpaths" --qt-version)" != "$version" ]]; then
  echo "error: QT_HOST_PATH must supply Qt $version host tools" >&2
  exit 1
fi
source_root="$ROOT/build/apple/qt-sources/$version"
build_root="$ROOT/build/apple/qt-$APPLE_SDK-$APPLE_ARCH"
prefix="${QT_TARGET_PREFIX:-$ROOT/build/apple/$APPLE_SDK-$APPLE_ARCH/qt}"
jobs="${JOBS:-$(sysctl -n hw.ncpu)}"
export PKG_CONFIG_LIBDIR="$build_root/empty-pkgconfig"
unset PKG_CONFIG_PATH CMAKE_PREFIX_PATH CMAKE_INCLUDE_PATH CMAKE_LIBRARY_PATH QT_PLUGIN_PATH QML2_IMPORT_PATH
mkdir -p "$source_root" "$build_root/empty-pkgconfig" "$prefix"

# Qt retains a UIKit tvOS source port, but does not list tvOS as an officially
# supported/tested platform. A successful source build is a prerequisite, not
# evidence of device playback, accessibility, or App Store acceptance.
for module in qtbase qtshadertools qtdeclarative qtsvg qtimageformats qtwebsockets qttools; do
  sha="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["qt"]["sources"][sys.argv[2]])' "$manifest" "$module")"
  archive="$ROOT/build/apple/downloads/$module-everywhere-src-$version.tar.xz"
  download_verified "$base_url/$module-everywhere-src-$version.tar.xz" "$sha" "$archive"
  extract_verified_source "$archive" "$sha" "$source_root/$module"
  args=(-S "$source_root/$module" -B "$build_root/$module" -G Ninja
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix"
    -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT="$APPLE_SDK"
    -DCMAKE_OSX_ARCHITECTURES="$APPLE_ARCH"
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$APPLE_DEPLOYMENT_TARGET"
    -DQT_HOST_PATH="$QT_HOST_PATH" -DQT_BUILD_TESTS=OFF -DQT_BUILD_EXAMPLES=OFF)
  if [[ "$module" == qtbase ]]; then
    cmake "${args[@]}" -DBUILD_SHARED_LIBS=OFF -DQT_QMAKE_TARGET_MKSPEC=macx-ios-clang \
      -DFEATURE_opengl=ON -DINPUT_opengl=es2 -DFEATURE_dbus=OFF -DFEATURE_printsupport=OFF
  else
    "$prefix/bin/qt-cmake" "${args[@]}" \
      -DQt6_DIR="$prefix/lib/cmake/Qt6" \
      -DQt6BuildInternals_DIR="$prefix/lib/cmake/Qt6BuildInternals" \
      -DINPUT_webp=qt -DINPUT_tiff=qt -DINPUT_jasper=no -DINPUT_mng=no
  fi
  cmake --build "$build_root/$module" --parallel "$jobs"
  cmake --install "$build_root/$module"
done
printf 'Built source Qt %s for %s/%s at %s\n' "$version" "$APPLE_SDK" "$APPLE_ARCH" "$prefix"
