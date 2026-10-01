#!/usr/bin/env bash
# Shared helpers for native build and packaging scripts.
#
# Source this file ("source tools/lib/build-common.sh"); do not execute it.
# It defines functions only and changes no global state.

ensure_native_shell() {
  local root="$1"
  local script="$2"
  shift 2

  if [[ "${SPOOL_SHELL:-0}" == "1" ]]; then
    return 0
  fi
  command -v nix >/dev/null 2>&1 || {
    echo "error: native build requires Nix; run from the repository root" >&2
    return 1
  }

  cd "$root"
  exec nix develop "$root#native" -c bash "$script" "$@"
}

read_project_version() {
  local root="$1"
  local version_file="$root/VERSION"
  local version

  [[ -f "$version_file" ]] || {
    echo "error: version file not found at $version_file" >&2
    return 1
  }
  version="$(tr -d '[:space:]' <"$version_file")"
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "error: invalid project version in $version_file: $version" >&2
    return 1
  }
  printf '%s\n' "$version"
}

# stage_elf_shared_library GLOB DESTINATION READELF
#
# Copy the newest matching ELF shared library by its real filename, then create
# its SONAME and unversioned linker aliases. Prints the staged real file path.
stage_elf_shared_library() {
  local pattern="$1"
  local destination="$2"
  local readelf_bin="$3"
  local matches=()
  local candidate
  local candidate_soname
  local index
  local source
  local real_source
  local real_name
  local soname
  local unversioned_name

  mapfile -t matches < <(compgen -G "$pattern" | sort -V)
  if (( ${#matches[@]} == 0 )); then
    echo "error: no shared library matched $pattern" >&2
    return 1
  fi

  source=""
  real_source=""
  soname=""
  for (( index = ${#matches[@]} - 1; index >= 0; --index )); do
    candidate="${matches[$index]}"
    [[ -e "$candidate" ]] || continue
    real_source="$(readlink -f "$candidate")"
    [[ -f "$real_source" ]] || continue
    candidate_soname="$("$readelf_bin" -d "$real_source" 2>/dev/null \
      | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p' \
      | head -n 1 || true)"
    if [[ -n "$candidate_soname" ]]; then
      source="$candidate"
      soname="$candidate_soname"
      break
    fi
  done

  if [[ -z "$soname" ]]; then
    echo "error: no ELF shared library with a SONAME matched $pattern" >&2
    return 1
  fi

  real_name="$(basename "$real_source")"
  mkdir -p "$destination"
  cp -f "$real_source" "$destination/$real_name"
  if [[ "$soname" != "$real_name" ]]; then
    ln -sfn "$real_name" "$destination/$soname"
  fi

  if [[ "$soname" == *.so.* ]]; then
    unversioned_name="${soname%%.so.*}.so"
    if [[ "$unversioned_name" != "$soname" ]]; then
      ln -sfn "$soname" "$destination/$unversioned_name"
    fi
  fi

  printf '%s\n' "$destination/$real_name"
}

# webos_pgo_flags COMPONENT PROFILE_DIR
#
# Prints GCC PGO flags for COMPONENT when COMPONENT_PGO_MODE or WEBOS_PGO_MODE is
# set to "generate" or "use". Defaults are intentionally empty so normal release
# builds remain unchanged unless a caller opts in.
webos_pgo_flags() {
  local component="$1"
  local profile_dir="$2"
  local mode_var="${component}_PGO_MODE"
  local mode="${!mode_var:-${WEBOS_PGO_MODE:-}}"

  case "${mode,,}" in
    ""|0|off|none)
      return 0
      ;;
    generate)
      mkdir -p "$profile_dir"
      printf '%s\n' "-fprofile-generate=$profile_dir"
      ;;
    use)
      printf '%s\n' "-fprofile-use=$profile_dir -fprofile-correction -fprofile-partial-training -freorder-functions -Wno-error=missing-profile"
      ;;
    *)
      echo "error: ${mode_var} must be one of: generate, use, off (got '$mode')" >&2
      return 1
      ;;
  esac
}

append_colon_path() {
  local variable="$1"
  local dir="$2"
  local current="${!variable:-}"
  [[ -d "$dir" ]] || return 0
  case ":$current:" in
    *":$dir:"*) ;;
    *) printf -v "$variable" '%s%s%s' "$dir" "${current:+:}" "$current" ;;
  esac
  export "$variable"
}

setup_native_ccache() {
  local root="$1"
  export CCACHE_DIR="${CCACHE_DIR:-$root/.ccache}"
  export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$root}"
  export CCACHE_COMPRESS="${CCACHE_COMPRESS:-1}"
  export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-time_macros,file_macro}"
  mkdir -p "$CCACHE_DIR"
}

is_positive_integer() {
  [[ "${1:-}" =~ ^[1-9][0-9]*$ ]]
}

logical_cpu_count() {
  if is_positive_integer "${SPOOL_BUILD_LOGICAL_CPUS:-}"; then
    printf '%s\n' "$SPOOL_BUILD_LOGICAL_CPUS"
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.logicalcpu 2>/dev/null || printf '1\n'
  else
    printf '1\n'
  fi
}

physical_cpu_count() {
  local logical physical

  if is_positive_integer "${SPOOL_BUILD_PHYSICAL_CPUS:-}"; then
    printf '%s\n' "$SPOOL_BUILD_PHYSICAL_CPUS"
    return 0
  fi

  if command -v sysctl >/dev/null 2>&1; then
    physical="$(sysctl -n hw.physicalcpu 2>/dev/null || true)"
    if is_positive_integer "$physical"; then
      printf '%s\n' "$physical"
      return 0
    fi
  fi

  if [[ -r /proc/cpuinfo ]]; then
    physical="$(awk '
      /^physical id[ \t]*:/ {
        split($0, value, ":")
        gsub(/^[ \t]+|[ \t]+$/, "", value[2])
        socket = value[2]
      }
      /^core id[ \t]*:/ {
        split($0, value, ":")
        gsub(/^[ \t]+|[ \t]+$/, "", value[2])
        core = value[2]
      }
      /^$/ {
        if (socket != "" && core != "") {
          seen[socket ":" core] = 1
        }
        socket = ""
        core = ""
      }
      END {
        for (key in seen) {
          count++
        }
        if (count > 0) {
          print count
        }
      }' /proc/cpuinfo)"
    if is_positive_integer "$physical"; then
      printf '%s\n' "$physical"
      return 0
    fi
  fi

  logical="$(logical_cpu_count)"
  if (( logical > 1 )); then
    printf '%s\n' "$(((logical + 1) / 2))"
  else
    printf '1\n'
  fi
}

memory_bytes_file_to_mib() {
  local file="$1"
  local bytes

  [[ -r "$file" ]] || return 1
  bytes="$(tr -d '[:space:]' < "$file")"
  [[ "$bytes" =~ ^[0-9]+$ ]] || return 1
  # Very large cgroup values mean "effectively unlimited"; avoid overflowing
  # shell arithmetic and fall back to the host memory total instead.
  (( ${#bytes} <= 15 )) || return 1
  (( bytes > 0 )) || return 1
  printf '%s\n' "$(((bytes + 1048575) / 1048576))"
}

memory_limit_mib() {
  local memory_mib memory_bytes

  if is_positive_integer "${SPOOL_BUILD_MEMORY_LIMIT_MIB:-}"; then
    printf '%s\n' "$SPOOL_BUILD_MEMORY_LIMIT_MIB"
    return 0
  fi

  memory_mib="$(memory_bytes_file_to_mib /sys/fs/cgroup/memory.max || true)"
  if is_positive_integer "$memory_mib"; then
    printf '%s\n' "$memory_mib"
    return 0
  fi

  memory_mib="$(memory_bytes_file_to_mib /sys/fs/cgroup/memory/memory.limit_in_bytes || true)"
  if is_positive_integer "$memory_mib"; then
    printf '%s\n' "$memory_mib"
    return 0
  fi

  if [[ -r /proc/meminfo ]]; then
    memory_mib="$(awk '/^MemTotal:/ { print int(($2 + 1023) / 1024); exit }' /proc/meminfo)"
    if is_positive_integer "$memory_mib"; then
      printf '%s\n' "$memory_mib"
      return 0
    fi
  fi

  if command -v sysctl >/dev/null 2>&1; then
    memory_bytes="$(sysctl -n hw.memsize 2>/dev/null || true)"
    if [[ "$memory_bytes" =~ ^[0-9]+$ ]] && (( ${#memory_bytes} <= 15 )) && (( memory_bytes > 0 )); then
      printf '%s\n' "$(((memory_bytes + 1048575) / 1048576))"
      return 0
    fi
  fi

  printf '4096\n'
}

recommended_parallel_jobs() {
  local per_job_mib="${1:-${SPOOL_BUILD_MEMORY_PER_JOB_MIB:-1536}}"
  local reserve_mib="${2:-${SPOOL_BUILD_MEMORY_RESERVE_MIB:-2048}}"
  local min_jobs="${SPOOL_BUILD_MIN_JOBS:-1}"
  local max_jobs="${SPOOL_BUILD_MAX_JOBS:-}"
  local logical physical memory_mib memory_jobs jobs

  if [[ -n "${JOBS:-}" ]]; then
    is_positive_integer "$JOBS" || {
      echo "error: JOBS must be a positive integer, got '$JOBS'" >&2
      return 1
    }
    printf '%s\n' "$JOBS"
    return 0
  fi

  is_positive_integer "$per_job_mib" || per_job_mib=1536
  is_positive_integer "$reserve_mib" || reserve_mib=2048
  is_positive_integer "$min_jobs" || min_jobs=1
  if [[ -n "$max_jobs" ]] && ! is_positive_integer "$max_jobs"; then
    echo "error: SPOOL_BUILD_MAX_JOBS must be a positive integer, got '$max_jobs'" >&2
    return 1
  fi

  logical="$(logical_cpu_count)"
  physical="$(physical_cpu_count)"
  memory_mib="$(memory_limit_mib)"

  if (( memory_mib > reserve_mib )); then
    memory_jobs="$(((memory_mib - reserve_mib) / per_job_mib))"
  else
    memory_jobs=1
  fi
  (( memory_jobs >= 1 )) || memory_jobs=1

  jobs="$logical"
  if (( memory_mib < 16384 && physical > 0 && jobs > physical )); then
    jobs="$physical"
  fi
  if (( jobs > memory_jobs )); then
    jobs="$memory_jobs"
  fi
  if [[ -n "$max_jobs" ]] && (( jobs > max_jobs )); then
    jobs="$max_jobs"
  fi
  if (( jobs < min_jobs )); then
    jobs="$min_jobs"
  fi
  if (( jobs > logical )); then
    jobs="$logical"
  fi

  printf '%s\n' "$jobs"
}

describe_parallel_jobs() {
  local jobs="$1"
  local label="${2:-build}"
  local per_job_mib="${3:-${SPOOL_BUILD_MEMORY_PER_JOB_MIB:-1536}}"
  local reserve_mib="${4:-${SPOOL_BUILD_MEMORY_RESERVE_MIB:-2048}}"

  printf 'Using %s parallel %s jobs (logical CPUs: %s, physical cores: %s, memory limit: %s MiB, per-job: %s MiB, reserve: %s MiB)\n' \
    "$jobs" "$label" "$(logical_cpu_count)" "$(physical_cpu_count)" "$(memory_limit_mib)" \
    "$per_job_mib" "$reserve_mib" >&2
}

native_mpv_args() {
  local prefix="$1"
  local build_type="$2"
  local platform="$3"
  local manifest="${MPV_NATIVE_MANIFEST:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/manifests/mpv-native.json}"
  command -v jq >/dev/null 2>&1 || {
    echo "error: jq is required to read $manifest" >&2
    return 1
  }
  [[ -f "$manifest" ]] || {
    echo "error: mpv feature manifest not found: $manifest" >&2
    return 1
  }

  local feature_args=()
  mapfile -t feature_args < <(
    jq -er --arg platform "$platform" \
      '(.common + .platforms[$platform])[] | "-D" + .' "$manifest"
  )
  MPV_NATIVE_ARGS=(
    --prefix "$prefix"
    --libdir lib
    --buildtype "$build_type"
    --default-library shared
    "${feature_args[@]}"
  )
}

# prune_stale_mpv_libraries PREFIX BUILD_DIR
#
# Drop installed libmpv artifacts that the current build no longer produces, so
# a soname bump cannot leave an old library behind for the linker to pick up.
# Everything the build still produces is left in place: meson installs those
# only when they change, and a fresh timestamp on libmpv relinks every
# executable that links it, which is most of the tree.
prune_stale_mpv_libraries() {
  local prefix="$1" build="$2"
  local installed name
  local matches=()
  mapfile -t matches < <(compgen -G "$prefix/lib/libmpv.so*" || true)
  mapfile -t -O "${#matches[@]}" matches < <(compgen -G "$prefix/lib/libmpv*.dylib" || true)
  for installed in "${matches[@]}"; do
    name="$(basename "$installed")"
    [[ -e "$build/$name" ]] && continue
    rm -f "$installed"
  done
}

first_missing_nix_store_path() {
  local file="$1"
  python3 - "$file" <<'PY'
import pathlib
import re
import sys

content = pathlib.Path(sys.argv[1]).read_text(errors="replace")
paths = set(re.findall(r"/nix/store/[a-z0-9]{32}-[^\s$\"',;:]+", content))
for path in sorted(candidate.rstrip("\\)]}") for candidate in paths):
    if not pathlib.Path(path).exists():
        print(path)
        raise SystemExit(0)
raise SystemExit(1)
PY
}

# mpv_meson_build SRC BUILD_DIR [meson setup args...]
#
# Configure + compile + install an mpv tree. Wipes BUILD_DIR first if it was
# previously configured against a different source path (meson bakes the source
# path into meson-info.json and refuses a plain --reconfigure across a move).
# The install --prefix must be passed in the setup args by the caller.
mpv_meson_build() {
  local src="$1" build="$2"
  shift 2
  local setup_args=("$@")

  if [[ -f "$build/meson-info/meson-info.json" ]]; then
    local cached_src
    cached_src="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("directories",{}).get("source",""))' \
      "$build/meson-info/meson-info.json" 2>/dev/null || true)"
    if [[ -n "$cached_src" && "$cached_src" != "$src" ]]; then
      echo "mpv build dir cached with stale source path ($cached_src != $src); wiping" >&2
      rm -rf "$build"
    fi
  fi

  local missing_dependency_path=""
  if [[ -f "$build/build.ninja" ]] \
      && missing_dependency_path="$(first_missing_nix_store_path "$build/build.ninja")"; then
    echo "mpv build dir references a missing Nix dependency ($missing_dependency_path); wiping" >&2
    rm -rf "$build"
  fi

  local dependency_fingerprint dependency_marker cached_fingerprint=""
  local pkg_config_executable
  dependency_marker="$build/.jellyfin-dependency-env"
  pkg_config_executable="$(command -v pkg-config)"
  dependency_fingerprint="$(printf '2\n%s\n%s\n%s\n%s\n' \
    "$pkg_config_executable" "${PKG_CONFIG_PATH:-}" "${CMAKE_PREFIX_PATH:-}" \
    "${setup_args[*]}" | sha256sum)"
  dependency_fingerprint="${dependency_fingerprint%% *}"
  [[ -f "$dependency_marker" ]] && read -r cached_fingerprint <"$dependency_marker"

  if [[ ! -f "$build/build.ninja" ]]; then
    meson setup "$build" "$src" "${setup_args[@]}"
  elif [[ "$cached_fingerprint" != "$dependency_fingerprint" ]]; then
    # Meson can retain resolved dependency objects after --clearcache. A
    # changed Nix shell must start clean or it may keep linking an old ABI.
    echo "mpv dependency environment changed; wiping cached build directory" >&2
    rm -rf "$build"
    meson setup "$build" "$src" "${setup_args[@]}"
  fi
  # A matching fingerprint means this build dir is already configured for these
  # inputs. Edits to meson.build still land: ninja reruns meson setup itself.
  printf '%s\n' "$dependency_fingerprint" >"$dependency_marker"
  local jobs
  jobs="$(recommended_parallel_jobs)"
  meson compile -C "$build" -j "$jobs"
  meson install -C "$build" --no-rebuild --only-changed
}

path_under_any_root() {
  local path="$1"
  shift
  local root
  for root in "$@"; do
    [[ -n "$root" ]] || continue
    [[ "$path" == "$root" || "$path" == "$root"/* ]] && return 0
  done
  return 1
}

cmake_cache_has_stale_qt_prefix() {
  local cache="$1"
  [[ -n "${CMAKE_PREFIX_PATH:-}" ]] || return 1
  local active_qt_dir="${SPOOL_QT_CMAKE_DIR:-}"
  if [[ -z "$active_qt_dir" ]] && command -v qtpaths >/dev/null 2>&1; then
    active_qt_dir="$(qtpaths --query QT_INSTALL_LIBS 2>/dev/null)/cmake/Qt6"
  fi


  local roots=()
  IFS=':;' read -r -a roots <<< "${CMAKE_PREFIX_PATH:-}"

  local key value
  while IFS='=' read -r key value; do
    [[ "$key" == Qt6*"_DIR:PATH" ]] || continue
    if [[ "$key" == "Qt6_DIR:PATH" && -n "$active_qt_dir" && "$value" != "$active_qt_dir" ]]; then
      echo "app build dir cached with inactive Qt package: $value" >&2
      return 0
    fi
    [[ "$value" == /nix/store/* ]] || continue
    if ! path_under_any_root "$value" "${roots[@]}"; then
      echo "app build dir cached with stale Qt prefix: $value" >&2
      return 0
    fi
  done < "$cache"

  return 1
}

# cmake_build_app SRC BUILD_DIR [cmake configure args...]
#
# Configure (Ninja) + build + install the app. Wipes BUILD_DIR first if its
# CMakeCache.txt points at a different source tree, a Qt package outside the
# current CMAKE_PREFIX_PATH or active qtpaths installation, or a generated
# Ninja rule references a collected Nix store dependency.
cmake_build_app() {
  local src="$1" build="$2"
  shift 2
  local cmake_args=("$@")
  if [[ -n "${SPOOL_CMAKE_EXTRA_ARGS:-}" ]]; then
    # Shell-style splitting is intentional: this is for simple -Dname=value
    # switches from flake runners and local diagnostics.
    # shellcheck disable=SC2206
    cmake_args+=(${SPOOL_CMAKE_EXTRA_ARGS})
  fi
  if [[ -n "${SPOOL_QT_CMAKE_DIR:-}" ]]; then
    cmake_args+=("-DQt6_DIR=$SPOOL_QT_CMAKE_DIR")
  fi

  local missing_dependency_path=""
  if [[ -f "$build/CMakeCache.txt" ]]; then
    if ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$src$" "$build/CMakeCache.txt"; then
      echo "app build dir cached with stale source path; wiping" >&2
      rm -rf "$build"
    elif cmake_cache_has_stale_qt_prefix "$build/CMakeCache.txt"; then
      echo "app build dir cached with stale Qt package paths; wiping" >&2
      rm -rf "$build"
    elif [[ -f "$build/build.ninja" ]] \
        && missing_dependency_path="$(first_missing_nix_store_path "$build/build.ninja")"; then
      echo "app build dir references a missing Nix dependency ($missing_dependency_path); wiping" >&2
      rm -rf "$build"
    fi
  fi
  mkdir -p "$build"

  # Reconfiguring costs a couple of seconds on every launch and only matters
  # when the arguments move. The project's own CMakeLists.txt is folded in as
  # well: the generated ninja file does rerun cmake when it changes, but only
  # a reconfigure here tells us that the build tree's moc output may have been
  # generated under different settings, which is what the autogen reset below
  # exists for.
  local configure_fingerprint configure_marker cached_configure=""
  configure_marker="$build/.jellyfin-configure-args"
  configure_fingerprint="$({
    printf '%s\n' "${cmake_args[*]}"
    sha256sum <"$src/CMakeLists.txt"
  } | sha256sum)"
  configure_fingerprint="${configure_fingerprint%% *}"
  [[ -f "$configure_marker" ]] && read -r cached_configure <"$configure_marker"
  if [[ ! -f "$build/build.ninja" || "$cached_configure" != "$configure_fingerprint" ]]; then
    cmake -S "$src" -B "$build" -GNinja "${cmake_args[@]}"
    # Compile definitions and include paths reach moc through autogen's own
    # settings file, which is not an input the generator tracks: a build tree
    # carried over from an earlier configure (a restored CI cache, a tree that
    # changed branches) can recompile every source against new flags while
    # keeping moc output generated under the old ones. Dropping the timestamps
    # makes the next build regenerate it. Costs seconds, and only when the
    # configuration actually moved.
    rm -f "$build"/*_autogen/timestamp
    printf '%s\n' "$configure_fingerprint" >"$configure_marker"
  fi
  local jobs
  jobs="$(recommended_parallel_jobs)"
  cmake --build "$build" --parallel "$jobs"
  cmake --install "$build"
}

# resolve_qmlimportscanner [BUILD_NINJA]
#
# Print the path to a usable qmlimportscanner, or nothing if none is found.
# Nix splits Qt tools across outputs, so qmlimportscanner is rarely on PATH;
# both the AppImage packager (linuxdeploy-plugin-qt) and macdeployqt need it.
# Search order: PATH, qtpaths libexec dirs, CMAKE_PREFIX_PATH roots, the app's
# build.ninja (if given), then a last-resort /nix/store scan.
resolve_qmlimportscanner() {
  local build_ninja="${1:-}"
  local scanner
  scanner="$(command -v qmlimportscanner || true)"
  if [[ -n "$scanner" ]]; then
    printf '%s\n' "$scanner"
    return 0
  fi

  local roots=()
  if command -v qtpaths6 >/dev/null 2>&1; then
    roots+=("$(qtpaths6 -query QT_HOST_LIBEXECS 2>/dev/null || true)")
    roots+=("$(qtpaths6 -query QT_INSTALL_LIBEXECS 2>/dev/null || true)")
  elif command -v qtpaths >/dev/null 2>&1; then
    roots+=("$(qtpaths -query QT_HOST_LIBEXECS 2>/dev/null || true)")
    roots+=("$(qtpaths -query QT_INSTALL_LIBEXECS 2>/dev/null || true)")
  fi
  local cmake_roots=()
  IFS=':;' read -r -a cmake_roots <<< "${CMAKE_PREFIX_PATH:-}"
  roots+=("${cmake_roots[@]}")

  local root candidate
  for root in "${roots[@]}"; do
    [[ -n "$root" ]] || continue
    for candidate in \
      "$root/qmlimportscanner" \
      "$root/bin/qmlimportscanner" \
      "$root/libexec/qmlimportscanner" \
      "$root/lib/qt-6/libexec/qmlimportscanner"; do
      if [[ -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  done

  if [[ -n "$build_ninja" && -f "$build_ninja" ]]; then
    scanner="$(awk '
      /qmlimportscanner/ {
        for (i = 1; i <= NF; ++i) {
          gsub(/^"|"$/, "", $i)
          if ($i ~ /\/qmlimportscanner$/) { print $i; exit }
        }
      }' "$build_ninja")"
    if [[ -n "$scanner" ]]; then
      printf '%s\n' "$scanner"
      return 0
    fi
  fi

  # Last resort (Nix): match the scanner to the active Qt version so we never
  # hand back a stale, ABI-incompatible build from an unrelated store path. A
  # mismatched qmlimportscanner can run fine here yet fail under the mutated
  # LD_LIBRARY_PATH the packagers set up later, so an unpinned scan is unsafe.
  local qt_ver=""
  if command -v qtpaths6 >/dev/null 2>&1; then
    qt_ver="$(qtpaths6 -query QT_VERSION 2>/dev/null || true)"
  elif command -v qtpaths >/dev/null 2>&1; then
    qt_ver="$(qtpaths -query QT_VERSION 2>/dev/null || true)"
  fi
  if [[ -n "$qt_ver" ]]; then
    # A shell glob over the store's top level is bounded and fast; a recursive
    # `find /nix/store` would stat the entire (multi-hundred-GB) store.
    for candidate in /nix/store/*-qtdeclarative-"$qt_ver"*/libexec/qmlimportscanner; do
      if [[ -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  fi

  return 0
}

# Default codegen tuning for webOS TV targets (202x models: AArch64 SoCs
# running a 32-bit userspace on Cortex-A53/A55/A73-class cores). Thumb-2 cuts
# text size ~25-30% at equal-or-better speed (smaller I-cache footprint);
# neon-fp-armv8 unlocks the ARMv8 FPU/NEON from AArch32. The float ABI stays
# softfp from the toolchain default: it must match the system libraries.
webos_tune_cflags() {
  echo "${WEBOS_TUNE_CFLAGS:--mthumb -mcpu=cortex-a53 -mfpu=neon-fp-armv8}"
}
