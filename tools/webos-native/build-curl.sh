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
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
TARGET_PREFIX="${WEBOS_TARGET_PREFIX:-/usr/local/webos-native}"
PREFIX="${WEBOS_NATIVE_PREFIX:-$SYSROOT$TARGET_PREFIX}"
SRC_DIR="${CURL_SOURCE_DIR:-$ROOT/build/third_party/curl}"
BUILD_DIR="${CURL_BUILD_DIR:-$ROOT/build/webos-curl}"
TOOLCHAIN_FILE="$ROOT/tools/webos-native/qt6-webos-toolchain.cmake"

prepare_manifest_source "$ROOT" "$MANIFEST" curl "$SRC_DIR"

if [[ "$PHASE" == "fetch" ]]; then
  echo "Fetched verified curl source into $SRC_DIR"
  exit 0
fi

ensure_webos_sdk_host_tools "$SDK_ROOT"
rm -rf "$BUILD_DIR"
rm -f "$PREFIX/lib/libcurl.a" "$PREFIX/lib/libcurl.so"*

cmake -S "$SRC_DIR" -B "$BUILD_DIR" -GNinja \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DCMAKE_INSTALL_PREFIX="$TARGET_PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DOPENSSL_USE_STATIC_LIBS=OFF \
  -DCMAKE_C_FLAGS="$(webos_tune_cflags)" \
  -DBUILD_CURL_EXE=OFF \
  -DBUILD_LIBCURL_DOCS=OFF \
  -DBUILD_MISC_DOCS=OFF \
  -DBUILD_EXAMPLES=OFF \
  -DBUILD_SHARED_LIBS=ON \
  -DBUILD_STATIC_LIBS=OFF \
  -DBUILD_TESTING=OFF \
  -DCURL_BROTLI=OFF \
  -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt \
  -DCURL_DISABLE_ALTSVC=ON \
  -DCURL_DISABLE_DICT=ON \
  -DCURL_DISABLE_DOH=ON \
  -DCURL_DISABLE_FILE=ON \
  -DCURL_DISABLE_FTP=ON \
  -DCURL_DISABLE_GOPHER=ON \
  -DCURL_DISABLE_HSTS=ON \
  -DCURL_DISABLE_IMAP=ON \
  -DCURL_DISABLE_IPFS=ON \
  -DCURL_DISABLE_LDAP=ON \
  -DCURL_DISABLE_LDAPS=ON \
  -DCURL_DISABLE_MQTT=ON \
  -DCURL_DISABLE_NETRC=ON \
  -DCURL_DISABLE_POP3=ON \
  -DCURL_DISABLE_RTSP=ON \
  -DCURL_DISABLE_SMB=ON \
  -DCURL_DISABLE_SMTP=ON \
  -DCURL_DISABLE_TELNET=ON \
  -DCURL_DISABLE_TFTP=ON \
  -DCURL_DISABLE_WEBSOCKETS=ON \
  -DCURL_LTO=ON \
  -DCURL_USE_GSSAPI=OFF \
  -DCURL_USE_LIBPSL=OFF \
  -DCURL_USE_LIBSSH2=OFF \
  -DCURL_USE_OPENSSL=ON \
  -DCURL_ZLIB=ON \
  -DENABLE_ARES=OFF \
  -DENABLE_THREADED_RESOLVER=ON \
  -DENABLE_CURL_MANUAL=OFF \
  -DUSE_LIBIDN2=OFF \
  -DUSE_NGHTTP2=OFF \
  -DUSE_NGTCP2=OFF \
  -DUSE_NGHTTP3=OFF

cmake --build "$BUILD_DIR" --parallel "$(recommended_parallel_jobs 512 1024)"
DESTDIR="$SYSROOT" cmake --install "$BUILD_DIR"

[[ -e "$PREFIX/lib/libcurl.so" ]] || {
  echo "error: shared curl install missing at $PREFIX/lib/libcurl.so" >&2
  exit 1
}
[[ ! -e "$PREFIX/lib/libcurl.a" ]] || {
  echo "error: private curl build unexpectedly installed a static library" >&2
  exit 1
}

printf '\nInstalled private shared curl into %s\n' "$PREFIX"
