#!/usr/bin/env bash
set -euo pipefail

if [[ $# == 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  echo "Usage: $0"
  echo 'Fetches pinned FFmpeg and makes a fresh webOS dependency build.'
  exit 0
fi
(( $# == 0 )) || { echo "Usage: $0" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$ROOT/tools/lib/build-common.sh"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SDK_BIN="$SDK_ROOT/bin"
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
TARGET_PREFIX="${WEBOS_TARGET_PREFIX:-/usr/local/webos-native}"
PREFIX="${WEBOS_NATIVE_PREFIX:-$SYSROOT$TARGET_PREFIX}"
FFMPEG_VERSION="${FFMPEG_VERSION:-$(toolchain_field "$ROOT" ffmpeg.version)}"
FFMPEG_URL="${FFMPEG_URL:-$(toolchain_field "$ROOT" ffmpeg.url)}"
FFMPEG_SHA256="${FFMPEG_SHA256:-$(toolchain_field "$ROOT" ffmpeg.sha256)}"
SRC_ARCHIVE="${FFMPEG_ARCHIVE:-$ROOT/build/downloads/ffmpeg-$FFMPEG_VERSION.tar.xz}"
SRC_DIR="${FFMPEG_SRC_DIR:-$ROOT/build/ffmpeg-src}"
BUILD_DIR="${FFMPEG_BUILD_DIR:-$ROOT/build/ffmpeg-build}"
WEBOS_BUILD_MEMORY_PER_JOB_MIB="${WEBOS_BUILD_MEMORY_PER_JOB_MIB:-1536}"
WEBOS_BUILD_MEMORY_RESERVE_MIB="${WEBOS_BUILD_MEMORY_RESERVE_MIB:-2048}"
WEBOS_BUILD_JOBS="$(recommended_parallel_jobs "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB")"
FFMPEG_PGO_FLAGS="$(webos_pgo_flags FFMPEG "$ROOT/build/pgo/ffmpeg")"
FFMPEG_CAPABILITY_MANIFEST="${FFMPEG_CAPABILITY_MANIFEST:-$ROOT/tools/manifests/ffmpeg-capabilities.json}"
# Keep unwind tables and debug information out of release libraries unless an
# instrumented build explicitly supplies FFMPEG_DIAG_CFLAGS.
FFMPEG_DIAG_CFLAGS="${FFMPEG_DIAG_CFLAGS:-}"

mkdir -p "$ROOT/build"

download_verified "$FFMPEG_URL" "$FFMPEG_SHA256" "$SRC_ARCHIVE"

rm -rf "$SRC_DIR" "$BUILD_DIR"
mkdir -p "$SRC_DIR" "$BUILD_DIR"
tar -xf "$SRC_ARCHIVE" -C "$SRC_DIR" --strip-components=1

export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"

cd "$SRC_DIR"
describe_parallel_jobs "$WEBOS_BUILD_JOBS" "FFmpeg" "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB"
python3 "$ROOT/tools/ffmpeg-capabilities.py" --manifest "$FFMPEG_CAPABILITY_MANIFEST" validate
mapfile -t FFMPEG_FEATURE_FLAGS < <(
  python3 "$ROOT/tools/ffmpeg-capabilities.py" --manifest "$FFMPEG_CAPABILITY_MANIFEST" \
    configure --platform webos
)


./configure \
  "${FFMPEG_FEATURE_FLAGS[@]}" \
  --prefix="$TARGET_PREFIX" \
  --arch=arm \
  --cpu=cortex-a53 \
  --enable-thumb \
  --target-os=linux \
  --enable-cross-compile \
  --cross-prefix="$SDK_BIN/arm-webos-linux-gnueabi-" \
  --cc="$SDK_BIN/arm-webos-linux-gnueabi-gcc" \
  --cxx="$SDK_BIN/arm-webos-linux-gnueabi-g++" \
  --ar="$SDK_BIN/arm-webos-linux-gnueabi-gcc-ar" \
  --ranlib="$SDK_BIN/arm-webos-linux-gnueabi-gcc-ranlib" \
  --strip="$SDK_BIN/arm-webos-linux-gnueabi-strip" \
  --pkg-config="$SDK_BIN/pkg-config" \
  --disable-static \
  --enable-shared \
  --enable-pic \
  --enable-lto \
  --extra-cflags="--sysroot=$SYSROOT -I$PREFIX/include $(webos_tune_cflags) $FFMPEG_DIAG_CFLAGS $FFMPEG_PGO_FLAGS" \
  --extra-ldflags="--sysroot=$SYSROOT -L$PREFIX/lib -Wl,-rpath-link,$PREFIX/lib $FFMPEG_PGO_FLAGS"

python3 "$ROOT/tools/ffmpeg-capabilities.py" --manifest "$FFMPEG_CAPABILITY_MANIFEST" \
  audit-config --platform webos ffbuild/config.log config_components.h
cp -f ffbuild/config.log "$BUILD_DIR/config.log"

grep -q "^license='LGPL version 2.1 or later'$" ffbuild/config.log || {
  echo "error: FFmpeg did not configure as LGPL-2.1-or-later" >&2
  exit 1
}

make -j"$WEBOS_BUILD_JOBS"
make DESTDIR="$SYSROOT" install

printf '\nInstalled FFmpeg into %s\n' "$PREFIX"
