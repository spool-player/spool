#!/usr/bin/env bash
# Source after platform compiler/OpenSSL setup. No host/toolchain inference.
build_streaming_http() {
  local sources="$1" builds="$2" prefix="$3" shared="$4"
  shift 4
  local dependency
  for dependency in nghttp2 nghttp3 ngtcp2 curl; do
    cmake -S "$sources/$dependency" -B "$builds/$dependency" -GNinja \
      -DSTREAMING_DEPENDENCY="$dependency" -DSTREAMING_SHARED="$shared" \
      -C "$ROOT/tools/cmake/StreamingDependencies.cmake" \
      -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$prefix" "$@"
    cmake --build "$builds/$dependency" --parallel "${JOBS:-4}"
    cmake --install "$builds/$dependency"
  done
}
