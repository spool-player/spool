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
source "$ROOT/tools/lib/build-common.sh"
source "$ROOT/tools/webos-native/nixos-sdk-compat.sh"
source "$ROOT/tools/lib/streaming-build.sh"
SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
TARGET_PREFIX="${WEBOS_TARGET_PREFIX:-/usr/local/webos-native}"
PREFIX="${WEBOS_NATIVE_PREFIX:-$SYSROOT$TARGET_PREFIX}"
SRC_ROOT="${STREAMING_SOURCE_DIR:-$ROOT/build/streaming-sources}"
BUILD_ROOT="${STREAMING_BUILD_DIR:-$ROOT/build/webos-streaming}"
JOBS="$(recommended_parallel_jobs 512 1024)"
bash "$ROOT/tools/prepare-streaming-sources.sh" --root "$ROOT" --output "$SRC_ROOT"
[[ "$PHASE" != fetch ]] || exit 0
ensure_webos_sdk_host_tools "$SDK_ROOT"
export PATH="$SDK_ROOT/bin:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
pin="$(python3 "$ROOT/tools/streaming-fingerprint.py" --root "$ROOT" --target "webos:$("$SDK_ROOT/bin/arm-webos-linux-gnueabi-gcc" --version):shared" --wrapper "${BASH_SOURCE[0]}" --wrapper "$ROOT/tools/lib/streaming-build.sh")"
stamp="$PREFIX/lib/.spool-streaming-configuration"
if [[ -f "$stamp" && "$(<"$stamp")" == "$pin" && -f "$PREFIX/lib/libcurl.so" ]]; then exit 0; fi
rm -rf "$BUILD_ROOT"
mkdir -p "$BUILD_ROOT/openssl"
(
  cd "$BUILD_ROOT/openssl"
  "$SRC_ROOT/openssl/Configure" linux-armv4 shared \
    no-apps no-docs no-tests no-comp no-dtls no-srp no-ssl3 no-weak-ssl-ciphers \
    --prefix="$TARGET_PREFIX" --libdir=lib --cross-compile-prefix=arm-webos-linux-gnueabi- \
    "$(webos_tune_cflags)"
  make -j"$JOBS"
  make DESTDIR="$SYSROOT" install_sw
)
export DESTDIR="$SYSROOT"
build_streaming_http "$SRC_ROOT" "$BUILD_ROOT" "$TARGET_PREFIX" ON \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/webos-native/qt6-webos-toolchain.cmake" \
  -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_C_FLAGS="$(webos_tune_cflags)" \
  -DOPENSSL_ROOT_DIR="$PREFIX" -DOPENSSL_INCLUDE_DIR="$PREFIX/include" \
  -DOPENSSL_SSL_LIBRARY="$PREFIX/lib/libssl.so" -DOPENSSL_CRYPTO_LIBRARY="$PREFIX/lib/libcrypto.so" \
  -DOPENSSL_USE_STATIC_LIBS=OFF -DCURL_ZLIB=ON -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
python3 "$ROOT/tools/check-streaming-dependencies.py" --prefix "$PREFIX" --manifest "$ROOT/tools/manifests/streaming-third-party.json"
printf '%s\n' "$pin" >"$stamp"
