#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
ABI="${ANDROID_ABI:-x86_64}"
QT_VERSION="${QT_VERSION:-$(toolchain_field "$ROOT" qt.version)}"
DEPS_PREFIX="${ANDROID_DEPS_PREFIX:-$ROOT/build/android/deps/$ABI}"
JOBS="${ANDROID_BUILD_JOBS:-$(nproc)}"
# RCC embeds source mtimes, including generated files and cached Qt resources.
# Give every ABI the same timestamp without weakening the universal input check.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" log -1 --format=%ct)}"
# The Nix setup hooks export every host build input through these variables, and
# Qt's Android toolchain folds QT_ADDITIONAL_PACKAGES_PREFIX_PATH into
# CMAKE_FIND_ROOT_PATH, which androiddeployqt then scans for libraries to bundle.
# Left alone, host GL/X11 libraries end up in the APK. Keep Android prefixes only.
unset CMAKE_PREFIX_PATH CMAKE_INCLUDE_PATH CMAKE_LIBRARY_PATH CMAKE_FRAMEWORK_PATH \
  Qt6_DIR QT_ADDITIONAL_PACKAGES_PREFIX_PATH QT_ADDITIONAL_HOST_PACKAGES_PREFIX_PATH \
  QMAKEPATH QML2_IMPORT_PATH QT_PLUGIN_PATH PKG_CONFIG_PATH
# Qt's Android toolchain folds this into both CMAKE_PREFIX_PATH and
# CMAKE_FIND_ROOT_PATH, so cross-compiled packages resolve despite the ONLY find
# root mode. It is also what androiddeployqt scans, so it must list Android
# prefixes and nothing else.
export QT_ADDITIONAL_PACKAGES_PREFIX_PATH="$ROOT/build/android/qcoro/$ABI:$DEPS_PREFIX"
# nixpkgs patches CMake's Unix platform module to drop /usr from the default
# search prefixes, which also stops find_path from looking in the NDK sysroot's
# /usr/include. Put it back so EGL/GLESv2 resolve against the NDK.
NDK_PREFIX_PATH=/usr

case "$ABI" in
  arm64-v8a) QT_ABI_DIR=android_arm64_v8a ;;
  armeabi-v7a) QT_ABI_DIR=android_armv7 ;;
  x86_64) QT_ABI_DIR=android_x86_64 ;;
  *)
    echo "error: unsupported Android ABI: $ABI" >&2
    exit 1
    ;;
esac
QT_PREFIX="${QT_ANDROID_PREFIX:-$ROOT/build/android/qt/$QT_VERSION/$QT_ABI_DIR}"

: "${ANDROID_HOME:?run through nix develop .#android}"
: "${ANDROID_NDK_ROOT:?run through nix develop .#android}"
[[ -x "$QT_PREFIX/bin/qt-cmake" ]] || {
  echo "error: Android Qt is not built at $QT_PREFIX" >&2
  exit 1
}
[[ -f "$DEPS_PREFIX/lib/libmpv.so" ]] || {
  echo "error: Android dependencies are not built at $DEPS_PREFIX" >&2
  exit 1
}

# shellcheck source=tools/android/signing.sh
source "$ROOT/tools/android/signing.sh"

build_qcoro() {
  local build="$ROOT/build/android/qcoro-build/$ABI"
  local prefix="$ROOT/build/android/qcoro/$ABI"
  if [[ -f "$prefix/lib/cmake/QCoro6/QCoro6Config.cmake" ]]; then
    printf 'Android QCoro for %s is current\n' "$ABI"
    return
  fi
  rm -rf "$build" "$prefix"
  "$QT_PREFIX/bin/qt-cmake" -S "$ROOT/build/android/sources/third-party/qcoro" -B "$build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_PREFIX_PATH="$NDK_PREFIX_PATH" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DQt6_DIR="$QT_PREFIX/lib/cmake/Qt6" \
    -DQt6Core_DIR="$QT_PREFIX/lib/cmake/Qt6Core" \
    -DQt6CorePrivate_DIR="$QT_PREFIX/lib/cmake/Qt6CorePrivate" \
    -DQt6Network_DIR="$QT_PREFIX/lib/cmake/Qt6Network" \
    -DQT_HOST_PATH="${SPOOL_ANDROID_QT_HOST:-}" \
    -DQT_HOST_PATH_CMAKE_DIR="${SPOOL_ANDROID_QT_HOST:-}/lib/cmake" \
    -DQCORO_BUILD_EXAMPLES=OFF \
    -DQCORO_WITH_QTDBUS=OFF \
    -DQCORO_WITH_QML=OFF \
    -DQCORO_WITH_QTQUICK=OFF \
    -DQCORO_WITH_QTWEBSOCKETS=OFF \
    -DQCORO_BUILD_TESTING=OFF \
    -DBUILD_TESTING=OFF
  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

build_app() {
  local build="$ROOT/build/android/app-$ABI"
  rm -rf "$build"
  "$QT_PREFIX/bin/qt-cmake" -S "$ROOT" -B "$build" -GNinja \
    -DQt6_DIR="$QT_PREFIX/lib/cmake/Qt6" \
    -DQt6Core_DIR="$QT_PREFIX/lib/cmake/Qt6Core" \
    -DQt6Gui_DIR="$QT_PREFIX/lib/cmake/Qt6Gui" \
    -DQt6Network_DIR="$QT_PREFIX/lib/cmake/Qt6Network" \
    -DQt6OpenGL_DIR="$QT_PREFIX/lib/cmake/Qt6OpenGL" \
    -DQt6Qml_DIR="$QT_PREFIX/lib/cmake/Qt6Qml" \
    -DQt6Quick_DIR="$QT_PREFIX/lib/cmake/Qt6Quick" \
    -DQt6Sql_DIR="$QT_PREFIX/lib/cmake/Qt6Sql" \
    -DQt6Svg_DIR="$QT_PREFIX/lib/cmake/Qt6Svg" \
    -DQt6WebSockets_DIR="$QT_PREFIX/lib/cmake/Qt6WebSockets" \
    -DQt6LinguistTools_DIR="${SPOOL_ANDROID_QT_HOST:-}/lib/cmake/Qt6LinguistTools" \
    -DQT_HOST_PATH="${SPOOL_ANDROID_QT_HOST:-}" \
    -DQT_HOST_PATH_CMAKE_DIR="${SPOOL_ANDROID_QT_HOST:-}/lib/cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_PREFIX_PATH="$NDK_PREFIX_PATH" \
    -DBUILD_TESTING=OFF \
    -DSPOOL_WEBOS=OFF \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_DEPS_PREFIX="$DEPS_PREFIX" \
    -DQT_ANDROID_SIGN_APK=ON
  # androiddeployqt scans SOURCE_DIR recursively and does not forward
  # QT_QML_IMPORT_SCANNER_EXTRA_ARGS. A cache miss leaves Qt's sources under
  # build/, so scanning the repository deploys imports from Qt's own tests.
  # Restrict its scan to application QML; generated resources remain in qrcFiles.
  python3 - "$build/android-spool-deployment-settings.json" "$ROOT/qml" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
settings = json.loads(path.read_text())
settings["qml-root-path"] = [sys.argv[2]]
path.write_text(json.dumps(settings, indent=2) + "\n")
PY
  cmake --build "$build" --target spool_make_apk --parallel "$JOBS"

  local apk="$build/android-build/spool.apk"
  [[ -f "$apk" ]] || {
    echo "error: APK was not generated at $apk" >&2
    exit 1
  }
  mkdir -p "$ROOT/dist/android"
  cp -f "$apk" "$ROOT/dist/android/spool-${ABI}.apk"
}

# The universal APK is built from these, not spliced out of the per-ABI APKs:
# res/values/libs.xml names every bundled library with its ABI in front of it,
# and that file is compiled into resources.arsc, which is not something to edit
# afterwards. Staging the Gradle project each ABI was packaged from lets one
# job put the union of those arrays back and package it properly.
#
# The native libraries are left out. They are the largest thing here by two
# orders of magnitude, and the universal build takes the stripped ones straight
# out of this ABI's APK instead.
stage_universal_inputs() {
  local build="$ROOT/build/android/app-$ABI/android-build"
  local staged="$ROOT/dist/android/universal-inputs/$ABI"
  rm -rf "$staged"
  mkdir -p "$staged/libs"
  cp -a "$build/AndroidManifest.xml" "$build/build.gradle" "$build/gradle.properties" \
    "$build/gradlew" "$build/gradle" "$build/res" "$build/src" "$build/assets" "$staged/"
  cp -a "$build"/libs/*.jar "$staged/libs/"
  # gradle.properties points Gradle at the Qt installation for the Java and
  # resource sources every Qt app shares. Carry a copy so the universal build
  # needs no Qt tree of its own, and name it by a path relative to the project.
  cp -a "$QT_PREFIX/src/android/java" "$staged/qt-android"
  sed -i -e 's#^qtAndroidDir=.*#qtAndroidDir=qt-android#' \
    -e 's#^qt5AndroidDir=.*#qt5AndroidDir=qt-android#' "$staged/gradle.properties"
}

prepare_keystore
build_qcoro
# One package serves televisions and handsets. What used to be two builds per
# ABI differing only in a manifest is now one; the form factor is asked of the
# system at runtime and the launch screen comes from a resource qualifier.
build_app
stage_universal_inputs
printf 'Android APK:\n  %s\n' "$ROOT/dist/android/spool-${ABI}.apk"
