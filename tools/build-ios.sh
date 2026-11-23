#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$(uname -s)" == Darwin ]] || { echo 'iOS builds require macOS and Xcode.' >&2; exit 1; }
: "${QT_TARGET_PREFIX:?Set QT_TARGET_PREFIX to the iOS Qt kit}"
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the matching macOS Qt kit}"
export APPLE_SYSTEM=iOS
export APPLE_SDK="${APPLE_SDK:-iphoneos}"
if [[ "$APPLE_SDK" == iphonesimulator ]]; then
  export APPLE_ARCH="${APPLE_ARCH:-x86_64}"
else
  export APPLE_ARCH="${APPLE_ARCH:-arm64}"
fi
export APPLE_DEPLOYMENT_TARGET="${APPLE_DEPLOYMENT_TARGET:-17.0}"
case "$APPLE_SDK:$APPLE_ARCH" in
  iphoneos:arm64|iphonesimulator:arm64|iphonesimulator:x86_64) ;;
  *) echo "error: unsupported iOS SDK/architecture: $APPLE_SDK/$APPLE_ARCH" >&2; exit 1 ;;
esac
# SPOOL_APPLE_APP_STORE=OFF builds the unsigned sideload flavour: every
# bundled provider and provider downloads, as on Android. Store submissions
# keep the default.
case "${SPOOL_APPLE_APP_STORE:-ON}" in
  ON|OFF) ;;
  *) echo "error: SPOOL_APPLE_APP_STORE must be ON or OFF" >&2; exit 1 ;;
esac
if [[ "${APPLE_ARCHIVE:-0}" == 1 ]]; then
  [[ "$APPLE_SDK" == iphoneos ]] || { echo 'Signed archives require APPLE_SDK=iphoneos.' >&2; exit 1; }
  [[ "${SPOOL_APPLE_APP_STORE:-ON}" == ON ]] || { echo 'Store archives require SPOOL_APPLE_APP_STORE=ON.' >&2; exit 1; }
  : "${APPLE_DEVELOPMENT_TEAM:?Archiving requires a signing team}"
  : "${APPLE_EXPORT_OPTIONS:?Set APPLE_EXPORT_OPTIONS to your ExportOptions.plist}"
fi
xcrun --sdk "$APPLE_SDK" --show-sdk-path >/dev/null
bash "$ROOT/tools/apple/build-ios-scanner.sh"
BASE="$ROOT/build/apple/$APPLE_SDK-$APPLE_ARCH"
export APPLE_DEPS_PREFIX="${APPLE_DEPS_PREFIX:-$BASE/prefix}"
if [[ "${BUILD_DEPENDENCIES:-1}" == 1 ]]; then "$ROOT/tools/apple/build-dependencies.sh"; fi
export PKG_CONFIG_PATH="$APPLE_DEPS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PKG_CONFIG_PATH"
ASSETS="$BASE/Spool.xcassets"
mkdir -p "$ASSETS/AppIcon.appiconset"
magick "$ROOT/app/icons/png/spool/1024.png" -background black -alpha remove -alpha off "$ASSETS/AppIcon.appiconset/icon.png"
printf '%s\n' '{"images":[{"filename":"icon.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}' >"$ASSETS/AppIcon.appiconset/Contents.json"
cmake -S "$ROOT" -B "$BASE/app" -GXcode \
  -DCMAKE_TOOLCHAIN_FILE="$QT_TARGET_PREFIX/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DQT_HOST_PATH="$QT_HOST_PATH" -DCMAKE_PREFIX_PATH="$APPLE_DEPS_PREFIX;$QT_TARGET_PREFIX" \
  -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH="$APPLE_DEPS_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$APPLE_DEPS_PREFIX;$QT_TARGET_PREFIX" \
  -DQCoro6_DIR="$APPLE_DEPS_PREFIX/lib/cmake/QCoro6" \
  -DCMAKE_OSX_SYSROOT="$APPLE_SDK" -DCMAKE_OSX_ARCHITECTURES="$APPLE_ARCH" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$APPLE_DEPLOYMENT_TARGET" \
  -DSPOOL_WEBOS=OFF -DSPOOL_APPLE_APP_STORE="${SPOOL_APPLE_APP_STORE:-ON}" -DBUILD_TESTING=OFF \
  -DSPOOL_APPLE_QML_IMPORT_SCANNER="$ROOT/build/apple/qmlimportscanner/spool-qmlimportscanner" \
  -DSPOOL_IOS_ASSETS="$ASSETS" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${APPLE_DEVELOPMENT_TEAM:-}" \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED="${APPLE_CODE_SIGNING_ALLOWED:-NO}"
cmake --build "$BASE/app" --config Release --target spool --parallel "${BUILD_JOBS:-4}"
if [[ "${APPLE_ARCHIVE:-0}" == 1 ]]; then
  : "${APPLE_DEVELOPMENT_TEAM:?Archiving requires a signing team}"
  : "${APPLE_EXPORT_OPTIONS:?Set APPLE_EXPORT_OPTIONS to your ExportOptions.plist}"
  xcodebuild -project "$BASE/app/SpoolWebOS.xcodeproj" -scheme spool -configuration Release \
    -destination 'generic/platform=iOS' -archivePath "$BASE/Spool.xcarchive" \
    DEVELOPMENT_TEAM="$APPLE_DEVELOPMENT_TEAM" CODE_SIGNING_ALLOWED=YES archive
  xcodebuild -exportArchive -archivePath "$BASE/Spool.xcarchive" -exportPath "$ROOT/dist/ios" \
    -exportOptionsPlist "$APPLE_EXPORT_OPTIONS"
fi
