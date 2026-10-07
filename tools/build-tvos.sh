#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -s)" != Darwin ]]; then
  echo 'error: tvOS builds require macOS and Xcode with the Apple TV SDK' >&2
  exit 1
fi
export APPLE_SYSTEM=tvOS
export APPLE_SDK="${APPLE_SDK:-appletvsimulator}"
export APPLE_ARCH="${APPLE_ARCH:-arm64}"
export APPLE_DEPLOYMENT_TARGET="${APPLE_DEPLOYMENT_TARGET:-16.0}"
export QT_TARGET_PREFIX="${QT_TARGET_PREFIX:-$ROOT/build/apple/$APPLE_SDK-$APPLE_ARCH/qt}"
export APPLE_DEPS_PREFIX="${APPLE_DEPS_PREFIX:-$ROOT/build/apple/$APPLE_SDK-$APPLE_ARCH/prefix}"
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the pinned macOS Qt host-tools prefix}"
case "$APPLE_SDK:$APPLE_ARCH" in
  appletvos:arm64|appletvsimulator:arm64|appletvsimulator:x86_64) ;;
  *) echo "error: unsupported tvOS SDK/architecture: $APPLE_SDK/$APPLE_ARCH" >&2; exit 1 ;;
esac
xcrun --sdk "$APPLE_SDK" --show-sdk-path >/dev/null
if [[ "$APPLE_SDK" == appletvsimulator ]]; then
  xcrun --find derq >/dev/null
fi
bash "$ROOT/tools/apple/build-qt-tvos.sh"
bash "$ROOT/tools/apple/build-dependencies.sh"
export PKG_CONFIG_LIBDIR="$APPLE_DEPS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
app_build="$ROOT/build/tvos/$APPLE_SDK-$APPLE_ARCH/app"
assets="$ROOT/build/tvos/$APPLE_SDK-$APPLE_ARCH/Assets.xcassets"
python3 "$ROOT/tools/apple/tvos-assets.py" "$assets"
"$QT_TARGET_PREFIX/bin/qt-cmake" -S "$ROOT" -B "$app_build" -G Xcode \
  -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT="$APPLE_SDK" \
  -DCMAKE_OSX_ARCHITECTURES="$APPLE_ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$APPLE_DEPLOYMENT_TARGET" \
  -DCMAKE_PREFIX_PATH="$QT_TARGET_PREFIX;$APPLE_DEPS_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$APPLE_DEPS_PREFIX;$QT_TARGET_PREFIX" \
  -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH="$APPLE_DEPS_PREFIX" \
  -DQCoro6_DIR="$APPLE_DEPS_PREFIX/lib/cmake/QCoro6" \
  -DSPOOL_APPLE_QML_IMPORT_SCANNER="$ROOT/build/apple/qmlimportscanner/spool-qmlimportscanner" \
  -DSPOOL_APPLE_BUNDLE_IDENTIFIER="${APPLE_BUNDLE_IDENTIFIER:-com.sachk.spool}" \
  -DSPOOL_APPLE_CREDENTIAL_SERVICE="${APPLE_BUNDLE_IDENTIFIER:-com.sachk.spool}" \
  -DQT_HOST_PATH="$QT_HOST_PATH" -DSPOOL_WEBOS=OFF -DTOUCHSCREEN=OFF \
  -DSPOOL_APPLE_APP_STORE=ON -DBUILD_TESTING=OFF \
  -DSPOOL_TVOS_ASSETS="$assets" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED="${CODE_SIGNING_ALLOWED:-NO}" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${APPLE_DEVELOPMENT_TEAM:-}"
cmake --build "$app_build" --config Release --parallel "${JOBS:-$(sysctl -n hw.ncpu)}"
cmake --install "$app_build" --config Release --prefix "$ROOT/build/tvos/$APPLE_SDK-$APPLE_ARCH/install"
printf 'tvOS app: %s\n' "$ROOT/build/tvos/$APPLE_SDK-$APPLE_ARCH/install/Spool.app"
