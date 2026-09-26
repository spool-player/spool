#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "${IN_NIX_SHELL:-}" && "${SPOOL_BUILD_IPK_ENTERED_NIX:-0}" != "1" ]]; then
  WORKSPACE_ROOT="$(cd "$ROOT/.." && pwd)"
  DEFAULT_SDK_ROOT="$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
  if [[ -z "${WEBOS_SDK_ROOT:-}" && -d "$WORKSPACE_ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot" ]]; then
    DEFAULT_SDK_ROOT="$WORKSPACE_ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot"
  fi
  cd "$ROOT"
  exec nix develop "$ROOT" -c env \
    SPOOL_BUILD_IPK_ENTERED_NIX=1 \
    WEBOS_SDK_ROOT="${WEBOS_SDK_ROOT:-$DEFAULT_SDK_ROOT}" \
    bash -c 'cd "$1" && shift && exec bash ./build-ipk.sh "$@"' bash "$ROOT" "$@"
fi

WEBOS_TOOLS_ROOT="${WEBOS_TOOLS_ROOT:-$ROOT/tools/webos-native}"
# shellcheck source=tools/lib/build-common.sh
source "$ROOT/tools/lib/build-common.sh"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"
# shellcheck source=tools/webos-native/nixos-sdk-compat.sh
source "$ROOT/tools/webos-native/nixos-sdk-compat.sh"

if (( $# == 0 )); then
  exec "$ROOT/tools/webos-native/build-webos.sh" all
fi
PHASE="$1"
DO_BUILD=0
DO_STAGE=0
DO_PACKAGE=0
case "$PHASE" in
  app) DO_BUILD=1 ;;
  stage) DO_STAGE=1 ;;
  package) DO_PACKAGE=1 ;;
  all)
    DO_BUILD=1
    DO_STAGE=1
    DO_PACKAGE=1
    ;;
  *)
    echo "usage: $0 [app|stage|package|all]" >&2
    exit 2
    ;;
esac

SDK_ROOT="${WEBOS_SDK_ROOT:-$ROOT/build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
SYSROOT="$SDK_ROOT/arm-webos-linux-gnueabi/sysroot"
PREFIX="$SYSROOT/usr/local/webos-native"
MPV_SRC="${MPV_SRC:-$ROOT/mpv}"
MPV_BUILD="${MPV_BUILD:-$MPV_SRC/build/webos-libmpv}"
WEBOS_CROSS_FILE="${WEBOS_CROSS_FILE:-$ROOT/build/webos-thirdparty-build/webos.cross.ini}"
FFMPEG_CAPABILITY_MANIFEST="${FFMPEG_CAPABILITY_MANIFEST:-$ROOT/tools/manifests/ffmpeg-capabilities.json}"
QT6_HOST_PREFIX="${QT6_HOST_PREFIX:-$ROOT/build/qt6-611-host-install}"
APP_SOURCE_DIR="$ROOT/app"
BUILD_DIR="$ROOT/build"
CMAKE_BUILD_DIR="${WEBOS_APP_BUILD_DIR:-$ROOT/build/webos/app}"
INSTALL_DIR="$CMAKE_BUILD_DIR/install"
APP_DIR="${WEBOS_STAGE_DIR:-$ROOT/build/webos/stage/app}"
STAGE_LIB="$APP_DIR/lib"
STAGE_BIN="$APP_DIR/bin"
STRIP_BIN="$SDK_ROOT/bin/arm-webos-linux-gnueabi-strip"
READELF_BIN="${WEBOS_READELF_BIN:-$SDK_ROOT/bin/arm-webos-linux-gnueabi-readelf}"
OBJDUMP_BIN="${WEBOS_OBJDUMP_BIN:-$SDK_ROOT/bin/arm-webos-linux-gnueabi-objdump}"
GCC_AR_BIN="$SDK_ROOT/bin/arm-webos-linux-gnueabi-gcc-ar"
GCC_RANLIB_BIN="$SDK_ROOT/bin/arm-webos-linux-gnueabi-gcc-ranlib"
MPV_LTO_CROSS_FILE="$BUILD_DIR/webos-lto.cross.ini"
WEBOS_BUILD_MEMORY_PER_JOB_MIB="${WEBOS_BUILD_MEMORY_PER_JOB_MIB:-1536}"
WEBOS_BUILD_MEMORY_RESERVE_MIB="${WEBOS_BUILD_MEMORY_RESERVE_MIB:-2048}"
WEBOS_BUILD_JOBS="$(recommended_parallel_jobs "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB")"
IMAGE_SUBTITLE_DIAGNOSTICS="${SPOOL_IMAGE_SUBTITLE_DIAGNOSTICS:-0}"
if [[ "$IMAGE_SUBTITLE_DIAGNOSTICS" == "0" ]]; then
  MPV_IMAGE_SUBTITLE_DIAGNOSTICS_FLAG=
  CMAKE_IMAGE_SUBTITLE_DIAGNOSTICS=OFF
else
  MPV_IMAGE_SUBTITLE_DIAGNOSTICS_FLAG=-DMP_IMAGE_SUBTITLE_DIAGNOSTICS=1
  CMAKE_IMAGE_SUBTITLE_DIAGNOSTICS=ON
fi


QT6_PREFIX="${QT6_PREFIX:-$ROOT/build/qt6-611-target-static-install}"
if (( DO_BUILD || DO_STAGE )); then
  WEBOS_SDK_ROOT="$SDK_ROOT" "$WEBOS_TOOLS_ROOT/restore-toolchain.sh"
  ensure_webos_sdk_host_tools "$SDK_ROOT"

  if [[ ! -f "$QT6_PREFIX/lib/libQt6Core.a" ]]; then
    echo "error: static Qt6 install not found at $QT6_PREFIX" >&2
    echo "       Run: QT_STATIC=1 bash $WEBOS_TOOLS_ROOT/build-qt6-611.sh target" >&2
    exit 1
  fi
  echo "Using static Qt6 build at $QT6_PREFIX"

  # Reject stale cached Qt prefixes that predate required local patches.
  QT_PATCH_MARKERS=(SPOOL_QT_NO_CURSOR_SURFACE)
  for marker in "${QT_PATCH_MARKERS[@]}"; do
    if ! grep -rqal "$marker" "$QT6_PREFIX/lib" 2>/dev/null; then
      echo "error: Qt at $QT6_PREFIX is missing patch marker '$marker'." >&2
      echo "       Rerun: ./build-ipk.sh" >&2
      exit 1
    fi
  done

  QT_NETWORK_CONFIG="$QT6_PREFIX/include/QtNetwork/qtnetwork-config.h"
  QT_CORE_CONFIG="$QT6_PREFIX/include/QtCore/qconfig.h"
  if ! grep -Fqx '#define QT_FEATURE_ssl 1' "$QT_NETWORK_CONFIG" 2>/dev/null \
      || ! grep -Fqx '#define QT_FEATURE_openssl_linked 1' "$QT_CORE_CONFIG" 2>/dev/null; then
    echo "error: Qt at $QT6_PREFIX was built without linked TLS support." >&2
    echo "       Rerun: QT_STATIC=1 bash $WEBOS_TOOLS_ROOT/build-qt6-611.sh target" >&2
    exit 1
  fi

  #QT_GUI_CONFIG="$QT6_PREFIX/include/QtGui/qtgui-config.h"
  #if ! grep -q '^#define QT_FEATURE_accessibility 1$' "$QT_GUI_CONFIG" 2>/dev/null; then
  #  echo "error: Qt at $QT6_PREFIX was built without accessibility support." >&2
  #  echo "       Rerun: bash $WEBOS_TOOLS_ROOT/build-qt6-611.sh" >&2
  #  exit 1
  #fi

  if [[ ! -f "$QT6_PREFIX/lib/cmake/QCoro6/QCoro6Config.cmake" ]]; then
    echo "error: target QCoro is missing from $QT6_PREFIX." >&2
    echo "       Run: bash $WEBOS_TOOLS_ROOT/build-qcoro.sh" >&2
    exit 1
  fi
fi

if (( DO_BUILD )); then
describe_parallel_jobs "$WEBOS_BUILD_JOBS" "webOS app" "$WEBOS_BUILD_MEMORY_PER_JOB_MIB" "$WEBOS_BUILD_MEMORY_RESERVE_MIB"

DOVI_TOOL_ROOT="${DOVI_TOOL_ROOT:-$ROOT/build/third_party/dovi_tool}"
if [[ ! -f "$DOVI_TOOL_ROOT/dolby_vision/Cargo.toml" ]]; then
  echo "error: verified dovi_tool source not found at $DOVI_TOOL_ROOT" >&2
  echo "       Run: bash tools/webos-native/build-third-party.sh fetch" >&2
  exit 1
fi

if [[ ! -e "$PREFIX/lib/libcurl.so" ]]; then
  echo "error: private shared curl is missing from $PREFIX." >&2
  echo "       Run: bash $WEBOS_TOOLS_ROOT/build-curl.sh build" >&2
  exit 1
fi

echo "Building libdovi..."
(
  cd "$DOVI_TOOL_ROOT/dolby_vision"
  DOVI_RUST_TOOLCHAIN="${DOVI_RUST_TOOLCHAIN:-1.89.0}"
  DOVI_RUST_TARGET="arm-unknown-linux-gnueabi"
  export CARGO_TARGET_ARM_UNKNOWN_LINUX_GNUEABI_LINKER="$SDK_ROOT/bin/arm-webos-linux-gnueabi-gcc"
  export CC_arm_unknown_linux_gnueabi="$SDK_ROOT/bin/arm-webos-linux-gnueabi-gcc"
  export CXX_arm_unknown_linux_gnueabi="$SDK_ROOT/bin/arm-webos-linux-gnueabi-g++"
  export AR_arm_unknown_linux_gnueabi="$SDK_ROOT/bin/arm-webos-linux-gnueabi-ar"
  export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-$WEBOS_BUILD_JOBS}"

  CARGO_CMD=(cargo)
  if command -v rustup >/dev/null 2>&1; then
    if ! rustup run "$DOVI_RUST_TOOLCHAIN" cargo --version >/dev/null 2>&1; then
      echo "Repairing incomplete Rust $DOVI_RUST_TOOLCHAIN toolchain..."
      rustup toolchain uninstall "$DOVI_RUST_TOOLCHAIN" || true
    fi
    rustup toolchain install "$DOVI_RUST_TOOLCHAIN" \
      --profile minimal \
      --target "$DOVI_RUST_TARGET"
    CARGO_CMD=(rustup run "$DOVI_RUST_TOOLCHAIN" cargo)
  fi
  "${CARGO_CMD[@]}" build \
    --release \
    --features capi,serde \
    --target "$DOVI_RUST_TARGET" \
    --jobs "$WEBOS_BUILD_JOBS"
)
DOVI_LIB="$DOVI_TOOL_ROOT/dolby_vision/target/arm-unknown-linux-gnueabi/release/libdovi.a"
DOVI_INC="$DOVI_TOOL_ROOT/dolby_vision/include"

# Profiling tooling may opt into unwind tables explicitly. Canonical release
# builds leave them out.
HEAPTRACK_UNWIND_FLAGS="${HEAPTRACK_UNWIND_FLAGS:-}"
WEBOS_TUNE_CFLAGS_EXPANDED="$(webos_tune_cflags)"
MPV_PGO_FLAGS="$(webos_pgo_flags MPV "$BUILD_DIR/pgo/mpv")"
APP_PGO_FLAGS="$(webos_pgo_flags APP "$BUILD_DIR/pgo/app")"
COMMON_WEBOS_CFLAGS="-I$DOVI_INC $WEBOS_TUNE_CFLAGS_EXPANDED $HEAPTRACK_UNWIND_FLAGS"
export CFLAGS="${CFLAGS:-} $COMMON_WEBOS_CFLAGS"
export CXXFLAGS="${CXXFLAGS:-} $COMMON_WEBOS_CFLAGS"
export LDFLAGS="${LDFLAGS:-} -L$(dirname "$DOVI_LIB")"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
# Host desktop sessions commonly export QML import paths for the system Qt/KDE
# stack. The webOS build must scan only the cross-built Qt prefix supplied below;
# otherwise qmlimportscanner can wander into Qt source/test fixtures and print
# parser errors for intentionally invalid QML files.
unset QML_IMPORT_PATH QML2_IMPORT_PATH NIXPKGS_QT6_QML_IMPORT_PATH

if [[ ! -x "$GCC_AR_BIN" || ! -x "$GCC_RANLIB_BIN" ]]; then
  echo "error: gcc-ar/gcc-ranlib are required for mpv LTO under $SDK_ROOT/bin" >&2
  exit 1
fi
mkdir -p "$(dirname "$MPV_LTO_CROSS_FILE")"
cat > "$MPV_LTO_CROSS_FILE" <<EOF
[binaries]
ar = '$GCC_AR_BIN'
ranlib = '$GCC_RANLIB_BIN'
EOF

native_mpv_args /usr/local release webos
MPV_SETUP_ARGS=(
  --cross-file "$WEBOS_CROSS_FILE"
  --cross-file "$MPV_LTO_CROSS_FILE"
  "${MPV_NATIVE_ARGS[@]}"
  "-Dc_link_args=-L$(dirname "$DOVI_LIB") -ldovi $MPV_PGO_FLAGS"
  "-Dcpp_link_args=-L$(dirname "$DOVI_LIB") -ldovi $MPV_PGO_FLAGS"
  # Explicit c_args/cpp_args: meson --reconfigure does not re-read CFLAGS env,
  # so pass the tuning + opt-in heaptrack/PGO flags here too.
  "-Dc_args=$WEBOS_TUNE_CFLAGS_EXPANDED $HEAPTRACK_UNWIND_FLAGS $MPV_PGO_FLAGS $MPV_IMAGE_SUBTITLE_DIAGNOSTICS_FLAG"
  "-Dcpp_args=$WEBOS_TUNE_CFLAGS_EXPANDED $HEAPTRACK_UNWIND_FLAGS $MPV_PGO_FLAGS $MPV_IMAGE_SUBTITLE_DIAGNOSTICS_FLAG"
)

if [[ -f "$MPV_BUILD/build.ninja" ]] && grep -Fq "$PREFIX/lib/libcurl.a" "$MPV_BUILD/build.ninja"; then
  echo "Removing mpv build cache tied to obsolete static curl"
  rm -rf "$MPV_BUILD"
fi

# mpv links FFmpeg by soname, and meson has no reason to relink when the
# library underneath it changes major version -- nothing it tracks has moved.
# The stale libmpv then reaches staging asking for a soname the package no
# longer carries, and the ELF audit is what finds out. Key the build directory
# on the FFmpeg pin instead, so the version moving is what rebuilds it.
MPV_FFMPEG_STAMP="$MPV_BUILD/.spool-ffmpeg-source"
MPV_FFMPEG_PIN="$(toolchain_field "$ROOT" ffmpeg.sha256)"
if [[ -f "$MPV_BUILD/build.ninja" ]] \
    && { [[ ! -f "$MPV_FFMPEG_STAMP" ]] || [[ "$(<"$MPV_FFMPEG_STAMP")" != "$MPV_FFMPEG_PIN" ]]; }; then
  echo "Removing mpv build cache built against a different FFmpeg"
  rm -rf "$MPV_BUILD"
fi

if [[ -f "$MPV_BUILD/build.ninja" ]]; then
  meson setup --reconfigure "$MPV_BUILD" "$MPV_SRC" "${MPV_SETUP_ARGS[@]}"
else
  meson setup "$MPV_BUILD" "$MPV_SRC" "${MPV_SETUP_ARGS[@]}"
fi
meson compile -C "$MPV_BUILD" -j "$WEBOS_BUILD_JOBS"
printf '%s' "$MPV_FFMPEG_PIN" >"$MPV_FFMPEG_STAMP"

if [[ -n "$APP_PGO_FLAGS" ]]; then
  export CFLAGS="$CFLAGS $APP_PGO_FLAGS"
  export CXXFLAGS="$CXXFLAGS $APP_PGO_FLAGS"
  export LDFLAGS="$LDFLAGS $APP_PGO_FLAGS"
fi

cmake -S "$ROOT" -B "$CMAKE_BUILD_DIR" -GNinja \
  --fresh \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DCMAKE_TOOLCHAIN_FILE="$WEBOS_TOOLS_ROOT/qt6-webos-toolchain.cmake" \
  -DCMAKE_STAGING_PREFIX="$QT6_PREFIX" \
  -DCMAKE_FIND_ROOT_PATH="$SYSROOT;$QT6_PREFIX" \
  -DCMAKE_PREFIX_PATH="$QT6_PREFIX;$QT6_PREFIX/lib/cmake" \
  -DQt6_DIR="$QT6_PREFIX/lib/cmake/Qt6" \
  -DQT_HOST_PATH="$QT6_HOST_PREFIX" \
  -DCMAKE_INSTALL_PREFIX=/usr/palm/applications/com.sachk.spool \
  -DSPOOL_IMAGE_SUBTITLE_DIAGNOSTICS="$CMAKE_IMAGE_SUBTITLE_DIAGNOSTICS"

cmake --build "$CMAKE_BUILD_DIR" --parallel "$WEBOS_BUILD_JOBS"
cmake --install "$CMAKE_BUILD_DIR" --prefix "$INSTALL_DIR"
fi

if (( DO_STAGE )); then
rm -rf "$APP_DIR"
mkdir -p "$BUILD_DIR" "$STAGE_LIB" "$STAGE_BIN" "$APP_DIR/fonts"
python3 "$ROOT/tools/render-appinfo.py" \
  "$APP_SOURCE_DIR/appinfo.json.in" \
  "$ROOT/VERSION" \
  "$APP_DIR/appinfo.json"
cp -f "$APP_SOURCE_DIR/icons/png/spool/80.png" "$APP_DIR/icon.png"
cp -f "$APP_SOURCE_DIR/icons/png/spool/130.png" "$APP_DIR/large-icon.png"
# Rendered by the app build, because it carries the version: the full-screen
# frame appinfo.json names, and the same block on its own for Qt to draw.
SPLASH_DIR="${SPOOL_SPLASH_DIR:-$CMAKE_BUILD_DIR/splash/webos}"
[[ -f "$SPLASH_DIR/splash.png" ]] || {
  echo "error: launch screen missing at $SPLASH_DIR; run '$0 app' first" >&2
  exit 1
}
cp -f "$SPLASH_DIR/splash.png" "$APP_DIR/splash.png"
cp -f "$SPLASH_DIR/splash-core.png" "$APP_DIR/splash-core.png"
cp -f "$INSTALL_DIR/fonts/"* "$APP_DIR/fonts/"
mkdir -p "$APP_DIR/notices"
cp -f "$ROOT/app/notices/OPEN_SOURCE_NOTICES.txt" "$ROOT/LICENSE" \
  "$ROOT/qml/fonts/AtkinsonHyperlegible-LICENSE.txt" \
  "$ROOT/qml/fonts/IBMPlexSans-LICENSE.txt" "$ROOT/qml/fonts/PTRootUI-LICENSE.txt" \
  "$ROOT/qml/fonts/MaterialIcons-LICENSE.txt" \
  "$APP_DIR/notices/"
PATCHELF_BIN="$(command -v patchelf)"

[[ -x "$INSTALL_DIR/bin/spool" ]] || {
  echo "error: app install missing; run '$0 app' first" >&2
  exit 1
}
cp -f "$INSTALL_DIR/bin/spool" "$STAGE_BIN/spool"

# mpv, curl, FFmpeg, and OpenSSL shared libraries are always needed. Stage
# their real files and derive compatibility symlinks from each ELF SONAME so
# patch-level dependency upgrades do not require edits here.
MPV_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$MPV_BUILD/libmpv.so.*" "$STAGE_LIB" "$READELF_BIN")"
CURL_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$PREFIX/lib/libcurl.so.*" "$STAGE_LIB" "$READELF_BIN")"


FFMPEG_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$PREFIX/lib/libavcodec.so.*" "$STAGE_LIB" "$READELF_BIN")"
AVFORMAT_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$PREFIX/lib/libavformat.so.*" "$STAGE_LIB" "$READELF_BIN")"
OPENSSL_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$SYSROOT/usr/lib/libssl.so.*" "$STAGE_LIB" "$READELF_BIN")"
OPENSSL_CRYPTO_STAGED_LIBRARY="$(stage_elf_shared_library \
  "$SYSROOT/usr/lib/libcrypto.so.*" "$STAGE_LIB" "$READELF_BIN")"

for pattern in \
  "$PREFIX/lib/libavfilter.so.*" \
  "$PREFIX/lib/libavutil.so.*" \
  "$PREFIX/lib/libswresample.so.*" \
  "$PREFIX/lib/libswscale.so.*" \
  "$SYSROOT/usr/lib/libAcbAPI.so.*" \
  "$SYSROOT/usr/lib/libstdc++.so.*" \
  "$SYSROOT/lib/libgcc_s.so.*" \
  "$SYSROOT/usr/lib/libpcre2-16.so.*" \
  "$SYSROOT/usr/lib/libjpeg.so.*" \
  "$SYSROOT/usr/lib/libpng16.so.*"
do
  stage_elf_shared_library "$pattern" "$STAGE_LIB" "$READELF_BIN" >/dev/null
done

# libavformat is on this list because lavf's HLS demuxer resolves every
# segment URL by protocol name before mpv's libcurl backend is consulted, and
# an FFmpeg without a TLS backend registers no https protocol to find. It
# moves no bytes; libcurl still carries playback.
for tls_consumer in "$STAGE_BIN/spool" "$CURL_STAGED_LIBRARY" "$AVFORMAT_STAGED_LIBRARY"; do
  TLS_DYNAMIC_SECTION="$("$READELF_BIN" -d "$tls_consumer")"
  if [[ "$TLS_DYNAMIC_SECTION" != *"[libssl.so"* && "$TLS_DYNAMIC_SECTION" != *"[libcrypto.so"* ]]; then
    echo "error: $(basename "$tls_consumer") does not use the packaged shared OpenSSL build" >&2
    exit 1
  fi
done
MPV_DYNAMIC_SECTION="$("$READELF_BIN" -d "$MPV_STAGED_LIBRARY")"
if [[ "$MPV_DYNAMIC_SECTION" != *"[libcurl.so"* ]]; then
  echo "error: $(basename "$MPV_STAGED_LIBRARY") does not use packaged shared curl" >&2
  exit 1
fi


# Keep every private dependency rooted in the package instead of binding to
# firmware libraries with the same SONAME.
"$PATCHELF_BIN" --force-rpath --set-rpath '$ORIGIN' "$MPV_STAGED_LIBRARY"
"$PATCHELF_BIN" --force-rpath --set-rpath '$ORIGIN' "$CURL_STAGED_LIBRARY"
"$PATCHELF_BIN" --force-rpath --set-rpath '$ORIGIN' "$AVFORMAT_STAGED_LIBRARY"
"$PATCHELF_BIN" --force-rpath --set-rpath '$ORIGIN/../lib' "$STAGE_BIN/spool"

"$ROOT/tools/webos-native/audit-arm-binaries.sh" \
  "$READELF_BIN" "$OBJDUMP_BIN" --thumb-archive \
  "$QT6_PREFIX/lib/libQt6Core.a"
"$ROOT/tools/webos-native/audit-arm-binaries.sh" \
  "$READELF_BIN" "$OBJDUMP_BIN" --thumb \
  "$STAGE_BIN/spool" "$MPV_STAGED_LIBRARY" "$CURL_STAGED_LIBRARY" \
  "$FFMPEG_STAGED_LIBRARY" "$AVFORMAT_STAGED_LIBRARY"

echo "Stripping every staged webOS ELF"
declare -A STRIPPED_ELF_INODES=()
while IFS= read -r -d '' elf; do
  "$READELF_BIN" -h "$elf" >/dev/null 2>&1 || continue
  inode="$(stat -Lc '%d:%i' "$elf")"
  [[ -z "${STRIPPED_ELF_INODES[$inode]:-}" ]] || continue
  "$STRIP_BIN" --strip-unneeded "$elf"
  STRIPPED_ELF_INODES[$inode]=1
done < <(find "$APP_DIR" -type f -print0)

"$ROOT/tools/webos-native/audit-arm-binaries.sh" \
  "$READELF_BIN" "$OBJDUMP_BIN" --stripped \
  "$STAGE_BIN/spool" "$MPV_STAGED_LIBRARY" "$CURL_STAGED_LIBRARY" \
  "$FFMPEG_STAGED_LIBRARY" "$AVFORMAT_STAGED_LIBRARY" "$OPENSSL_STAGED_LIBRARY" \
  "$OPENSSL_CRYPTO_STAGED_LIBRARY"

WEBOS_ELF_AUDIT_ARGS=(
  elf "$APP_DIR"
  --root bin/spool
  --readelf "$READELF_BIN"
)
for runtime_root in \
  "$MPV_STAGED_LIBRARY" \
  "$CURL_STAGED_LIBRARY" \
  "$STAGE_LIB"/libAcbAPI.so.* \
  "$STAGE_LIB"/libpcre2-16.so.*
do
  WEBOS_ELF_AUDIT_ARGS+=(--root "${runtime_root#"$APP_DIR/"}")
done
for firmware_soname in \
  ld-linux.so.3 libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 libresolv.so.2 libutil.so.1 \
  libatomic.so.1 libz.so.1 libfontconfig.so.1 libfreetype.so.6 \
  libEGL.so.1 libGLESv2.so.2 libwayland-client.so.0 libwayland-cursor.so.0 \
  libwayland-egl.so libwayland-egl.so.1 libwayland-webos-client.so.0 libwayland-webos-client.so.1 \
  libxkbcommon.so.0 libasound.so.2 libglib-2.0.so.0 libpbnjson_c.so.2 libluna-service2.so.3 \
  libpf-1.0.so.1 libplayerAPIs.so.1 libunwind.so.8 \
  libPmLogLib.so.3 libnyx.so.0 libudev.so.0 libhelpers.so.1 libhelpers.so.2
do
  WEBOS_ELF_AUDIT_ARGS+=(--allow-system "$firmware_soname")
done
python3 "$ROOT/tools/package-audit.py" "${WEBOS_ELF_AUDIT_ARGS[@]}"
python3 "$ROOT/tools/ffmpeg-capabilities.py" \
  --manifest "$FFMPEG_CAPABILITY_MANIFEST" audit-closure "$STAGE_LIB"
python3 "$ROOT/tools/package-audit.py" inventory "$APP_DIR" \
  --output "$BUILD_DIR/webos-stage.inventory.tsv"
fi

if (( DO_PACKAGE )); then
[[ -f "$APP_DIR/appinfo.json" && -x "$STAGE_BIN/spool" ]] || {
  echo "error: staged webOS app missing; run '$0 stage' first" >&2
  exit 1
}
rm -f "$BUILD_DIR"/*.ipk
# Plain ares-package output, no maintainer scripts. Hardware decode LS2 access
# comes from appinstalld's rolegen (/usr/share/rolegen/templates/NDK.*), which
# generates media-client roles for every native dev app from appinfo's "main"
# executable — the same mechanism Kodi relies on for non-rooted TVs.
npx -y -p @webos-tools/cli@3.2.3 ares-package "$APP_DIR" --outdir "$BUILD_DIR"
fi
