#!/usr/bin/env bash
set -euo pipefail

PHASE="${1:-all}"
if (( $# > 1 )); then echo "usage: $0 [fetch|build|all]" >&2; exit 2; fi
case "$PHASE" in
  -h|--help) echo "usage: $0 [fetch|build|all]"; exit 0 ;;
  fetch|build|all) ;;
  *) echo "usage: $0 [fetch|build|all]" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$ROOT/tools/lib/build-common.sh"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
# shellcheck source=tools/webos-native/nixos-sdk-compat.sh
source "$ROOT/tools/webos-native/nixos-sdk-compat.sh"
MANIFEST="${WEBOS_THIRD_PARTY_MANIFEST:-$ROOT/tools/manifests/webos-third-party.json}"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SDK_BIN="$SDK_ROOT/bin"
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
TARGET_PREFIX="${WEBOS_TARGET_PREFIX:-/usr/local/webos-native}"
PREFIX="${WEBOS_NATIVE_PREFIX:-$SYSROOT$TARGET_PREFIX}"
SRC_ROOT="${WEBOS_NATIVE_SRC_ROOT:-$ROOT/build/third_party}"
BUILD_ROOT="${WEBOS_NATIVE_BUILD_ROOT:-$ROOT/build/webos-thirdparty-build}"
CROSS_FILE="$BUILD_ROOT/webos.cross.ini"
WEBOS_BUILD_MEMORY_PER_JOB_MIB="${WEBOS_BUILD_MEMORY_PER_JOB_MIB:-1536}"
WEBOS_BUILD_MEMORY_RESERVE_MIB="${WEBOS_BUILD_MEMORY_RESERVE_MIB:-2048}"
WEBOS_BUILD_JOBS="$(recommended_parallel_jobs "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB")"

mkdir -p "$SRC_ROOT" "$BUILD_ROOT" "$PREFIX"

fetch_sources() {
  local path url sha archive

  prepare_manifest_source "$ROOT" "$MANIFEST" fribidi "$SRC_ROOT/fribidi"
  prepare_manifest_source "$ROOT" "$MANIFEST" harfbuzz "$SRC_ROOT/harfbuzz"
  prepare_manifest_source "$ROOT" "$MANIFEST" libass "$SRC_ROOT/libass"
  prepare_manifest_source "$ROOT" "$MANIFEST" libplacebo "$SRC_ROOT/libplacebo"
  prepare_manifest_source "$ROOT" "$MANIFEST" dovi_tool "$SRC_ROOT/dovi_tool"
  prepare_manifest_source "$ROOT" "$MANIFEST" lua "$SRC_ROOT/lua"

  while IFS=$'\t' read -r path url sha; do
    [[ -n "$path" ]] || continue
    archive="$ROOT/build/downloads/libplacebo-${path//\//-}-$sha.archive"
    download_verified "$url" "$sha" "$archive"
    extract_verified_source "$archive" "$sha" "$SRC_ROOT/libplacebo/$path"
  done < <(manifest_vendored_sources "$MANIFEST" libplacebo)
}

fetch_sources

if [[ "$PHASE" == "fetch" ]]; then
  echo "Fetched verified webOS third-party sources into $SRC_ROOT"
  exit 0
fi


describe_parallel_jobs "$WEBOS_BUILD_JOBS" "webOS third-party" "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB"

if [[ ! -x "$SDK_BIN/arm-webos-linux-gnueabi-gcc" ]]; then
  printf 'Missing webOS SDK compiler under %s\n' "$SDK_BIN" >&2
  exit 1
fi
ensure_webos_sdk_host_tools "$SDK_ROOT"

cat >"$CROSS_FILE" <<EOF
[binaries]
c = '$SDK_BIN/arm-webos-linux-gnueabi-gcc'
cpp = '$SDK_BIN/arm-webos-linux-gnueabi-g++'
ar = '$SDK_BIN/arm-webos-linux-gnueabi-ar'
strip = '$SDK_BIN/arm-webos-linux-gnueabi-strip'
ranlib = '$SDK_BIN/arm-webos-linux-gnueabi-ranlib'
pkg-config = '$SDK_BIN/pkg-config'

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = ['$PREFIX/lib/pkgconfig', '$SYSROOT/usr/lib/pkgconfig', '$SYSROOT/usr/share/pkgconfig']
needs_exe_wrapper = true

[host_machine]
system = 'linux'
cpu_family = 'arm'
cpu = 'armv7'
endian = 'little'
EOF

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"

build_meson() {
  local name="$1"
  local src_dir="$2"
  shift 2
  local build_dir="$BUILD_ROOT/$name"

  rm -rf "$build_dir"
  meson setup "$build_dir" "$src_dir" \
    --cross-file "$CROSS_FILE" \
    --prefix "$TARGET_PREFIX" \
    "$@"
  meson compile -C "$build_dir" -j "$WEBOS_BUILD_JOBS"
  DESTDIR="$SYSROOT" meson install -C "$build_dir"
}

rm -f "$SRC_ROOT/lua/src"/*.o "$SRC_ROOT/lua/src/liblua.a"
make -C "$SRC_ROOT/lua/src" \
  CC="$SDK_BIN/arm-webos-linux-gnueabi-gcc -std=gnu99" \
  AR="$SDK_BIN/arm-webos-linux-gnueabi-ar rcu" \
  RANLIB="$SDK_BIN/arm-webos-linux-gnueabi-ranlib" \
  MYCFLAGS="-fPIC" \
  SYSLIBS="-ldl -lm" \
  liblua.a
install -Dm644 "$SRC_ROOT/lua/src/liblua.a" "$PREFIX/lib/liblua.a"
for header in lua.h luaconf.h lualib.h lauxlib.h lua.hpp; do
  install -Dm644 "$SRC_ROOT/lua/src/$header" "$PREFIX/include/$header"
done
mkdir -p "$PREFIX/lib/pkgconfig"
cat >"$PREFIX/lib/pkgconfig/lua.pc" <<EOF
prefix=$TARGET_PREFIX
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: Lua
Description: Lua 5.2 scripting language
Version: 5.2.4
Libs: -L\${libdir} -llua -lm -ldl
Cflags: -I\${includedir}
EOF

build_meson fribidi "$SRC_ROOT/fribidi" \
  --default-library static \
  -Ddocs=false \
  -Dbin=false \
  -Dtests=false \
  --buildtype release

build_meson harfbuzz "$SRC_ROOT/harfbuzz" \
  --default-library static \
  -Dtests=disabled \
  -Dutilities=disabled \
  -Ddocs=disabled \
  -Dglib=disabled \
  -Dgobject=disabled \
  -Dcairo=disabled \
  -Dchafa=disabled \
  -Dpng=disabled \
  -Dzlib=disabled \
  -Dicu=disabled \
  -Dgraphite=disabled \
  -Dgraphite2=disabled \
  -Dfreetype=disabled \
  -Dfontations=disabled \
  -Dgdi=disabled \
  -Ddirectwrite=disabled \
  -Dcoretext=disabled \
  -Dharfrust=disabled \
  -Dkbts=disabled \
  -Dwasm=disabled \
  -Draster=disabled \
  -Dvector=disabled \
  -Dgpu=disabled \
  -Dsubset=disabled \
  --buildtype release

build_meson libass "$SRC_ROOT/libass" \
  --default-library static \
  -Dtest=disabled \
  -Dcompare=disabled \
  -Dprofile=disabled \
  -Dfuzz=disabled \
  -Dcheckasm=disabled \
  -Dfontconfig=enabled \
  -Dlibunibreak=disabled \
  -Dasm=disabled \
  --buildtype release

build_meson libplacebo "$SRC_ROOT/libplacebo" \
  --default-library static \
  -Ddemos=false \
  -Dtests=false \
  -Dbench=false \
  -Dfuzz=false \
  -Dvulkan=disabled \
  -Dvk-proc-addr=disabled \
  -Dd3d11=disabled \
  -Dshaderc=disabled \
  -Dglslang=disabled \
  -Dlcms=disabled \
  -Ddovi=disabled \
  -Dlibdovi=disabled \
  -Dxxhash=disabled \
  -Dopengl=enabled \
  -Dgl-proc-addr=enabled \
  --buildtype release

printf '\nInstalled target dependencies into %s\n' "$PREFIX"
