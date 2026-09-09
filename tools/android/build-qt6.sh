#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/lib/build-common.sh"
source "$ROOT/tools/lib/manifest-sources.sh"

PHASE="${1:-all}"
ABI="${ANDROID_ABI:-x86_64}"
MANIFEST="${QT_ANDROID_MANIFEST:-$ROOT/tools/manifests/qt-android-6.11.json}"
QT_VERSION="$(manifest_qt_field "$MANIFEST" qtVersion)"
QT_BASE_URL="$(manifest_qt_field "$MANIFEST" baseUrl)"
QT_HOST_PATH="${SPOOL_ANDROID_QT_HOST:-}"
ANDROID_API="${ANDROID_API:-28}"
JOBS="$(recommended_parallel_jobs "${QT_ANDROID_MEMORY_PER_JOB_MIB:-1536}" "${QT_ANDROID_MEMORY_RESERVE_MIB:-2048}")"
# The Nix setup hooks publish every host build input through these, the same way
# build-dependencies.sh and build-apks.sh guard against. Qt's own find modules
# reach a host library through any one of them and link it into a target
# library: adding ImageMagick to the dev shell was enough for qtimageformats to
# find the host libwebp and fail the arm64 link. A host package must not be able
# to change what this builds.
unset CMAKE_PREFIX_PATH CMAKE_INCLUDE_PATH CMAKE_LIBRARY_PATH CMAKE_FRAMEWORK_PATH \
  Qt6_DIR QT_ADDITIONAL_PACKAGES_PREFIX_PATH QT_ADDITIONAL_HOST_PACKAGES_PREFIX_PATH \
  QMAKEPATH QML2_IMPORT_PATH QT_PLUGIN_PATH

case "$ABI" in
  arm64-v8a)
    QT_ABI_DIR=android_arm64_v8a
    NDK_TRIPLE=aarch64-linux-android
    ;;
  armeabi-v7a)
    QT_ABI_DIR=android_armv7
    NDK_TRIPLE=arm-linux-androideabi
    ;;
  x86_64)
    QT_ABI_DIR=android_x86_64
    NDK_TRIPLE=x86_64-linux-android
    ;;
  *)
    echo "error: unsupported Android ABI: $ABI" >&2
    exit 1
    ;;
esac

SOURCE_ROOT="$ROOT/build/android/sources/qt-$QT_VERSION"
DOWNLOAD_ROOT="$ROOT/build/android/downloads"
BUILD_ROOT="$ROOT/build/android/qt-build/$QT_ABI_DIR"
INSTALL_ROOT="${QT_ANDROID_INSTALL_ROOT:-$ROOT/build/android/qt/$QT_VERSION}"
PREFIX="$INSTALL_ROOT/$QT_ABI_DIR"
ANDROID_DEPS_PREFIX="${ANDROID_DEPS_PREFIX:-$ROOT/build/android/deps/$ABI}"
NDK_SYSROOT="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
NDK_LIBDIR="$NDK_SYSROOT/usr/lib/$NDK_TRIPLE/$ANDROID_API"
# The other half of the same guard: pkg-config is the search path CMake's find
# modules fall back to, so point it at the Android prefix alone. LIBDIR replaces
# the built-in system directories rather than adding to them.
export PKG_CONFIG_PATH="$ANDROID_DEPS_PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$ANDROID_DEPS_PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR

require_environment() {
  : "${ANDROID_HOME:?run through nix develop .#android}"
  : "${ANDROID_NDK_ROOT:?run through nix develop .#android}"
  [[ -d "$ANDROID_HOME/platforms/android-36" ]] || {
    echo "error: Android platform 36 missing" >&2
    exit 1
  }
  [[ -f "$ANDROID_DEPS_PREFIX/lib/libssl.so" ]] || {
    echo "error: build Android dependencies before Qt so Qt Network can link OpenSSL" >&2
    exit 1
  }
  [[ -f "$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" ]] || {
    echo "error: Android NDK toolchain missing at $ANDROID_NDK_ROOT" >&2
    exit 1
  }
  [[ -n "$QT_HOST_PATH" && -x "$QT_HOST_PATH/libexec/moc" ]] || {
    echo "error: SPOOL_ANDROID_QT_HOST must name the Qt 6.11 host-tools prefix" >&2
    exit 1
  }
}

apply_qt_patches() {
  local patch_file
  for patch_file in "$ROOT/tools/android/patches/qt-android-tv-keyboard.patch" \
                    "$ROOT/tools/android/patches/qt-android-live-density.patch"; do
    if patch --dry-run --forward --silent -d "$SOURCE_ROOT/qtbase" -p1 <"$patch_file" >/dev/null 2>&1; then
      patch --forward --silent -d "$SOURCE_ROOT/qtbase" -p1 <"$patch_file"
    elif ! patch --dry-run --reverse --silent -d "$SOURCE_ROOT/qtbase" -p1 <"$patch_file" >/dev/null 2>&1; then
      echo "error: Qt Android patch does not apply cleanly: $patch_file" >&2
      exit 1
    fi
  done
}

module_names() {
  python3 -c 'import json,sys; print("\n".join(x["name"] for x in json.load(open(sys.argv[1]))["modules"]))' "$MANIFEST"
}

fetch_sources() {
  mkdir -p "$SOURCE_ROOT" "$DOWNLOAD_ROOT"
  local module sha archive source
  while IFS= read -r module; do
    [[ -n "$module" ]] || continue
    sha="$(manifest_qt_module_sha256 "$MANIFEST" "$module")"
    archive="$DOWNLOAD_ROOT/$module-everywhere-src-$QT_VERSION.tar.xz"
    source="$SOURCE_ROOT/$module"
    download_verified "$QT_BASE_URL/$module-everywhere-src-$QT_VERSION.tar.xz" "$sha" "$archive"
    extract_verified_source "$archive" "$sha" "$source"
  done < <(module_names)
  apply_qt_patches
}

qt_prefix_current() {
  [[ -f "$PREFIX/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake" ]] || return 1
  grep -Fqx "set(PACKAGE_VERSION \"$QT_VERSION\")" "$PREFIX/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
  [[ -f "$PREFIX/.spool-android-qt-$ABI-api-$ANDROID_API-tv-keyboard-v1-density-v1" ]]
}

build_qtbase() {
  if qt_prefix_current && [[ -f "$PREFIX/lib/cmake/Qt6Gui/Qt6GuiConfig.cmake" ]]; then
    printf 'Android Qt %s qtbase for %s is current\n' "$QT_VERSION" "$ABI"
    return
  fi
  rm -rf "$BUILD_ROOT" "$PREFIX"
  mkdir -p "$BUILD_ROOT/qtbase" "$PREFIX"
  # configure drops config.opt and friends in the working directory, so run it
  # from the build tree rather than littering the repository root.
  (
    cd "$BUILD_ROOT/qtbase"
    "$SOURCE_ROOT/qtbase/configure" \
      -prefix "$PREFIX" \
      -qt-host-path "$QT_HOST_PATH" \
      -android-abis "$ABI" \
      -android-sdk "$ANDROID_HOME" \
      -android-ndk "$ANDROID_NDK_ROOT" \
      -release \
      -opensource -confirm-license \
      -nomake examples -nomake tests \
      -openssl-linked \
      -opengl es2 \
      -no-feature-dbus \
      -no-feature-printsupport \
      -no-feature-cups \
      -- \
      -B "$BUILD_ROOT/qtbase" \
      -GNinja \
      -DANDROID_PLATFORM="android-$ANDROID_API" \
      -DQT_BUILD_TESTS=OFF \
      -DGLESv2_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
      -DEGL_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
      -DOPENGL_GLES2_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
      -DOPENGL_EGL_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
      -DQT_BUILD_EXAMPLES=OFF \
      -DOPENSSL_ROOT_DIR="$ANDROID_DEPS_PREFIX" \
      -DCMAKE_PREFIX_PATH="$ANDROID_DEPS_PREFIX"
  )
  cmake --build "$BUILD_ROOT/qtbase" --parallel "$JOBS"
  cmake --install "$BUILD_ROOT/qtbase"
  printf '1\n' >"$PREFIX/.spool-android-qt-$ABI-api-$ANDROID_API-tv-keyboard-v1-density-v1"
}

module_marker() {
  case "$1" in
    qtshadertools) printf '%s\n' 'lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake' ;;
    qtdeclarative) printf '%s\n' 'lib/cmake/Qt6Quick/Qt6QuickConfig.cmake' ;;
    qtsvg) printf '%s\n' 'lib/cmake/Qt6Svg/Qt6SvgConfig.cmake' ;;
    qtimageformats) printf '%s\n' "plugins/imageformats/libplugins_imageformats_qwebp_$ABI.so" ;;
    qtwebsockets) printf '%s\n' 'lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake' ;;
  esac
}

# Which third-party libraries a module builds against is a property of the
# Android target, not of whatever the host happens to have installed. Qt decides
# it by probing, so say it outright: the bundled copies are the only ones built
# for this ABI, and the formats with no Android source at all stay off.
module_extra_args() {
  case "$1" in
    qtimageformats)
      printf '%s\n' -DINPUT_webp=qt -DINPUT_tiff=qt -DINPUT_jasper=no -DINPUT_mng=no
      ;;
  esac
}

build_module() {
  local module="$1" marker build_dir
  local -a extra_args=()
  mapfile -t extra_args < <(module_extra_args "$module")
  marker="$(module_marker "$module")"
  if [[ ",${QT_ANDROID_FORCE_MODULES:-}," != *",$module,"* && -e "$PREFIX/$marker" ]]; then
    printf 'Android Qt %s %s for %s is current\n' "$QT_VERSION" "$module" "$ABI"
    return
  fi
  build_dir="$BUILD_ROOT/$module"
  rm -rf "$build_dir"
  # Without an explicit Qt6BuildInternals the module picks up the host Qt's copy
  # and inherits its install layout, which puts QML modules and plugins under
  # lib/qt-6 where androiddeployqt never looks.
  "$PREFIX/bin/qt-cmake" -S "$SOURCE_ROOT/$module" -B "$build_dir" -GNinja \
    -DQt6_DIR="$PREFIX/lib/cmake/Qt6" \
    -DQt6BuildInternals_DIR="$PREFIX/lib/cmake/Qt6BuildInternals" \
    -DQt6Core_DIR="$PREFIX/lib/cmake/Qt6Core" \
    -DQt6CorePrivate_DIR="$PREFIX/lib/cmake/Qt6CorePrivate" \
    -DQt6Concurrent_DIR="$PREFIX/lib/cmake/Qt6Concurrent" \
    -DQt6Gui_DIR="$PREFIX/lib/cmake/Qt6Gui" \
    -DQt6GuiPrivate_DIR="$PREFIX/lib/cmake/Qt6GuiPrivate" \
    -DQt6Network_DIR="$PREFIX/lib/cmake/Qt6Network" \
    -DQt6NetworkPrivate_DIR="$PREFIX/lib/cmake/Qt6NetworkPrivate" \
    -DQt6OpenGL_DIR="$PREFIX/lib/cmake/Qt6OpenGL" \
    -DQt6OpenGLPrivate_DIR="$PREFIX/lib/cmake/Qt6OpenGLPrivate" \
    -DQt6Sql_DIR="$PREFIX/lib/cmake/Qt6Sql" \
    -DQt6SqlPrivate_DIR="$PREFIX/lib/cmake/Qt6SqlPrivate" \
    -DQt6Widgets_DIR="$PREFIX/lib/cmake/Qt6Widgets" \
    -DQt6WidgetsPrivate_DIR="$PREFIX/lib/cmake/Qt6WidgetsPrivate" \
    -DQt6Test_DIR="$PREFIX/lib/cmake/Qt6Test" \
    -DQt6TestPrivate_DIR="$PREFIX/lib/cmake/Qt6TestPrivate" \
    -DQt6Svg_DIR="$PREFIX/lib/cmake/Qt6Svg" \
    -DQt6SvgPrivate_DIR="$PREFIX/lib/cmake/Qt6SvgPrivate" \
    -DQt6Qml_DIR="$PREFIX/lib/cmake/Qt6Qml" \
    -DQt6QmlPrivate_DIR="$PREFIX/lib/cmake/Qt6QmlPrivate" \
    -DQt6Quick_DIR="$PREFIX/lib/cmake/Qt6Quick" \
    -DQt6QuickPrivate_DIR="$PREFIX/lib/cmake/Qt6QuickPrivate" \
    -DQt6Xml_DIR="$PREFIX/lib/cmake/Qt6Xml" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DEGL_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
    -DEGL_LIBRARY="$NDK_LIBDIR/libEGL.so" \
    -DGLESv2_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
    -DGLESv2_LIBRARY="$NDK_LIBDIR/libGLESv2.so" \
    -DOPENGL_EGL_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
    -DOPENGL_GLES2_INCLUDE_DIR="$NDK_SYSROOT/usr/include" \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DQT_HOST_PATH_CMAKE_DIR="$QT_HOST_PATH/lib/cmake" \
    -DQT_BUILD_TESTS=OFF \
    -DQT_NO_TARGET_QMLTESTRUNNER=ON \
    -DQT_BUILD_EXAMPLES=OFF \
    -DCMAKE_PREFIX_PATH="$ANDROID_DEPS_PREFIX" \
    "${extra_args[@]}"
  cmake --build "$build_dir" --parallel "$JOBS"
  cmake --install "$build_dir"
}

build_all() {
  require_environment
  fetch_sources
  describe_parallel_jobs "$JOBS" "Android Qt" "${QT_ANDROID_MEMORY_PER_JOB_MIB:-1536}" "${QT_ANDROID_MEMORY_RESERVE_MIB:-2048}"
  build_qtbase
  build_module qtshadertools
  build_module qtsvg
  build_module qtdeclarative
  build_module qtimageformats
  build_module qtwebsockets
  printf 'Android Qt installed at %s\n' "$PREFIX"
}

case "$PHASE" in
  fetch) fetch_sources ;;
  build | all) build_all ;;
  clean) rm -rf "$BUILD_ROOT" "$PREFIX" ;;
  *)
    echo "usage: $0 [fetch|build|all|clean]" >&2
    exit 2
    ;;
esac
