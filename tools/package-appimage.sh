#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
ensure_native_shell "$APP_ROOT" "$APP_ROOT/tools/package-appimage.sh" "$@"
# shellcheck source=tools/lib/qt-deploy.sh
source "$APP_ROOT/tools/lib/qt-deploy.sh"
# shellcheck source=tools/lib/manifest-sources.sh
source "$APP_ROOT/tools/lib/manifest-sources.sh"
TOOL_MANIFEST="${LINUXDEPLOY_MANIFEST:-$APP_ROOT/tools/manifests/linuxdeploy.json}"
FFMPEG_CAPABILITY_MANIFEST="${FFMPEG_CAPABILITY_MANIFEST:-$APP_ROOT/tools/manifests/ffmpeg-capabilities.json}"
APP_VERSION="$(read_project_version "$APP_ROOT")"
APP_INSTALL="${APP_INSTALL:-$APP_ROOT/build/linux-release/install}"
APPDIR="${APPDIR:-$APP_ROOT/build/appimage/AppDir}"
ARTIFACT_DIR="${ARTIFACT_DIR:-$APP_ROOT/dist}"
QT_PLUGIN="${QT_PLUGIN:-$APP_ROOT/build/appimage/linuxdeploy-plugin-qt-x86_64.AppImage}"
DWARFS_RUNTIME="${DWARFS_RUNTIME:-$APP_ROOT/build/appimage/uruntime-appimage-dwarfs-lite-x86_64}"
PATCHELF_ARCHIVE="${PATCHELF_ARCHIVE:-$APP_ROOT/build/appimage/patchelf-0.18.0-x86_64.tar.gz}"
PATCHELF_ROOT="${PATCHELF_ROOT:-$APP_ROOT/build/appimage/patchelf-0.18.0}"
PATCHELF_BIN="$PATCHELF_ROOT/bin/patchelf"
DWARFS_IMAGE="${DWARFS_IMAGE:-$APP_ROOT/build/appimage/root.dwarfs}"

append_library_path() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  case ":${LD_LIBRARY_PATH:-}:" in
    *":$dir:"*) ;;
    *) export LD_LIBRARY_PATH="$dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
  esac
}

prepend_path() {
  local dir="$1"
  [[ -d "$dir" ]] || return 0
  case ":$PATH:" in
    *":$dir:"*) ;;
    *) export PATH="$dir:$PATH" ;;
  esac
}

is_bundleable_elf_dep() {
  local dep="$1"
  local base="${dep##*/}"
  [[ -f "$dep" ]] || return 1
  case "$dep" in
    /nix/store/*-glibc-*) return 1 ;;
  esac
  case "$base" in
    ld-linux*.so*|libc.so*|libdl.so*|libm.so*|libpthread.so*|libresolv.so*|librt.so*|libutil.so*)
      return 1
      ;;
    libGLES*.so*|libglapi*.so*|libgbm*.so*)
      return 1
      ;;
  esac
  case "$dep" in
    /nix/store/*|"$APP_INSTALL"/*) return 0 ;;
    *) return 1 ;;
  esac
}

copy_elf_deps() {
  local deferred_dep="${1:-}"
  local elf
  local dep
  local copied=1
  local roots=("$APPDIR/usr/bin" "$APPDIR/usr/lib")
  [[ -d "$APPDIR/usr/plugins" ]] && roots+=("$APPDIR/usr/plugins")
  [[ -d "$APPDIR/usr/qml" ]] && roots+=("$APPDIR/usr/qml")

  while (( copied )); do
    copied=0
    while IFS= read -r elf; do
      [[ -f "$elf" ]] || continue
      while IFS= read -r dep; do
        if [[ -n "$deferred_dep" && "${dep##*/}" == "$deferred_dep" ]]; then
          continue
        fi
        is_bundleable_elf_dep "$dep" || continue
        if [[ ! -e "$APPDIR/usr/lib/$(basename "$dep")" ]]; then
          cp -L "$dep" "$APPDIR/usr/lib/"
          chmod u+w "$APPDIR/usr/lib/$(basename "$dep")" 2>/dev/null || true
          copied=1
        fi
      done < <(ldd "$elf" 2>/dev/null | awk '/=> \// { print $3 } /^\// { print $1 }')
    done < <(find "${roots[@]}" -type f)
  done
}

copy_tree_if_exists() {
  local src="$1"
  local dst="$2"
  [[ -e "$src" ]] || return 0
  mkdir -p "$(dirname "$dst")"
  rm -rf "$dst"
  cp -a "$src" "$dst"
  chmod -R u+w "$dst" 2>/dev/null || true
}


bundle_wayland_plugins() {
  local plugins_dir
  plugins_dir="$(qt_deploy_query_path QT_INSTALL_PLUGINS)"
  [[ -d "$plugins_dir" ]] || return 0

  copy_tree_if_exists "$plugins_dir/platforms/libqwayland.so" \
    "$APPDIR/usr/plugins/platforms/libqwayland.so"
  copy_tree_if_exists "$plugins_dir/wayland-shell-integration" \
    "$APPDIR/usr/plugins/wayland-shell-integration"
  copy_tree_if_exists "$plugins_dir/wayland-graphics-integration-client" \
    "$APPDIR/usr/plugins/wayland-graphics-integration-client"
  copy_tree_if_exists "$plugins_dir/wayland-decoration-client" \
    "$APPDIR/usr/plugins/wayland-decoration-client"
}

prime_linuxdeploy_qt_plugins() {
  local root plugins_dir plugin_dir platform
  local plugin_roots=()

  plugins_dir="$(qt_deploy_query_path QT_INSTALL_PLUGINS)"
  [[ -d "$plugins_dir" ]] && plugin_roots+=("$plugins_dir")

  local qmake_roots=()
  IFS=: read -r -a qmake_roots <<< "${QMAKEPATH:-}"
  for root in "${qmake_roots[@]}"; do
    plugins_dir="$root/lib/qt-6/plugins"
    [[ -d "$plugins_dir" ]] || continue
    plugin_roots+=("$plugins_dir")
  done

  for plugins_dir in "${plugin_roots[@]}"; do
    for plugin_dir in \
      platforminputcontexts \
      tls \
      wayland-decoration-client \
      wayland-graphics-integration-client \
      wayland-shell-integration; do
      copy_tree_if_exists "$plugins_dir/$plugin_dir" \
        "$APPDIR/usr/lib/qt-6/plugins/$plugin_dir"
    done

    mkdir -p "$APPDIR/usr/lib/qt-6/plugins/platforms" "$APPDIR/usr/lib/qt-6/plugins/sqldrivers"
    for platform in libqxcb.so libqwayland.so; do
      [[ -f "$plugins_dir/platforms/$platform" ]] || continue
      cp -a "$plugins_dir/platforms/$platform" "$APPDIR/usr/lib/qt-6/plugins/platforms/"
      chmod u+w "$APPDIR/usr/lib/qt-6/plugins/platforms/$platform" 2>/dev/null || true
    done
    if [[ -f "$plugins_dir/sqldrivers/libqsqlite.so" ]]; then
      cp -a "$plugins_dir/sqldrivers/libqsqlite.so" "$APPDIR/usr/lib/qt-6/plugins/sqldrivers/"
      chmod u+w "$APPDIR/usr/lib/qt-6/plugins/sqldrivers/libqsqlite.so" 2>/dev/null || true
    fi
  done

  # qtimageformats is not on QMAKEPATH — nothing links it, it is loaded at
  # runtime — so the readers above stop at qtbase's gif/ico/jpeg. Artwork is
  # served as webp, which lands in none of them. Take that one reader and leave
  # the rest of the module alone: its JPEG 2000 plugin pulls libheif and the
  # x265/aom/vmaf closure that audit_ffmpeg_closure exists to keep out.
  local extra_roots=()
  IFS=: read -r -a extra_roots <<< "${SPOOL_QT_EXTRA_PLUGIN_DIRS:-}"
  for plugins_dir in "${extra_roots[@]}"; do
    [[ -n "$plugins_dir" && -f "$plugins_dir/imageformats/libqwebp.so" ]] || continue
    mkdir -p "$APPDIR/usr/lib/qt-6/plugins/imageformats"
    cp -a "$plugins_dir/imageformats/libqwebp.so" \
      "$APPDIR/usr/lib/qt-6/plugins/imageformats/"
    chmod u+w "$APPDIR/usr/lib/qt-6/plugins/imageformats/libqwebp.so" 2>/dev/null || true
  done
}

# Artwork is served as webp, so a bundle without this reader shows an app with
# no posters at all while every download still succeeds. tools/windows/stage.ps1
# asserts the same thing for qwebp.dll; the failure is silent without a check.
audit_image_formats() {
  local plugin="$APPDIR/usr/plugins/imageformats/libqwebp.so"
  if [[ ! -f "$plugin" ]]; then
    printf 'error: qwebp imageformat plugin was not deployed to %s\n' "$plugin" >&2
    printf 'hint: SPOOL_QT_EXTRA_PLUGIN_DIRS must point at the qtimageformats plugin prefix (see flake.nix)\n' >&2
    exit 1
  fi
}

prune_appdir() {
  rm -rf \
    "$APPDIR/usr/qml/QtQuick/Controls/designer" \
    "$APPDIR/usr/qml/QtQuick/Controls/FluentWinUI3" \
    "$APPDIR/usr/qml/QtQuick/Controls/Fusion" \
    "$APPDIR/usr/lib/qt-6" \
    "$APPDIR/usr/qml/QtQuick/Controls/Imagine" \
    "$APPDIR/usr/qml/QtQuick/Controls/Material" \
    "$APPDIR/usr/qml/QtQuick/Controls/Universal" \
    "$APPDIR/usr/qml/QtQuick/Controls/FluentWinUI3.impl" \
    "$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qml/+Fusion" \
    "$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qml/+Imagine" \
    "$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qml/+Material" \
    "$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qml/+Universal" \
    "$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qml/+FluentWinUI3" \
    "$APPDIR/usr/qml/QtQuick/tooling" \
    "$APPDIR/usr/qml/QtQuick/Shapes/DesignHelpers" \
    "$APPDIR/usr/qml/QtQuick/VectorImage/Helpers"

  local dialogs_qmldir="$APPDIR/usr/qml/QtQuick/Dialogs/quickimpl/qmldir"
  if [[ -f "$dialogs_qmldir" ]]; then
    sed -E -i '/qml\/\+(Fusion|Imagine|Material|Universal|FluentWinUI3)\//d' "$dialogs_qmldir"
  fi
  find "$APPDIR/usr" -type f -name '*.qmltypes' -delete

  find "$APPDIR/usr/lib" -maxdepth 1 -type f \( \
    -name 'libQt6QuickControls2FluentWinUI3*.so*' -o \
    -name 'libQt6QuickControls2Fusion*.so*' -o \
    -name 'libQt6QuickControls2Imagine*.so*' -o \
    -name 'libQt6QuickControls2Material*.so*' -o \
    -name 'libQt6QuickControls2Universal*.so*' -o \
    -name 'libQt6QuickDialogs2QuickImpl.so*' -o \
    -name 'libQt6QuickShapesDesignHelpers.so*' -o \
    -name 'libQt6QuickVectorImageHelpers.so*' \
  \) -delete

  find "$APPDIR/usr/plugins/sqldrivers" -maxdepth 1 -type f ! -name 'libqsqlite.so' -delete 2>/dev/null || true
  find "$APPDIR/usr/plugins/platforms" -maxdepth 1 -type f \
    ! -name 'libqxcb.so' ! -name 'libqwayland.so' -delete 2>/dev/null || true
  find "$APPDIR/usr/lib" -maxdepth 1 -type f \( \
    -name 'libGLES*.so*' -o \
    -name 'libglapi*.so*' -o \
    -name 'libgbm*.so*' \
  \) -delete

  if [[ -d "$APPDIR/usr/translations" ]]; then
    find "$APPDIR/usr/translations" -maxdepth 1 -type f -name '*.qm' \
      ! -name 'qtbase_en*.qm' ! -name 'qt_en*.qm' -delete 2>/dev/null || true
  fi
}

# Fail packaging if a GPL/external codec library or another manifest-forbidden
# dependency leaked into the AppImage closure.
audit_ffmpeg_closure() {
  python3 "$APP_ROOT/tools/ffmpeg-capabilities.py" --manifest "$FFMPEG_CAPABILITY_MANIFEST" \
    audit-closure "$APPDIR/usr/lib" "$APPDIR/usr/bin"
}

normalize_nix_store_needed() {
  local elf="$1"
  local needed
  while IFS= read -r needed; do
    if is_bundleable_elf_dep "$needed" && [[ ! -e "$APPDIR/usr/lib/${needed##*/}" ]]; then
      cp -L "$needed" "$APPDIR/usr/lib/"
      chmod u+w "$APPDIR/usr/lib/${needed##*/}"
    fi
    [[ "$needed" == /nix/store/* ]] || continue
    "$PATCHELF_BIN" --replace-needed "$needed" "${needed##*/}" "$elf"
  done < <("$PATCHELF_BIN" --print-needed "$elf")
}

patchelf_set_rpath() {
  local rpath="$1"
  local elf="$2"
  chmod u+w "$elf"
  "$PATCHELF_BIN" --set-rpath "$rpath" "$elf"
}

set_appdir_rpaths() {
  local elf rel depth prefix
  while IFS= read -r elf; do
    file -b "$elf" | grep -q 'ELF' || continue
    normalize_nix_store_needed "$elf"
    case "$elf" in
      "$APPDIR/usr/bin/"*) patchelf_set_rpath '$ORIGIN/../lib' "$elf" ;;
      "$APPDIR/usr/lib/"*) patchelf_set_rpath '$ORIGIN' "$elf" ;;
      *)
        rel="${elf#"$APPDIR/usr/"}"
        depth=$(awk -F/ '{ print NF - 1 }' <<< "$rel")
        prefix='$ORIGIN'
        while (( depth > 0 )); do
          prefix="$prefix/.."
          depth=$((depth - 1))
        done
        patchelf_set_rpath "$prefix/lib:\$ORIGIN" "$elf"
        ;;
    esac
  done < <(find "$APPDIR/usr" -type f)
}

strip_appdir_elfs() {
  local elf inode
  declare -A stripped_inodes=()
  while IFS= read -r elf; do
    file -b "$elf" | grep -q 'ELF.*not stripped' || continue
    inode="$(stat -Lc '%d:%i' "$elf")"
    [[ -z "${stripped_inodes[$inode]:-}" ]] || continue
    eu-strip "$elf"
    stripped_inodes[$inode]=1
  done < <(find "$APPDIR/usr" -type f | sort)
}

audit_and_sweep_appdir_elfs() {
  local elf rel status kind
  local -a audit_args=(
    elf "$APPDIR"
    --root usr/bin/jellyfin-native
    --allow-system ld-linux-x86-64.so.2
    --allow-system libc.so.6
    --allow-system libdl.so.2
    --allow-system libm.so.6
    --allow-system libpthread.so.0
    --allow-system libresolv.so.2
    --allow-system librt.so.1
    --allow-system libmvec.so.1
    --allow-system libutil.so.1
    --allow-system libGLESv2.so.2
    --allow-system libgbm.so.1
    --allow-system libglapi.so.0
  )
  if [[ -f "$APPDIR/usr/bin/secret-tool" ]]; then
    audit_args+=(--root usr/bin/secret-tool)
  fi
  while IFS= read -r elf; do
    audit_args+=(--root "${elf#"$APPDIR/"}")
  done < <(find "$APPDIR/usr/lib" -type f -name 'libmpv.so.*' | sort)
  for rel in usr/plugins usr/qml; do
    [[ -d "$APPDIR/$rel" ]] || continue
    while IFS= read -r elf; do
      audit_args+=(--root "${elf#"$APPDIR/"}")
    done < <(find "$APPDIR/$rel" -type f -name '*.so*' | sort)
  done

  local report="$APPDIR/.elf-audit-unreachable"
  local errors="$APPDIR/.elf-audit-errors"
  set +e
  python3 "$APP_ROOT/tools/package-audit.py" "${audit_args[@]}" >"$report" 2>"$errors"
  status=$?
  set -e
  local removed=0
  while IFS=$'\t' read -r kind rel; do
    [[ "$kind" == UNREACHABLE && -n "$rel" ]] || continue
    rm -f "$APPDIR/$rel"
    removed=$((removed + 1))
  done <"$report"
  rm -f "$report"
  if (( status != 0 && removed == 0 )); then
    cat "$errors" >&2
    rm -f "$errors"
    return "$status"
  fi
  rm -f "$errors"
  python3 "$APP_ROOT/tools/package-audit.py" "${audit_args[@]}"
}

if [[ ! -x "$APP_INSTALL/bin/jellyfin-native" ]]; then
  echo "error: installed build output not found at $APP_INSTALL/bin/jellyfin-native" >&2
  exit 1
fi

mkdir -p "$ARTIFACT_DIR" "$(dirname "$QT_PLUGIN")" "$(dirname "$DWARFS_RUNTIME")"
if [[ -d "$APPDIR" ]]; then
  chmod -R u+w "$APPDIR" 2>/dev/null || true
fi
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr" "$APPDIR/usr/share/jellyfin-native/notices"
cp -a "$APP_INSTALL/." "$APPDIR/usr/"
if command -v secret-tool >/dev/null 2>&1; then
  cp -f "$(command -v secret-tool)" "$APPDIR/usr/bin/secret-tool"
fi
cp -f "$APP_ROOT/app/notices/OPEN_SOURCE_NOTICES.txt" "$APP_ROOT/LICENSE" \
  "$APP_ROOT/qml/fonts/AtkinsonHyperlegible-LICENSE.txt" \
  "$APP_ROOT/qml/fonts/IBMPlexSans-LICENSE.txt" "$APP_ROOT/qml/fonts/PTRootUI-LICENSE.txt" \
  "$APP_ROOT/qml/fonts/MaterialIcons-LICENSE.txt" \
  "$APPDIR/usr/share/jellyfin-native/notices/"
ln -s "usr/share/icons/hicolor/256x256/apps/com.sachk.spool.png" "$APPDIR/com.sachk.spool.png"
ln -s "usr/share/applications/com.sachk.spool.desktop" "$APPDIR/com.sachk.spool.desktop"
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/usr/bin/env bash
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HERE/usr/bin${PATH:+:$PATH}"
unset QML2_IMPORT_PATH QML_IMPORT_PATH QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH
if [[ -d /run/opengl-driver/lib ]]; then
  export LD_LIBRARY_PATH="$HERE/usr/lib:/run/opengl-driver/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [[ -z "${__EGL_VENDOR_LIBRARY_DIRS:-}" && -d /run/opengl-driver/share/glvnd/egl_vendor.d ]]; then
  export __EGL_VENDOR_LIBRARY_DIRS=/run/opengl-driver/share/glvnd/egl_vendor.d
fi
if [[ -z "${__EGL_VENDOR_LIBRARY_FILENAMES:-}" && -z "${__EGL_VENDOR_LIBRARY_DIRS:-}" && -d /run/opengl-driver/lib ]]; then
  egl_vendor_dir="${XDG_RUNTIME_DIR:-/tmp}/jellyfin-native-egl-vendors"
  mkdir -p "$egl_vendor_dir"
  rm -f "$egl_vendor_dir"/*.json
  for egl_vendor in /run/opengl-driver/lib/libEGL_*.so*; do
    [[ -e "$egl_vendor" ]] || continue
    egl_name="${egl_vendor##*/}"
    egl_name="${egl_name#libEGL_}"
    egl_name="${egl_name%%.so*}"
    cat > "$egl_vendor_dir/10_${egl_name}.json" <<EOF
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "$egl_vendor"
  }
}
EOF
  done
  if compgen -G "$egl_vendor_dir/*.json" >/dev/null; then
    export __EGL_VENDOR_LIBRARY_DIRS="$egl_vendor_dir"
  fi
fi
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/usr/plugins/platforms"
export QML2_IMPORT_PATH="$HERE/usr/qml"
export QML_IMPORT_PATH="$HERE/usr/qml"
# NixOS appimage-run's FHS container cannot initialize the host Wayland EGL
# vendor stack reliably. XWayland uses GLX and avoids that mixed-driver path.
# An installed bundle (/opt/spool, from the portable tarball or the Arch
# package) is not that container, so it says so and keeps native Wayland.
if [[ "${SPOOL_PORTABLE_BUNDLE:-0}" != 1 \
  && "$HERE" != /tmp/.mount_* && -n "${DISPLAY:-}" ]]; then
  export QT_QPA_PLATFORM=xcb
else
  export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
fi
exec "$HERE/usr/bin/jellyfin-native" "$@"
APPRUN
chmod +x "$APPDIR/AppRun"


download_verified \
  "$(manifest_tool_field "$TOOL_MANIFEST" linuxdeploy-plugin-qt url)" \
  "$(manifest_tool_field "$TOOL_MANIFEST" linuxdeploy-plugin-qt sha256)" \
  "$QT_PLUGIN"
chmod +x "$QT_PLUGIN"
download_verified \
  "$(manifest_tool_field "$TOOL_MANIFEST" patchelf url)" \
  "$(manifest_tool_field "$TOOL_MANIFEST" patchelf sha256)" \
  "$PATCHELF_ARCHIVE"
rm -rf "$PATCHELF_ROOT"
mkdir -p "$PATCHELF_ROOT"
tar -xzf "$PATCHELF_ARCHIVE" -C "$PATCHELF_ROOT"
[[ -x "$PATCHELF_BIN" ]] || {
  echo "error: pinned patchelf executable is missing at $PATCHELF_BIN" >&2
  exit 1
}
download_verified \
  "$(manifest_tool_field "$TOOL_MANIFEST" dwarfs-runtime url)" \
  "$(manifest_tool_field "$TOOL_MANIFEST" dwarfs-runtime sha256)" \
  "$DWARFS_RUNTIME"

QT_DEPLOY_SHADOW="$APP_ROOT/build/appimage/qt-host-tools"
qt_deploy_path="$(qt_deploy_linuxdeploy_qt_shadow \
  "${APP_BUILD_NINJA:-$APP_ROOT/build/linux-release/app/build.ninja}" "$QT_DEPLOY_SHADOW")"
if [[ -z "$qt_deploy_path" || ! -x "$qt_deploy_path/qmlimportscanner" ]]; then
  echo "error: qmlimportscanner is required for AppImage QML deployment" >&2
  exit 1
fi
export PATH="$qt_deploy_path${PATH:+:$PATH}"

append_library_path "$APP_INSTALL/lib"
IFS=':' read -r -a cmake_library_dirs <<< "${CMAKE_LIBRARY_PATH:-}"
for lib_dir in "${cmake_library_dirs[@]}"; do
  append_library_path "$lib_dir"
done
while IFS= read -r dep; do
  case "$dep" in
    /nix/store/*-glibc-*) continue ;;
  esac
  append_library_path "$(dirname "$dep")"
  is_bundleable_elf_dep "$dep" || continue
done < <(ldd "$APPDIR/usr/bin/jellyfin-native" "$APPDIR"/usr/lib/libmpv.so* 2>/dev/null | awk '/=> \// { print $3 } /^\// { print $1 }' | sort -u)

export EXTRA_PLATFORM_PLUGINS="${EXTRA_PLATFORM_PLUGINS:-libqwayland.so}"
export QML_SOURCES_PATHS="${QML_SOURCES_PATHS:-$APP_ROOT/qml}"
export APPIMAGE_EXTRACT_AND_RUN="${APPIMAGE_EXTRACT_AND_RUN:-1}"
export OUTPUT="${OUTPUT:-Spool-for-Jellyfin-${APP_VERSION}-x86_64.AppImage}"

copy_elf_deps libQt6WebSockets.so.6
set_appdir_rpaths

prime_linuxdeploy_qt_plugins
"$QT_PLUGIN" --appdir "$APPDIR"

unset EXTRA_PLATFORM_PLUGINS
bundle_wayland_plugins
prune_appdir
set_appdir_rpaths
copy_elf_deps
set_appdir_rpaths
chmod -R u+w "$APPDIR/usr"
strip_appdir_elfs
audit_and_sweep_appdir_elfs
audit_ffmpeg_closure
audit_image_formats

rm -f "$DWARFS_IMAGE" "$ARTIFACT_DIR/$OUTPUT.tmp"
mkdwarfs \
  --input "$APPDIR" \
  --output "$DWARFS_IMAGE" \
  --block-size-bits 24 \
  --compression zstd:level=22 \
  --schema-compression zstd:level=16 \
  --metadata-compression zstd:level=22 \
  --history-compression zstd:level=16 \
  --window-size 12 \
  --window-step 3 \
  --order nilsimsa \
  --set-owner 0 \
  --set-group 0 \
  --set-time "${SOURCE_DATE_EPOCH:-0}" \
  --no-create-timestamp \
  --no-history \
  --no-progress
cat "$DWARFS_RUNTIME" "$DWARFS_IMAGE" >"$ARTIFACT_DIR/$OUTPUT.tmp"
chmod 0755 "$ARTIFACT_DIR/$OUTPUT.tmp"
mv "$ARTIFACT_DIR/$OUTPUT.tmp" "$ARTIFACT_DIR/$OUTPUT"
python3 "$APP_ROOT/tools/package-audit.py" inventory "$APPDIR" \
  --output "$ARTIFACT_DIR/${OUTPUT%.AppImage}.inventory.tsv"
printf '%s\n' "$ARTIFACT_DIR/$OUTPUT"
