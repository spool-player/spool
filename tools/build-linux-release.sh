#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
ensure_native_shell "$APP_ROOT" "$APP_ROOT/tools/build-linux-release.sh" "$@"
MPV_SRC="${MPV_SRC:-$APP_ROOT/mpv}"
BUILD_ROOT="${BUILD_ROOT:-$APP_ROOT/build/linux-release}"
MPV_BUILD="${MPV_BUILD:-$BUILD_ROOT/mpv}"
MPV_PREFIX="${MPV_PREFIX:-$BUILD_ROOT/mpv-prefix}"
APP_BUILD="${APP_BUILD:-$BUILD_ROOT/app}"
APP_INSTALL="${APP_INSTALL:-$BUILD_ROOT/install}"

setup_native_ccache "$APP_ROOT"
mkdir -p "$MPV_PREFIX" "$APP_BUILD"

native_mpv_args "$MPV_PREFIX" release linux
MPV_SETUP_ARGS=(
  "${MPV_NATIVE_ARGS[@]}"
)

mpv_meson_build "$MPV_SRC" "$MPV_BUILD" "${MPV_SETUP_ARGS[@]}"
prune_stale_mpv_libraries "$MPV_PREFIX" "$MPV_BUILD"

append_colon_path PKG_CONFIG_PATH "$MPV_PREFIX/lib/pkgconfig"
while IFS= read -r pc_dir; do
  append_colon_path PKG_CONFIG_PATH "$pc_dir"
done < <(find "$MPV_PREFIX/lib" -path '*/pkgconfig/mpv.pc' -exec dirname {} \;)

rm -rf "$APP_INSTALL"
mkdir -p "$APP_INSTALL"

cmake_build_app "$APP_ROOT" "$APP_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSPOOL_WEBOS=OFF \
  -DSPOOL_DEV_BUILD=OFF \
  -DCMAKE_PREFIX_PATH="$MPV_PREFIX${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}" \
  -DCMAKE_INSTALL_PREFIX="$APP_INSTALL"
stage_elf_shared_library "$MPV_PREFIX/lib/libmpv.so*" "$APP_INSTALL/lib" "${READELF:-readelf}" >/dev/null

printf '%s\n' "$APP_INSTALL/bin/spool"
