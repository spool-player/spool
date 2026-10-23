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
QT_MANIFEST="${QT_MANIFEST:-$ROOT/tools/manifests/qt-webos-6.11.json}"
QT_STATIC="${QT_STATIC:-1}"
QT_VERSION="${QT_VERSION:-$(manifest_qt_field "$QT_MANIFEST" qtVersion)}"
QT_BUILD_TAG="${QT_VERSION%.*}"
QT_BUILD_TAG="${QT_BUILD_TAG//./}"

if [[ -n "${WEBOS_SDK_ROOT:-}" ]]; then
  SDK_ROOT="$WEBOS_SDK_ROOT"
elif [[ -d "$ROOT/../build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot" ]]; then
  SDK_ROOT="$ROOT/../build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
else
  SDK_ROOT="$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
fi

if [[ -n "${QT6_PREFIX:-}" ]]; then
  QT_TARGET_PREFIX="$QT6_PREFIX"
elif [[ "$QT_STATIC" == "1" ]]; then
  QT_TARGET_PREFIX="$ROOT/build/qt6-$QT_BUILD_TAG-target-static-install"
else
  QT_TARGET_PREFIX="$ROOT/build/qt6-$QT_BUILD_TAG-target-install"
fi

QT_HOST_PREFIX="${QT6_HOST_PREFIX:-$ROOT/build/qt6-$QT_BUILD_TAG-host-install}"
SOURCE_DIR="${QCORO_SOURCE_DIR:-$ROOT/build/third_party/qcoro}"
BUILD_DIR="${QCORO_BUILD_DIR:-$ROOT/build/webos-qcoro}"
TOOLCHAIN_FILE="$ROOT/tools/webos-native/qt6-webos-toolchain.cmake"
QCORO_BUILD_MEMORY_PER_JOB_MIB="${QCORO_BUILD_MEMORY_PER_JOB_MIB:-768}"
QCORO_BUILD_MEMORY_RESERVE_MIB="${QCORO_BUILD_MEMORY_RESERVE_MIB:-2048}"
QCORO_BUILD_JOBS="$(recommended_parallel_jobs \
  "$QCORO_BUILD_MEMORY_PER_JOB_MIB" "$QCORO_BUILD_MEMORY_RESERVE_MIB")"

prepare_manifest_source "$ROOT" "$MANIFEST" qcoro "$SOURCE_DIR"

if [[ "$PHASE" == "fetch" ]]; then
  echo "Fetched verified QCoro source into $SOURCE_DIR"
  exit 0
fi


if [[ ! -f "$QT_TARGET_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" ]]; then
  echo "error: target Qt prefix is missing at $QT_TARGET_PREFIX" >&2
  echo "       Run the webOS qt-target phase first." >&2
  exit 1
fi
if [[ ! -d "$QT_HOST_PREFIX" ]]; then
  echo "error: host Qt prefix is missing at $QT_HOST_PREFIX" >&2
  echo "       Run the webOS qt-host phase first." >&2
  exit 1
fi

ensure_webos_sdk_host_tools "$SDK_ROOT"
describe_parallel_jobs "$QCORO_BUILD_JOBS" "webOS QCoro" \
  "$QCORO_BUILD_MEMORY_PER_JOB_MIB" "$QCORO_BUILD_MEMORY_RESERVE_MIB"

shared_flag=OFF
if [[ "$QT_STATIC" != "1" ]]; then
  shared_flag=ON
fi

if [[ "${QCORO_BUILD_FRESH:-0}" == "1" ]]; then
  rm -rf "$BUILD_DIR"
elif [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
  if ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$SOURCE_DIR" "$BUILD_DIR/CMakeCache.txt" \
      || ! grep -Fqx "CMAKE_TOOLCHAIN_FILE:FILEPATH=$TOOLCHAIN_FILE" "$BUILD_DIR/CMakeCache.txt" \
      || ! grep -Eq "^Qt6_DIR:[^=]*=$QT_TARGET_PREFIX/lib/cmake/Qt6$" "$BUILD_DIR/CMakeCache.txt"; then
    echo "QCoro build cache targets a different source, toolchain, or Qt prefix; wiping" >&2
    rm -rf "$BUILD_DIR"
  fi
fi

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -GNinja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_STAGING_PREFIX="$QT_TARGET_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot;$QT_TARGET_PREFIX" \
  -DCMAKE_PREFIX_PATH="$QT_TARGET_PREFIX;$QT_TARGET_PREFIX/lib/cmake" \
  -DQt6_DIR="$QT_TARGET_PREFIX/lib/cmake/Qt6" \
  -DQT_HOST_PATH="$QT_HOST_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$QT_TARGET_PREFIX" \
  -DBUILD_SHARED_LIBS="$shared_flag" \
  -DBUILD_TESTING=OFF \
  -DQCORO_BUILD_EXAMPLES=OFF \
  -DQCORO_BUILD_TESTING=OFF \
  -DQCORO_WITH_QTDBUS=OFF \
  -DQCORO_WITH_QML=OFF \
  -DQCORO_WITH_QTNETWORK=ON \
  -DQCORO_WITH_QTQUICK=OFF \
  -DQCORO_WITH_QTTEST=OFF \
  -DQCORO_WITH_QTWEBSOCKETS=OFF \
  -DUSE_QT_VERSION=6

cmake --build "$BUILD_DIR" --parallel "$QCORO_BUILD_JOBS"
cmake --install "$BUILD_DIR"

test -f "$QT_TARGET_PREFIX/lib/cmake/QCoro6/QCoro6Config.cmake"
test -f "$QT_TARGET_PREFIX/lib/cmake/QCoro6Core/QCoro6CoreConfig.cmake"
test -f "$QT_TARGET_PREFIX/lib/cmake/QCoro6Network/QCoro6NetworkConfig.cmake"
echo "Installed QCoro for webOS into $QT_TARGET_PREFIX"
