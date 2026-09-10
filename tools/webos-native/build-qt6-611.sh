#!/usr/bin/env bash
set -euo pipefail

# Build a private Qt 6.11 tree for native webOS development.
#
# Design goals:
#   - never accidentally link against nixpkgs' Qt while building Qt from source;
#   - build a full-enough host Qt for Qt's own tools, including Widgets;
#   - keep the webOS target Qt lean;
#   - make reruns incremental, but clean obviously poisoned CMake caches;
#   - provide clear, actionable failures for missing SDK/tooling pieces.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$ROOT/tools/lib/build-common.sh"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
# shellcheck source=tools/webos-native/nixos-sdk-compat.sh
source "$ROOT/tools/webos-native/nixos-sdk-compat.sh"
QT_MANIFEST="${QT_MANIFEST:-$ROOT/tools/manifests/qt-webos-6.11.json}"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
QT_VERSION="${QT_VERSION:-$(manifest_qt_field "$QT_MANIFEST" qtVersion)}"
QT_SERIES="${QT_VERSION%.*}"
QT_BASE_URL="${QT_BASE_URL:-$(manifest_qt_field "$QT_MANIFEST" baseUrl)}"
QT_STATIC="${QT_STATIC:-0}"
PHASE="${1:-all}"
QT_BUILD_CLEAN_POISONED="${QT_BUILD_CLEAN_POISONED:-1}"
QT_BUILD_MEMORY_PER_JOB_MIB="${QT_BUILD_MEMORY_PER_JOB_MIB:-1536}"
QT_BUILD_MEMORY_RESERVE_MIB="${QT_BUILD_MEMORY_RESERVE_MIB:-2048}"
QT_TARGET_BUILD_WITH_PCH="${QT_TARGET_BUILD_WITH_PCH:-OFF}"
if [[ -z "${QT_BUILD_PRUNE_COMPLETED+x}" ]]; then
  case "${CI:-}" in
    1|true|TRUE|yes|YES) QT_BUILD_PRUNE_COMPLETED=1 ;;
    *) QT_BUILD_PRUNE_COMPLETED=0 ;;
  esac
fi
JOBS="$(recommended_parallel_jobs "$QT_BUILD_MEMORY_PER_JOB_MIB" "$QT_BUILD_MEMORY_RESERVE_MIB")"
QT_TARGET_TUNE_CFLAGS="$(webos_tune_cflags)"

SRC_DIR="$ROOT/build/qt6-src"
QTBASE_TARBALL="$SRC_DIR/qtbase-everywhere-src-$QT_VERSION.tar.xz"
QTSHADERTOOLS_TARBALL="$SRC_DIR/qtshadertools-everywhere-src-$QT_VERSION.tar.xz"
QTTOOLS_TARBALL="$SRC_DIR/qttools-everywhere-src-$QT_VERSION.tar.xz"
QTDECLARATIVE_TARBALL="$SRC_DIR/qtdeclarative-everywhere-src-$QT_VERSION.tar.xz"
QTWEBSOCKETS_TARBALL="$SRC_DIR/qtwebsockets-everywhere-src-$QT_VERSION.tar.xz"
QTWAYLAND_TARBALL="$SRC_DIR/qtwayland-everywhere-src-$QT_VERSION.tar.xz"
QTIMAGEFORMATS_TARBALL="$SRC_DIR/qtimageformats-everywhere-src-$QT_VERSION.tar.xz"
QTSVG_TARBALL="$SRC_DIR/qtsvg-everywhere-src-$QT_VERSION.tar.xz"

QTBASE_SRC="$SRC_DIR/qtbase-everywhere-src-$QT_VERSION"
QTSHADERTOOLS_SRC="$SRC_DIR/qtshadertools-everywhere-src-$QT_VERSION"
QTTOOLS_SRC="$SRC_DIR/qttools-everywhere-src-$QT_VERSION"
QTDECLARATIVE_SRC="$SRC_DIR/qtdeclarative-everywhere-src-$QT_VERSION"
QTWEBSOCKETS_SRC="$SRC_DIR/qtwebsockets-everywhere-src-$QT_VERSION"
QTWAYLAND_SRC="$SRC_DIR/qtwayland-everywhere-src-$QT_VERSION"
QTIMAGEFORMATS_SRC="$SRC_DIR/qtimageformats-everywhere-src-$QT_VERSION"
QTSVG_SRC="$SRC_DIR/qtsvg-everywhere-src-$QT_VERSION"

QT_BUILD_TAG="${QT_SERIES//./}"
HOST_BUILD_ROOT="$ROOT/build/qt6-$QT_BUILD_TAG-host"
HOST_INSTALL="$ROOT/build/qt6-$QT_BUILD_TAG-host-install"

if [[ "$QT_STATIC" == "1" ]]; then
  TARGET_BUILD_ROOT="$ROOT/build/qt6-$QT_BUILD_TAG-target-static"
  TARGET_STAGING="$ROOT/build/qt6-$QT_BUILD_TAG-target-static-install"
  TARGET_PREFIX="${QT_TARGET_PREFIX:-/opt/qt6-webos-$QT_SERIES-static}"
else
  TARGET_BUILD_ROOT="$ROOT/build/qt6-$QT_BUILD_TAG-target"
  TARGET_STAGING="$ROOT/build/qt6-$QT_BUILD_TAG-target-install"
  TARGET_PREFIX="${QT_TARGET_PREFIX:-/opt/qt6-webos-$QT_SERIES}"
fi
TARGET_QTBASE_REBUILT=0
HOST_QTBASE_REBUILT=0
HOST_MODULES_REBUILD=0
if [[ ! -f "$HOST_INSTALL/.jellyfin-host-tools-profile-v1" ]]; then
  HOST_MODULES_REBUILD=1
fi

TOOLCHAIN_FILE="$ROOT/tools/webos-native/qt6-webos-toolchain.cmake"
PKG_CONFIG_WEBOS="$ROOT/tools/webos-native/pkg-config-webos.sh"
PATCH_DIR="$ROOT/tools/webos-native/patches"
HOST_WAYLAND_SCANNER_DEFAULT="$(command -v wayland-scanner || true)"
TARGET_WAYLAND_SCANNER_DEFAULT="$SDK_ROOT/bin/wayland-scanner"

fresh_flag=()
if [[ "${QT_BUILD_FRESH:-0}" == "1" ]]; then
  fresh_flag=(--fresh)
fi

cmake_registry_flags=(
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
)

log() { printf '\n>>> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

have() { command -v "$1" >/dev/null 2>&1; }
require_command() { have "$1" || die "required command '$1' not found in PATH"; }

prune_completed_build_dir() {
  local dir="$1"
  [[ "$QT_BUILD_PRUNE_COMPLETED" == "1" ]] || return 0
  log "Removing completed build directory $dir"
  rm -rf -- "$dir"
}

# Run CMake with Qt-related environment variables stripped. This is the main
# guard against nixpkgs Qt leaking into the Qt source build through mkShell's
# CMAKE_PREFIX_PATH or user shell state.
cmake_clean_env() {
  env \
    -u CMAKE_PREFIX_PATH \
    -u Qt6_DIR \
    -u Qt6Core_DIR \
    -u Qt6CoreTools_DIR \
    -u Qt6Gui_DIR \
    -u Qt6GuiTools_DIR \
    -u Qt6Widgets_DIR \
    -u Qt6WidgetsTools_DIR \
    -u Qt6Qml_DIR \
    -u Qt6QmlTools_DIR \
    -u Qt6Quick_DIR \
    -u Qt6ShaderTools_DIR \
    -u Qt6LinguistTools_DIR \
    -u Qt6WebSockets_DIR \
    -u Qt6Svg_DIR \
    -u Qt6WaylandClient_DIR \
    -u Qt6WaylandScannerTools_DIR \
    -u PKG_CONFIG \
    -u PKG_CONFIG_PATH \
    -u PKG_CONFIG_LIBDIR \
    -u PKG_CONFIG_SYSROOT_DIR \
    -u QT_PLUGIN_PATH \
    -u QML_IMPORT_PATH \
    -u QML2_IMPORT_PATH \
    -u QT_SELECT \
    "$@"
}

cmake_configure() { cmake_clean_env cmake "$@"; }
cmake_build() { cmake_clean_env cmake --build "$@"; }
cmake_install() { cmake_clean_env cmake --install "$@"; }


require_base_tools() {
  require_command cmake
  require_command ninja
  require_command curl
  require_command tar
  require_command patch
  require_command sed
  require_command find
}

require_target_sdk() {
  [[ -f "$TOOLCHAIN_FILE" ]] || die "toolchain file not found at $TOOLCHAIN_FILE"
  [[ -x "$PKG_CONFIG_WEBOS" ]] || die "webOS pkg-config wrapper not executable: $PKG_CONFIG_WEBOS"
  [[ -d "$SDK_ROOT" ]] || die "WEBOS_SDK_ROOT does not exist: $SDK_ROOT"
  [[ -d "$SYSROOT" ]] || die "webOS sysroot does not exist: $SYSROOT"
  ensure_webos_sdk_host_tools "$SDK_ROOT"
}

sdk_sysroot_library() {
  local name="$1"
  local candidate
  local matches=()
  local nullglob_state

  for candidate in "$SYSROOT/usr/lib/lib${name}.so" "$SYSROOT/lib/lib${name}.so"; do
    if [[ -e "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  nullglob_state="$(shopt -p nullglob)"
  shopt -s nullglob
  matches=("$SYSROOT/usr/lib/lib${name}.so."* "$SYSROOT/lib/lib${name}.so."*)
  eval "$nullglob_state"

  if (( ${#matches[@]} > 0 )); then
    printf '%s\n' "${matches[0]}"
    return 0
  fi

  die "webOS sysroot library not found: lib${name}.so[.*]"
}

maybe_clean_poisoned_build_dir() {
  local dir="$1"
  local expected_source="$2"
  [[ "$QT_BUILD_CLEAN_POISONED" == "1" ]] || return 0
  [[ -f "$dir/CMakeCache.txt" ]] || return 0

  if ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$expected_source" "$dir/CMakeCache.txt"; then
    log "Removing stale-source CMake cache: $dir"
    rm -rf "$dir"
  elif grep -Eq '/nix/store/[^ ;"]*-qt(base|declarative|tools|websockets|wayland|imageformats|svg)-' "$dir/CMakeCache.txt"; then
    log "Removing nixpkgs-poisoned CMake cache: $dir"
    rm -rf "$dir"
  fi
}

clean_host() {
  rm -rf "$HOST_BUILD_ROOT" "$HOST_INSTALL"
}

clean_target() {
  rm -rf "$TARGET_BUILD_ROOT" "$TARGET_STAGING"
}

download_submodule() {
  local module="$1"
  local tarball="$2"
  local expected
  expected="$(manifest_qt_module_sha256 "$QT_MANIFEST" "$module")"

  log "Downloading $module $QT_VERSION"
  download_verified \
    "$QT_BASE_URL/${module}-everywhere-src-$QT_VERSION.tar.xz" \
    "$expected" \
    "$tarball"
}

extract_if_needed() {
  local tarball="$1"
  local src_dir="$2"
  local module expected
  module="$(basename "$tarball" "-everywhere-src-$QT_VERSION.tar.xz")"
  expected="$(manifest_qt_module_sha256 "$QT_MANIFEST" "$module")"
  extract_verified_source "$tarball" "$expected" "$src_dir"
}

apply_patch_if_needed() {
  local src_dir="$1"
  local patch_file="$2"
  shift 2 || true

  [[ -d "$src_dir" ]] || die "source directory not found: $src_dir"
  [[ -f "$patch_file" ]] || die "missing patch file: $patch_file"

  (
    cd "$src_dir"
    if patch -p1 --dry-run --reverse --silent < "$patch_file" >/dev/null 2>&1; then
      return 0
    fi
    patch -p1 < "$patch_file"
  )
}

apply_local_patches() {
  apply_patch_if_needed "$QTBASE_SRC" "$PATCH_DIR/qtbase-$QT_SERIES-webos-qstorageinfo-linux.patch"
  apply_patch_if_needed "$QTBASE_SRC" "$PATCH_DIR/qtbase-$QT_SERIES-webos-qelfparser.patch"
  apply_patch_if_needed "$QTBASE_SRC" "$PATCH_DIR/qtbase-$QT_SERIES-webos-wayland-no-opengl-forward-decl.patch"
  apply_patch_if_needed "$QTBASE_SRC" "$PATCH_DIR/qtbase-$QT_SERIES-webos-qplatformwindow-private-moc.patch"
  apply_patch_if_needed "$QTDECLARATIVE_SRC" \
    "$PATCH_DIR/qtdeclarative-$QT_SERIES-qmlimportscanner-exclude-subtrees.patch"
  apply_patch_if_needed "$QTWAYLAND_SRC" "$PATCH_DIR/qtwayland-$QT_SERIES-webos-wayland-version.patch"

  # In Qt 6.11 the client-side wayland platform plugin code is in qtbase, not
  # qtwayland. This keeps the webOS magic-remote cursor opt-out built in.
  apply_patch_if_needed "$QTBASE_SRC" "$PATCH_DIR/qtbase-$QT_SERIES-webos-no-cursor-set.patch"
}

qt_prefix_matches_version() {
  local prefix="$1"
  local version_file="$prefix/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"
  [[ -f "$version_file" ]] || return 1
  grep -Fqx "set(PACKAGE_VERSION \"$QT_VERSION\")" "$version_file"
}

clean_mismatched_qt_prefix() {
  local prefix="$1"
  local build_root="$2"
  local label="$3"
  local version_file="$prefix/lib/cmake/Qt6/Qt6ConfigVersionImpl.cmake"

  [[ -f "$version_file" ]] || return 0
  if ! qt_prefix_matches_version "$prefix"; then
    log "Removing $label built for a different Qt patch release"
    rm -rf "$build_root" "$prefix"
  fi
}

fetch_sources() {
  require_base_tools
  mkdir -p "$SRC_DIR"

  download_submodule qtbase "$QTBASE_TARBALL"
  download_submodule qtshadertools "$QTSHADERTOOLS_TARBALL"
  download_submodule qttools "$QTTOOLS_TARBALL"
  download_submodule qtdeclarative "$QTDECLARATIVE_TARBALL"
  download_submodule qtwebsockets "$QTWEBSOCKETS_TARBALL"
  download_submodule qtwayland "$QTWAYLAND_TARBALL"
  download_submodule qtimageformats "$QTIMAGEFORMATS_TARBALL"
  download_submodule qtsvg "$QTSVG_TARBALL"

  extract_if_needed "$QTBASE_TARBALL" "$QTBASE_SRC"
  extract_if_needed "$QTSHADERTOOLS_TARBALL" "$QTSHADERTOOLS_SRC"
  extract_if_needed "$QTTOOLS_TARBALL" "$QTTOOLS_SRC"
  extract_if_needed "$QTDECLARATIVE_TARBALL" "$QTDECLARATIVE_SRC"
  extract_if_needed "$QTWEBSOCKETS_TARBALL" "$QTWEBSOCKETS_SRC"
  extract_if_needed "$QTWAYLAND_TARBALL" "$QTWAYLAND_SRC"
  extract_if_needed "$QTIMAGEFORMATS_TARBALL" "$QTIMAGEFORMATS_SRC"
  extract_if_needed "$QTSVG_TARBALL" "$QTSVG_SRC"

  apply_local_patches
}

host_qtbase_up_to_date() {
  [[ "${QT_BUILD_FORCE:-0}" != "1" ]] || return 1
  qt_prefix_matches_version "$HOST_INSTALL" || return 1
  [[ -f "$HOST_INSTALL/lib/cmake/Qt6/Qt6Config.cmake" ]] || return 1
  [[ -e "$HOST_INSTALL/lib/libQt6Core.so" || -e "$HOST_INSTALL/lib/libQt6Core.a" ]] || return 1
  [[ -f "$HOST_INSTALL/.jellyfin-host-qtbase-profile-v1" ]] || return 1
  [[ -e "$HOST_INSTALL/lib/libQt6Gui.so" || -e "$HOST_INSTALL/lib/libQt6Gui.a" ]] || return 1
  [[ -e "$HOST_INSTALL/lib/libQt6Widgets.so" || -e "$HOST_INSTALL/lib/libQt6Widgets.a" ]] || return 1
  return 0
}

configure_host_qtbase() {
  clean_mismatched_qt_prefix "$HOST_INSTALL" "$HOST_BUILD_ROOT" "host Qt prefix"
  maybe_clean_poisoned_build_dir "$HOST_BUILD_ROOT/qtbase" "$QTBASE_SRC"
  log "Configuring host qtbase"
  cmake_configure -S "$QTBASE_SRC" -B "$HOST_BUILD_ROOT/qtbase" -GNinja \
    "${fresh_flag[@]}" \
    "${cmake_registry_flags[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOST_INSTALL" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DINPUT_opengl=no \
    -DFEATURE_vulkan=OFF \
    -DFEATURE_gui=ON \
    -DFEATURE_widgets=ON \
    -DFEATURE_accessibility=ON \
    -DFEATURE_printsupport=OFF \
    -DFEATURE_network=ON \
    -DFEATURE_sql=ON \
    -DFEATURE_concurrent=ON \
    -DFEATURE_dbus=OFF \
    -DFEATURE_xml=OFF \
    -DFEATURE_xcb=OFF \
    -DFEATURE_gtk=OFF \
    -DFEATURE_glib=OFF \
    -DFEATURE_system_pcre2=OFF
}

build_host_qtbase() {
  if host_qtbase_up_to_date; then
    log "host qtbase: up to date, skipping"
    return 0
  fi
  if [[ ! -f "$HOST_INSTALL/.jellyfin-host-qtbase-profile-v1" ]]; then
    log "host Qt profile changed; removing incompatible cached host prefix"
    rm -rf "$HOST_INSTALL" "$HOST_BUILD_ROOT"
  fi
  HOST_QTBASE_REBUILT=1
  configure_host_qtbase
  cmake_build "$HOST_BUILD_ROOT/qtbase" --parallel "$JOBS"
  cmake_install "$HOST_BUILD_ROOT/qtbase"
  prune_completed_build_dir "$HOST_BUILD_ROOT/qtbase"
  printf '1\n' >"$HOST_INSTALL/.jellyfin-host-qtbase-profile-v1"
}

configure_host_module() {
  local name="$1"
  local src="$2"
  local extra=("${@:3}")

  maybe_clean_poisoned_build_dir "$HOST_BUILD_ROOT/$name" "$src"
  log "Configuring host $name"
  cmake_configure -S "$src" -B "$HOST_BUILD_ROOT/$name" -GNinja \
    "${fresh_flag[@]}" \
    "${cmake_registry_flags[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOST_INSTALL" \
    -DCMAKE_PREFIX_PATH="$HOST_INSTALL" \
    -DQt6_DIR="$HOST_INSTALL/lib/cmake/Qt6" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    "${extra[@]}"
}

build_host_module() {
  local name="$1"
  cmake_build "$HOST_BUILD_ROOT/$name" --parallel "$JOBS"
  cmake_install "$HOST_BUILD_ROOT/$name"
  prune_completed_build_dir "$HOST_BUILD_ROOT/$name"
}

host_module_up_to_date() {
  local name="$1"
  local marker="$2"
  [[ "${QT_BUILD_FORCE:-0}" != "1" ]] || return 1
  [[ "$HOST_QTBASE_REBUILT" != "1" ]] || return 1
  [[ "$HOST_MODULES_REBUILD" != "1" ]] || return 1
  [[ ",${QT_BUILD_FORCE_MODULES:-}," != *",$name,"* ]] || return 1
  [[ -e "$HOST_INSTALL/$marker" ]] || return 1
  return 0
}

qmlimportscanner_excludes_subtrees() {
  local scanner="$HOST_INSTALL/libexec/qmlimportscanner"
  [[ -x "$scanner" ]] || return 1

  local fixture output
  fixture="$(mktemp -d)"
  output="$fixture/imports.json"
  mkdir -p "$fixture/build/nested"
  printf 'import Excluded.Module\n' >"$fixture/build/nested/Excluded.qml"

  if ! "$scanner" \
      -rootPath "$fixture" \
      -exclude build \
      >"$output" 2>/dev/null; then
    rm -rf "$fixture"
    return 1
  fi

  if grep -q 'Excluded.Module' "$output"; then
    rm -rf "$fixture"
    return 1
  fi

  rm -rf "$fixture"
}




build_all_host_modules() {
  local host_wayland_scanner="${HOST_WAYLAND_SCANNER:-$HOST_WAYLAND_SCANNER_DEFAULT}"
  [[ -n "$host_wayland_scanner" && -x "$host_wayland_scanner" ]] || \
    die "host wayland-scanner not found; add wayland-scanner to the shell or set HOST_WAYLAND_SCANNER"

  if host_module_up_to_date qtshadertools "lib/cmake/Qt6ShaderTools/Qt6ShaderToolsConfig.cmake"; then
    log "host qtshadertools: up to date, skipping"
  else
    configure_host_module qtshadertools "$QTSHADERTOOLS_SRC"
    build_host_module qtshadertools
  fi

  if host_module_up_to_date qttools "lib/cmake/Qt6LinguistTools/Qt6LinguistToolsConfig.cmake"; then
    log "host qttools: up to date, skipping"
  else
    configure_host_module qttools "$QTTOOLS_SRC" \
      -DFEATURE_linguist=ON \
      -DFEATURE_assistant=OFF \
      -DFEATURE_designer=OFF \
      -DFEATURE_distancefieldgenerator=OFF \
      -DFEATURE_pixeltool=OFF \
      -DFEATURE_qdoc=OFF \
      -DFEATURE_qtattributionsscanner=OFF \
      -DFEATURE_qtdiag=OFF \
      -DFEATURE_qtplugininfo=OFF
    build_host_module qttools
  fi

  if host_module_up_to_date qtdeclarative "lib/cmake/Qt6QmlTools/Qt6QmlToolsConfig.cmake" \
      && qmlimportscanner_excludes_subtrees; then
    log "host qtdeclarative: up to date, skipping"
  else
    # The host prefix only supplies QML code-generation tools to the cross
    # build. Hiding qsb prevents Qt Quick, Controls, styles, and examples from
    # being built and installed for the host.
    configure_host_module qtdeclarative "$QTDECLARATIVE_SRC" \
      -DCMAKE_DISABLE_FIND_PACKAGE_Qt6ShaderToolsTools=TRUE \
      -DFEATURE_quick_vectorimage=OFF
    build_host_module qtdeclarative
  fi

  if host_module_up_to_date qtwebsockets "lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake"; then
    log "host qtwebsockets: up to date, skipping"
  else
    configure_host_module qtwebsockets "$QTWEBSOCKETS_SRC"
    build_host_module qtwebsockets
  fi


  if host_module_up_to_date qtwayland "lib/cmake/Qt6WaylandClient/Qt6WaylandClientConfig.cmake"; then
    log "host qtwayland: up to date, skipping"
  else
    configure_host_module qtwayland "$QTWAYLAND_SRC" \
      -DWaylandScanner_EXECUTABLE="$host_wayland_scanner"
    build_host_module qtwayland
  fi


  if [[ -z "${QT_BUILD_FORCE_MODULES:-}" ]]; then
    printf '1\n' >"$HOST_INSTALL/.jellyfin-host-tools-profile-v1"
  fi

}

configure_target_qtbase() {
  require_target_sdk
  clean_mismatched_qt_prefix "$TARGET_STAGING" "$TARGET_BUILD_ROOT" "target Qt prefix"
  local target_wayland_scanner="${TARGET_WAYLAND_SCANNER:-$TARGET_WAYLAND_SCANNER_DEFAULT}"
  [[ -x "$target_wayland_scanner" ]] || \
    die "target wayland-scanner not found at $target_wayland_scanner; set TARGET_WAYLAND_SCANNER"
  local sdk_glesv2_lib sdk_egl_lib sdk_xkb_lib sdk_ssl_lib sdk_crypto_lib
  local sdk_wayland_client_lib sdk_wayland_server_lib sdk_wayland_cursor_lib sdk_wayland_egl_lib
  sdk_glesv2_lib="$(sdk_sysroot_library GLESv2)"
  sdk_egl_lib="$(sdk_sysroot_library EGL)"
  sdk_xkb_lib="$(sdk_sysroot_library xkbcommon)"
  sdk_ssl_lib="$(sdk_sysroot_library ssl)"
  sdk_crypto_lib="$(sdk_sysroot_library crypto)"
  sdk_wayland_client_lib="$(sdk_sysroot_library wayland-client)"
  sdk_wayland_server_lib="$(sdk_sysroot_library wayland-server)"
  sdk_wayland_cursor_lib="$(sdk_sysroot_library wayland-cursor)"
  sdk_wayland_egl_lib="$(sdk_sysroot_library wayland-egl)"

  local build_type_flags=()
  if [[ "$QT_STATIC" == "1" ]]; then
    build_type_flags=(
      -DBUILD_SHARED_LIBS=OFF
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_BUILD_TYPE=MinSizeRel
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
      "-DCMAKE_C_FLAGS_MINSIZEREL=$QT_TARGET_TUNE_CFLAGS -Os -ffunction-sections -fdata-sections -DNDEBUG"
      "-DCMAKE_CXX_FLAGS_MINSIZEREL=$QT_TARGET_TUNE_CFLAGS -Os -ffunction-sections -fdata-sections -DNDEBUG"
      "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--gc-sections"
      -DFEATURE_ltcg=ON
      -DFEATURE_reduce_exports=ON
    )
  else
    build_type_flags=(-DCMAKE_BUILD_TYPE=Release)
  fi

  maybe_clean_poisoned_build_dir "$TARGET_BUILD_ROOT/qtbase" "$QTBASE_SRC"
  log "Configuring target qtbase"
  cmake_configure -S "$QTBASE_SRC" -B "$TARGET_BUILD_ROOT/qtbase" -GNinja \
    "${fresh_flag[@]}" \
    "${cmake_registry_flags[@]}" \
    "${build_type_flags[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG_WEBOS" \
    -DQT_HOST_PATH="$HOST_INSTALL" \
    -DQT_HOST_PATH_CMAKE_DIR="$HOST_INSTALL/lib/cmake" \
    -DCMAKE_STAGING_PREFIX="$TARGET_STAGING" \
    -DCMAKE_INSTALL_PREFIX="$TARGET_PREFIX" \
    -DBUILD_WITH_PCH="$QT_TARGET_BUILD_WITH_PCH" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DFEATURE_testlib=OFF \
    -DINPUT_opengl=es2 \
    -DFEATURE_egl=ON \
    -DFEATURE_gui=ON \
    -DFEATURE_widgets=OFF \
    -DFEATURE_network=ON \
    -DFEATURE_sql=ON \
    -DFEATURE_concurrent=ON \
    -DFEATURE_dbus=OFF \
    -DFEATURE_printsupport=OFF \
    -DFEATURE_xml=OFF \
    -DFEATURE_glib=OFF \
    -DFEATURE_icu=OFF \
    -DFEATURE_xcb=OFF \
    -DFEATURE_gtk=OFF \
    -DFEATURE_accessibility=ON \
    -DFEATURE_accessibility_atspi_bridge=OFF \
    -DFEATURE_xkbcommon=ON \
    -DFEATURE_wayland=ON \
    -DFEATURE_wayland_client=ON \
    -DFEATURE_wayland_client_wl_shell=ON \
    -DFEATURE_wayland_client_xdg_shell=ON \
    -DFEATURE_wayland_egl=ON \
    -DFEATURE_system_freetype=ON \
    -DFEATURE_fontconfig=ON \
    -DFEATURE_webp=ON \
    -DFEATURE_system_pcre2=OFF \
    -DGLESv2_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DGLESv2_LIBRARY="$sdk_glesv2_lib" \
    -DEGL_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DEGL_LIBRARY="$sdk_egl_lib" \
    -DXKB_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DXKB_LIBRARY="$sdk_xkb_lib" \
    -DFREETYPE_INCLUDE_DIR_freetype2="$SYSROOT/usr/include/freetype2" \
    -DFREETYPE_INCLUDE_DIR_ft2build="$SYSROOT/usr/include/freetype2" \
    -DFREETYPE_LIBRARY_RELEASE="$SYSROOT/usr/lib/libfreetype.so" \
    -DFontconfig_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DFontconfig_LIBRARY="$SYSROOT/usr/lib/libfontconfig.so" \
    -DWayland_Client_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Client_LIBRARY="$sdk_wayland_client_lib" \
    -DWayland_Server_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Server_LIBRARY="$sdk_wayland_server_lib" \
    -DWayland_Cursor_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Cursor_LIBRARY="$sdk_wayland_cursor_lib" \
    -DWayland_Egl_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Egl_LIBRARY="$sdk_wayland_egl_lib" \
    -DINPUT_openssl=linked \
    -DFEATURE_ssl=ON \
    -DOPENSSL_USE_STATIC_LIBS=OFF \
    -DOPENSSL_ROOT_DIR="$SYSROOT/usr" \
    -DOPENSSL_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DOPENSSL_SSL_LIBRARY="$sdk_ssl_lib" \
    -DOPENSSL_CRYPTO_LIBRARY="$sdk_crypto_lib" \
    -DWaylandScanner_EXECUTABLE="$target_wayland_scanner"
}

target_qtbase_up_to_date() {
  [[ "${QT_BUILD_FORCE:-0}" != "1" ]] || return 1
  qt_prefix_matches_version "$TARGET_STAGING" || return 1
  [[ -f "$TARGET_STAGING/.jellyfin-openssl-shared" ]] || return 1
  [[ -f "$TARGET_STAGING/.spool-no-testlib-v1" ]] || return 1
  if [[ "$QT_STATIC" == "1" ]]; then
    [[ -f "$TARGET_STAGING/.jellyfin-static-size-profile-v1" ]] || return 1
    [[ -f "$TARGET_STAGING/.jellyfin-static-pic-profile-v1" ]] || return 1
  fi
  [[ -f "$TARGET_STAGING/lib/cmake/Qt6/Qt6Config.cmake" ]] || return 1
  [[ -e "$TARGET_STAGING/$(target_lib_marker Qt6Core)" ]] || return 1
  [[ -e "$TARGET_STAGING/$(target_lib_marker Qt6Gui)" ]] || return 1
  local tls_backend_marker="plugins/tls/libqopensslbackend.so"
  [[ "$QT_STATIC" != "1" ]] || tls_backend_marker="plugins/tls/libqopensslbackend.a"
  [[ -e "$TARGET_STAGING/$tls_backend_marker" ]] || return 1
  grep -Fqx '#define QT_FEATURE_ssl 1' "$TARGET_STAGING/include/QtNetwork/qtnetwork-config.h" || return 1
  grep -Fqx '#define QT_FEATURE_openssl_linked 1' "$TARGET_STAGING/include/QtCore/qconfig.h" || return 1
  [[ -d "$TARGET_STAGING/lib/cmake/Qt6WaylandClient" ]] || return 1
  return 0
}

build_target_qtbase() {
  if target_qtbase_up_to_date; then
    log "target qtbase: up to date, skipping"
    return 0
  fi
  if [[ ! -f "$TARGET_STAGING/.spool-no-testlib-v1" ]]; then
    log "target Qt profile changed; removing incompatible cached target prefix"
    clean_target
  fi
  TARGET_QTBASE_REBUILT=1
  configure_target_qtbase
  cmake_build "$TARGET_BUILD_ROOT/qtbase" --parallel "$JOBS"
  cmake_install "$TARGET_BUILD_ROOT/qtbase"
  printf 'shared\n' >"$TARGET_STAGING/.jellyfin-openssl-shared"
  prune_completed_build_dir "$TARGET_BUILD_ROOT/qtbase"
  printf '1\n' >"$TARGET_STAGING/.spool-no-testlib-v1"
}

configure_target_module() {
  local name="$1"
  local src="$2"
  local extra=("${@:3}")

  local build_type="Release"
  local module_flags=()
  local sdk_glesv2_lib sdk_egl_lib sdk_xkb_lib
  local sdk_wayland_client_lib sdk_wayland_server_lib sdk_wayland_cursor_lib sdk_wayland_egl_lib
  if [[ "$QT_STATIC" == "1" ]]; then
    build_type="MinSizeRel"
    module_flags=(
      -DBUILD_SHARED_LIBS=OFF
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
      "-DCMAKE_C_FLAGS_MINSIZEREL=$QT_TARGET_TUNE_CFLAGS -Os -ffunction-sections -fdata-sections -DNDEBUG"
      "-DCMAKE_CXX_FLAGS_MINSIZEREL=$QT_TARGET_TUNE_CFLAGS -Os -ffunction-sections -fdata-sections -DNDEBUG"
      "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--gc-sections"
      -DFEATURE_ltcg=ON
    )
  fi
  sdk_glesv2_lib="$(sdk_sysroot_library GLESv2)"
  sdk_egl_lib="$(sdk_sysroot_library EGL)"
  sdk_xkb_lib="$(sdk_sysroot_library xkbcommon)"
  sdk_wayland_client_lib="$(sdk_sysroot_library wayland-client)"
  sdk_wayland_server_lib="$(sdk_sysroot_library wayland-server)"
  sdk_wayland_cursor_lib="$(sdk_sysroot_library wayland-cursor)"
  sdk_wayland_egl_lib="$(sdk_sysroot_library wayland-egl)"

  maybe_clean_poisoned_build_dir "$TARGET_BUILD_ROOT/$name" "$src"
  log "Configuring target $name"
  cmake_configure -S "$src" -B "$TARGET_BUILD_ROOT/$name" -GNinja \
    "${fresh_flag[@]}" \
    "${cmake_registry_flags[@]}" \
    -DCMAKE_BUILD_TYPE="$build_type" \
    "${module_flags[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DPKG_CONFIG_EXECUTABLE="$PKG_CONFIG_WEBOS" \
    -DQT_HOST_PATH="$HOST_INSTALL" \
    -DQT_HOST_PATH_CMAKE_DIR="$HOST_INSTALL/lib/cmake" \
    -DCMAKE_STAGING_PREFIX="$TARGET_STAGING" \
    -DCMAKE_INSTALL_PREFIX="$TARGET_PREFIX" \
    -DCMAKE_PREFIX_PATH="$TARGET_STAGING" \
    -DQt6_DIR="$TARGET_STAGING/lib/cmake/Qt6" \
    -DBUILD_WITH_PCH="$QT_TARGET_BUILD_WITH_PCH" \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DQT_SKIP_AUTO_PLUGIN_INCLUSION=ON \
    -DQT_SKIP_AUTO_QML_PLUGIN_INCLUSION=ON \
    -DGLESv2_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DGLESv2_LIBRARY="$sdk_glesv2_lib" \
    -DEGL_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DEGL_LIBRARY="$sdk_egl_lib" \
    -DXKB_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DXKB_LIBRARY="$sdk_xkb_lib" \
    -DWayland_Client_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Client_LIBRARY="$sdk_wayland_client_lib" \
    -DWayland_Server_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Server_LIBRARY="$sdk_wayland_server_lib" \
    -DWayland_Cursor_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Cursor_LIBRARY="$sdk_wayland_cursor_lib" \
    -DWayland_Egl_INCLUDE_DIR="$SYSROOT/usr/include" \
    -DWayland_Egl_LIBRARY="$sdk_wayland_egl_lib" \
    "${extra[@]}"
}

build_target_module() {
  local name="$1"
  cmake_build "$TARGET_BUILD_ROOT/$name" --parallel "$JOBS"
  cmake_install "$TARGET_BUILD_ROOT/$name"
  prune_completed_build_dir "$TARGET_BUILD_ROOT/$name"
}

target_module_up_to_date() {
  local name="$1"
  local marker="$2"
  [[ "$TARGET_QTBASE_REBUILT" != "1" ]] || return 1
  [[ "${QT_BUILD_FORCE:-0}" != "1" ]] || return 1
  [[ ",${QT_BUILD_FORCE_MODULES:-}," != *",$name,"* ]] || return 1
  [[ -e "$TARGET_STAGING/$marker" ]] || return 1
  return 0
}

target_lib_marker() {
  local lib="$1"
  if [[ "$QT_STATIC" == "1" ]]; then
    printf 'lib/lib%s.a\n' "$lib"
  else
    printf 'lib/lib%s.so\n' "$lib"
  fi
}

build_all_target_modules() {
  require_target_sdk
  local target_wayland_scanner="${TARGET_WAYLAND_SCANNER:-$TARGET_WAYLAND_SCANNER_DEFAULT}"
  [[ -x "$target_wayland_scanner" ]] || \
    die "target wayland-scanner not found at $target_wayland_scanner; set TARGET_WAYLAND_SCANNER"

  if target_module_up_to_date qtshadertools "$(target_lib_marker Qt6ShaderTools)"; then
    log "target qtshadertools: up to date, skipping"
  else
    configure_target_module qtshadertools "$QTSHADERTOOLS_SRC"
    build_target_module qtshadertools
  fi

  if target_module_up_to_date qttools "lib/cmake/Qt6Tools/Qt6ToolsConfig.cmake"; then
    log "target qttools: up to date, skipping"
  else
    configure_target_module qttools "$QTTOOLS_SRC" \
      -DFEATURE_linguist=ON \
      -DFEATURE_assistant=OFF \
      -DFEATURE_designer=OFF \
      -DFEATURE_distancefieldgenerator=OFF \
      -DFEATURE_pixeltool=OFF \
      -DFEATURE_qdoc=OFF \
      -DFEATURE_qtattributionsscanner=OFF \
      -DFEATURE_qtdiag=OFF \
      -DFEATURE_qtplugininfo=OFF
    build_target_module qttools
  fi

  if target_module_up_to_date qtdeclarative "$(target_lib_marker Qt6Qml)"; then
    log "target qtdeclarative: up to date, skipping"
  else
    configure_target_module qtdeclarative "$QTDECLARATIVE_SRC" \
      -DQT_NO_TARGET_QMLTESTRUNNER=ON \
      -DFEATURE_quick_vectorimage=OFF
    build_target_module qtdeclarative
  fi

  if target_module_up_to_date qtwebsockets "$(target_lib_marker Qt6WebSockets)"; then
    log "target qtwebsockets: up to date, skipping"
  else
    configure_target_module qtwebsockets "$QTWEBSOCKETS_SRC"
    build_target_module qtwebsockets
  fi


  if target_module_up_to_date qtwayland "$(target_lib_marker Qt6WaylandClient)"; then
    log "target qtwayland: up to date, skipping"
  else
    configure_target_module qtwayland "$QTWAYLAND_SRC" \
      -DWaylandScanner_EXECUTABLE="$target_wayland_scanner"
    build_target_module qtwayland
  fi

  local imageformats_marker
  if [[ "$QT_STATIC" == "1" ]]; then
    imageformats_marker="plugins/imageformats/libqwebp.a"
  else
    imageformats_marker="plugins/imageformats/libqwebp.so"
  fi
  if target_module_up_to_date qtimageformats "$imageformats_marker"; then
    log "target qtimageformats: up to date, skipping"
  else
    configure_target_module qtimageformats "$QTIMAGEFORMATS_SRC"
    build_target_module qtimageformats
  fi

  local svg_marker
  if [[ "$QT_STATIC" == "1" ]]; then
    svg_marker="plugins/imageformats/libqsvg.a"
  else
    svg_marker="plugins/imageformats/libqsvg.so"
  fi
  if target_module_up_to_date qtsvg "$svg_marker"; then
    log "target qtsvg: up to date, skipping"
  else
    configure_target_module qtsvg "$QTSVG_SRC"
    build_target_module qtsvg
  fi


  if [[ "$QT_STATIC" == "1" && -z "${QT_BUILD_FORCE_MODULES:-}" ]]; then
    printf '1\n' >"$TARGET_STAGING/.jellyfin-static-size-profile-v1"
    printf '1\n' >"$TARGET_STAGING/.jellyfin-static-pic-profile-v1"
  fi

}

ensure_host() {
  build_host_qtbase
  build_all_host_modules
}

show_summary() {
  echo
  echo "Host install: $HOST_INSTALL"
  if [[ -d "$HOST_INSTALL/lib/cmake" ]]; then
    find "$HOST_INSTALL/lib/cmake" -maxdepth 1 -mindepth 1 -type d | sort | sed -n '1,100p'
  fi
  echo
  echo "Target staging install: $TARGET_STAGING"
  if [[ -d "$TARGET_STAGING/lib/cmake" ]]; then
    find "$TARGET_STAGING/lib/cmake" -maxdepth 1 -mindepth 1 -type d | sort | sed -n '1,140p'
  fi
}

usage() {
  cat >&2 <<USAGE_EOF
usage: $0 [fetch|host|target|all|clean-host|clean-target|clean|summary]

Environment knobs:
  QT_VERSION=$QT_VERSION
  QT_STATIC=0|1
  JOBS=N                       explicit parallel job count
  QT_BUILD_MEMORY_PER_JOB_MIB=1536
  QT_BUILD_MEMORY_RESERVE_MIB=2048
  QT_TARGET_BUILD_WITH_PCH=ON|OFF
  QT_BUILD_PRUNE_COMPLETED=0|1 remove completed Qt build trees (defaults to 1 in CI)
  QT_BUILD_FRESH=1              pass --fresh to CMake configure
  QT_BUILD_FORCE=1              ignore module install markers
  QT_BUILD_FORCE_MODULES=a,b    rebuild selected modules
  QT_BUILD_CLEAN_POISONED=0|1   auto-remove CMake caches that mention nixpkgs Qt
  WEBOS_SDK_ROOT=/path/to/arm-webos-linux-gnueabi_sdk-buildroot
USAGE_EOF
}

case "$PHASE" in
  fetch)
    fetch_sources
    ;;
  host)
    describe_parallel_jobs "$JOBS" "Qt build" "$QT_BUILD_MEMORY_PER_JOB_MIB" "$QT_BUILD_MEMORY_RESERVE_MIB"
    fetch_sources
    ensure_host
    ;;
  target)
    describe_parallel_jobs "$JOBS" "Qt build" "$QT_BUILD_MEMORY_PER_JOB_MIB" "$QT_BUILD_MEMORY_RESERVE_MIB"
    fetch_sources
    ensure_host
    build_target_qtbase
    build_all_target_modules
    ;;
  all)
    describe_parallel_jobs "$JOBS" "Qt build" "$QT_BUILD_MEMORY_PER_JOB_MIB" "$QT_BUILD_MEMORY_RESERVE_MIB"
    fetch_sources
    ensure_host
    build_target_qtbase
    build_all_target_modules
    show_summary
    ;;
  clean-host)
    clean_host
    ;;
  clean-target)
    clean_target
    ;;
  clean)
    clean_host
    clean_target
    ;;
  summary)
    show_summary
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage
    exit 1
    ;;
esac
