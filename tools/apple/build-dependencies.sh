#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/lib/manifest-sources.sh"
[[ "$(uname -s)" == Darwin ]] || { echo 'Apple builds require macOS and Xcode.' >&2; exit 1; }
APPLE_SYSTEM="${APPLE_SYSTEM:-iOS}"
APPLE_SDK="${APPLE_SDK:-iphoneos}"
APPLE_ARCH="${APPLE_ARCH:-arm64}"
APPLE_DEPLOYMENT_TARGET="${APPLE_DEPLOYMENT_TARGET:-16.0}"
case "$APPLE_SYSTEM:$APPLE_SDK" in
  iOS:iphoneos) target_os=ios ;;
  iOS:iphonesimulator) target_os=ios; simulator=-simulator ;;
  tvOS:appletvos) target_os=tvos ;;
  tvOS:appletvsimulator) target_os=tvos; simulator=-simulator ;;
  *) echo 'Invalid Apple system/SDK pair.' >&2; exit 2 ;;
esac
case "$APPLE_ARCH" in arm64) cpu_family=aarch64; ffmpeg_arch=aarch64 ;; x86_64) cpu_family=x86_64; ffmpeg_arch=x86_64 ;; *) exit 2 ;; esac
: "${QT_TARGET_PREFIX:?Set QT_TARGET_PREFIX to the target Qt kit}"
: "${QT_HOST_PATH:?Set QT_HOST_PATH to the matching host Qt kit}"
SDKROOT="$(xcrun --sdk "$APPLE_SDK" --show-sdk-path)"
CC="$(xcrun --sdk "$APPLE_SDK" --find clang)"
CXX="$(xcrun --sdk "$APPLE_SDK" --find clang++)"
AR="$(xcrun --sdk "$APPLE_SDK" --find ar)"
RANLIB="$(xcrun --sdk "$APPLE_SDK" --find ranlib)"
STRIP="$(xcrun --sdk "$APPLE_SDK" --find strip)"
TRIPLE="$APPLE_ARCH-apple-$target_os$APPLE_DEPLOYMENT_TARGET${simulator:-}"
BASE="$ROOT/build/apple/$APPLE_SDK-$APPLE_ARCH"
export MACOS_SDK="$SDKROOT"
export MACOS_SDK_VERSION="$(xcrun --sdk "$APPLE_SDK" --show-sdk-version)"
PREFIX="${APPLE_DEPS_PREFIX:-$BASE/prefix}"
SOURCE_ROOT="$ROOT/build/apple/sources"
BUILD_ROOT="$BASE/dependencies"
MANIFEST="$ROOT/tools/manifests/android-third-party.json"
JOBS="${BUILD_JOBS:-$(sysctl -n hw.logicalcpu)}"
mkdir -p "$SOURCE_ROOT" "$BUILD_ROOT" "$PREFIX"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
export CFLAGS="-target $TRIPLE -isysroot $SDKROOT -fPIC"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-target $TRIPLE -isysroot $SDKROOT -L$PREFIX/lib"
export CC CXX AR RANLIB SDKROOT
for name in openssl curl freetype qcoro fribidi harfbuzz libass libplacebo; do
  prepare_manifest_source "$ROOT" "$MANIFEST" "$name" "$SOURCE_ROOT/$name"
done
prepare_toolchain_ffmpeg "$ROOT" "$SOURCE_ROOT/ffmpeg"
while IFS=$'\t' read -r path url sha; do
  archive="$ROOT/build/downloads/apple-libplacebo-${path//\//-}-$sha.archive"
  download_verified "$url" "$sha" "$archive"
  extract_verified_source "$archive" "$sha" "$SOURCE_ROOT/libplacebo/$path"
done < <(manifest_vendored_sources "$MANIFEST" libplacebo)
CROSS_FILE="$BUILD_ROOT/apple.ini"
cat >"$CROSS_FILE" <<EOF
[binaries]
c = ['$CC', '-target', '$TRIPLE', '-isysroot', '$SDKROOT']
cpp = ['$CXX', '-target', '$TRIPLE', '-isysroot', '$SDKROOT']
objc = ['$CC', '-target', '$TRIPLE', '-isysroot', '$SDKROOT']
objcpp = ['$CXX', '-target', '$TRIPLE', '-isysroot', '$SDKROOT']
ar = '$AR'
strip = '$STRIP'
pkg-config = 'pkg-config'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$PREFIX/lib/pkgconfig']
[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
c_link_args = ['-lc++']
cpp_link_args = ['-lc++']
[host_machine]
system = 'darwin'
cpu_family = '$cpu_family'
cpu = '$APPLE_ARCH'
endian = 'little'
EOF
NATIVE_FILE="$BUILD_ROOT/native.ini"
NATIVE_CC="$(xcrun --sdk macosx --find clang)"
NATIVE_CXX="$(xcrun --sdk macosx --find clang++)"
NATIVE_SDK="$(xcrun --sdk macosx --show-sdk-path)"
cat >"$NATIVE_FILE" <<EOF
[binaries]
c = ['$NATIVE_CC', '-isysroot', '$NATIVE_SDK']
cpp = ['$NATIVE_CXX', '-isysroot', '$NATIVE_SDK']
[built-in options]
c_args = []
cpp_args = []
c_link_args = []
cpp_link_args = []
EOF
cmake_cross=(-DCMAKE_SYSTEM_NAME="$APPLE_SYSTEM" -DCMAKE_OSX_SYSROOT="$APPLE_SDK"
  -DCMAKE_OSX_ARCHITECTURES="$APPLE_ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET="$APPLE_DEPLOYMENT_TARGET"
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX"
  -DCMAKE_FIND_ROOT_PATH="$PREFIX;$SDKROOT" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
# Isolated build directories retain toolchain flags; source pins are verified
# above. Reconfigure every dependency rather than trusting stale install stamps.
openssl_build="$BUILD_ROOT/openssl"
mkdir -p "$openssl_build"
export CROSS_TOP="$(dirname "$SDKROOT")"
export CROSS_SDK="$(basename "$SDKROOT")"
case "$APPLE_SDK:$APPLE_ARCH" in
  iphoneos:arm64|appletvos:arm64) openssl_target=ios64-xcrun ;;
  iphonesimulator:arm64|appletvsimulator:arm64) openssl_target=iossimulator-arm64-xcrun ;;
  iphonesimulator:x86_64|appletvsimulator:x86_64) openssl_target=iossimulator-xcrun ;;
  *) echo 'Unsupported OpenSSL Apple target.' >&2; exit 2 ;;
esac
(cd "$openssl_build"; "$SOURCE_ROOT/openssl/Configure" "$openssl_target" no-shared no-tests no-asm --prefix="$PREFIX"; make -j"$JOBS"; make install_sw)
cmake -S "$SOURCE_ROOT/curl" -B "$BUILD_ROOT/curl" -GNinja "${cmake_cross[@]}" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
  -DCURL_USE_OPENSSL=ON -DCURL_USE_SECTRUST=ON -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DOPENSSL_INCLUDE_DIR="$PREFIX/include" -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.a" \
  -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.a" \
  -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF
cmake --build "$BUILD_ROOT/curl" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/curl"
cmake -S "$SOURCE_ROOT/freetype" -B "$BUILD_ROOT/freetype" -GNinja "${cmake_cross[@]}" \
  -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
cmake --build "$BUILD_ROOT/freetype" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/freetype"
meson_build() {
  local name="$1"; shift
  local build="$BUILD_ROOT/$name"
  if [[ -f "$build/meson-private/coredata.dat" ]]; then
    meson setup --reconfigure "$build" "$SOURCE_ROOT/$name" --cross-file "$CROSS_FILE" --native-file "$NATIVE_FILE" --prefix "$PREFIX" --default-library static "$@"
  else
    meson setup "$build" "$SOURCE_ROOT/$name" --cross-file "$CROSS_FILE" --native-file "$NATIVE_FILE" --prefix "$PREFIX" --default-library static "$@"
  fi
  meson compile -C "$build" -j "$JOBS"
  meson install -C "$build"
}
meson_build fribidi -Ddocs=false -Dbin=false -Dtests=false
meson_build harfbuzz -Dtests=disabled -Dutilities=disabled -Ddocs=disabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled -Dicu=disabled -Dfreetype=enabled -Dcoretext=enabled
meson_build libass -Dtest=disabled -Dcompare=disabled -Dprofile=disabled -Dfuzz=disabled -Dfontconfig=disabled -Dlibunibreak=disabled -Dasm=disabled
meson_build libplacebo -Ddemos=false -Dtests=false -Dbench=false -Dfuzz=false -Dvulkan=disabled -Dvk-proc-addr=disabled -Dd3d11=disabled -Dshaderc=disabled -Dglslang=disabled -Dlcms=disabled -Ddovi=disabled -Dlibdovi=disabled -Dxxhash=disabled -Dopengl=enabled -Dgl-proc-addr=enabled
ffmpeg_build="$BUILD_ROOT/ffmpeg"
mkdir -p "$ffmpeg_build"
python3 "$ROOT/tools/ffmpeg-capabilities.py" configure --platform macos >"$ffmpeg_build/flags"
feature_flags=()
while IFS= read -r flag; do feature_flags+=("$flag"); done <"$ffmpeg_build/flags"
(cd "$ffmpeg_build"; "$SOURCE_ROOT/ffmpeg/configure" --prefix="$PREFIX" --target-os=darwin --arch="$ffmpeg_arch" \
  --enable-cross-compile --cc="$CC" --cxx="$CXX" --ar="$AR" --ranlib="$RANLIB" --sysroot="$SDKROOT" \
  --enable-static --disable-shared --enable-pic --pkg-config=pkg-config \
  --extra-cflags="$CFLAGS -I$PREFIX/include" --extra-ldflags="$LDFLAGS" "${feature_flags[@]}"; make -j"$JOBS"; make install)
python3 "$ROOT/tools/ffmpeg-capabilities.py" audit-config --platform macos "$ffmpeg_build/config.h"
python3 "$ROOT/tools/ffmpeg-capabilities.py" audit-components --platform macos "$ffmpeg_build/config_components.h"
mpv_build="$BUILD_ROOT/mpv"
mpv_args=(--cross-file "$CROSS_FILE" --native-file "$NATIVE_FILE" --prefix "$PREFIX" --default-library static --buildtype release
  -Dcplayer=false -Dlibmpv=true -Dbuild-date=false -Dtests=false -Dlua=disabled -Djavascript=disabled
  -Dmanpage-build=disabled -Dlibarchive=disabled -Dlibbluray=disabled -Dlibcurl=enabled -Dgl=enabled
  -Dios-gl=enabled -Dvideotoolbox-gl=disabled -Dvideotoolbox-pl=disabled -Dvulkan=disabled
  -Dcocoa=disabled -Dcoreaudio=disabled -Davfoundation=disabled -Daudiounit=enabled
  -Dgl-cocoa=disabled)
if [[ -f "$mpv_build/meson-private/coredata.dat" ]]; then
  meson setup --reconfigure "$mpv_build" "$ROOT/mpv" "${mpv_args[@]}"
else
  meson setup "$mpv_build" "$ROOT/mpv" "${mpv_args[@]}"
fi
meson compile -C "$mpv_build" -j "$JOBS"
meson install -C "$mpv_build"
cmake -S "$SOURCE_ROOT/qcoro" -B "$BUILD_ROOT/qcoro" -GNinja "${cmake_cross[@]}" \
  -DCMAKE_TOOLCHAIN_FILE="$QT_TARGET_PREFIX/lib/cmake/Qt6/qt.toolchain.cmake" -DQT_HOST_PATH="$QT_HOST_PATH" \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DQCORO_BUILD_EXAMPLES=OFF -DQCORO_WITH_QTDBUS=OFF
cmake --build "$BUILD_ROOT/qcoro" --parallel "$JOBS"
cmake --install "$BUILD_ROOT/qcoro"
printf 'Apple media prefix: %s\n' "$PREFIX"
