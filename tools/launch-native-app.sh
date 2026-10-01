#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<EOF
usage: $(basename "$0") [path-to-spool-or-app-bundle]

Runs the native app launch test in an isolated environment and requires a rendered frame.

With SPOOL_BENCH set, runs the render benchmark instead of the launch test:
the app comes up the same way and is then walked through route switches,
writing what each one cost to SPOOL_BENCH_OUT.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

bundled_app=0
candidate="${1:-$APP_ROOT/build/linux-release/install/bin/spool}"
if [[ -d "$candidate" && "$candidate" == *.app ]]; then
  bundled_app=1
  candidate="$candidate/Contents/MacOS/Spool"
fi

if [[ ! -x "$candidate" ]]; then
  echo "error: executable not found: $candidate" >&2
  exit 1
fi

work="$(mktemp -d)"
cleanup() {
  rm -rf "$work"
}
trap cleanup EXIT

mkdir -p \
  "$work/home" \
  "$work/cache" \
  "$work/config" \
  "$work/data" \
  "$work/runtime" \
  "$work/diagnostics"
chmod 700 "$work/runtime"

join_colon_paths() {
  local IFS=:
  printf '%s' "$*"
}

filter_qt_version_paths() {
  local input="$1"
  local version="$2"
  local path
  local paths=()
  IFS=: read -r -a paths <<< "$input"
  local filtered=()
  for path in "${paths[@]}"; do
    [[ -d "$path" ]] || continue
    [[ "$path" == *"-${version}/"* || "$path" == *"-${version}" ]] || continue
    filtered+=("$path")
  done
  join_colon_paths "${filtered[@]}"
}

qt_version_subdirs_from_roots() {
  local roots_input="$1"
  local version="$2"
  local suffix="$3"
  local root
  local roots=()
  IFS=: read -r -a roots <<< "$roots_input"
  local matches=()
  for root in "${roots[@]}"; do
    [[ -n "$root" ]] || continue
    [[ "$root" == *"-${version}/"* || "$root" == *"-${version}" ]] || continue
    [[ -d "$root/$suffix" ]] || continue
    matches+=("$root/$suffix")
  done
  join_colon_paths "${matches[@]}"
}

configure_isolated_qt_paths() {
  [[ "${SPOOL_LAUNCH_TEST_INHERIT_QT_PATHS:-0}" != "1" ]] || return 0
  command -v qtpaths6 >/dev/null 2>&1 || return 0

  local qt_version qt_plugins qt_qml
  qt_version="$(qtpaths6 -query QT_VERSION 2>/dev/null || true)"
  qt_plugins="$(qtpaths6 -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
  qt_qml="$(qtpaths6 -query QT_INSTALL_QML 2>/dev/null || true)"
  [[ -n "$qt_version" ]] || return 0

  local qml_paths=()
  local plugin_paths=()
  local filtered_qml filtered_plugins qmake_qml qmake_plugins
  filtered_qml="$(filter_qt_version_paths "${NIXPKGS_QT6_QML_IMPORT_PATH:-}" "$qt_version")"
  filtered_plugins="$(filter_qt_version_paths "${QT_PLUGIN_PATH:-}" "$qt_version")"
  qmake_qml="$(qt_version_subdirs_from_roots "${QMAKEPATH:-}" "$qt_version" "lib/qt-6/qml")"
  qmake_plugins="$(qt_version_subdirs_from_roots "${QMAKEPATH:-}" "$qt_version" "lib/qt-6/plugins")"

  [[ -n "$qt_qml" && -d "$qt_qml" ]] && qml_paths+=("$qt_qml")
  [[ -n "$qmake_qml" ]] && qml_paths+=("$qmake_qml")
  [[ -n "$filtered_qml" ]] && qml_paths+=("$filtered_qml")

  [[ -n "$qt_plugins" && -d "$qt_plugins" ]] && plugin_paths+=("$qt_plugins")
  [[ -n "$qmake_plugins" ]] && plugin_paths+=("$qmake_plugins")
  [[ -n "$filtered_plugins" ]] && plugin_paths+=("$filtered_plugins")

  export QML2_IMPORT_PATH="$(join_colon_paths "${qml_paths[@]}")"
  export QML_IMPORT_PATH="$QML2_IMPORT_PATH"
  export NIXPKGS_QT6_QML_IMPORT_PATH="$QML2_IMPORT_PATH"
  export QT_PLUGIN_PATH="$(join_colon_paths "${plugin_paths[@]}")"
}

# A throwaway profile is the point of this script: the launch test has to prove
# the app comes up for someone who has never run it, and CI has nothing else.
#
# Benchmarking wants the opposite. What a page costs to paint needs a real GPU,
# which needs the compositor socket in the session's own XDG_RUNTIME_DIR, and
# what a library page costs needs a library, which needs a signed-in profile.
# SPOOL_BENCH_HOST_SESSION=1 borrows both from the developer running it. It is
# never set in CI, and it is refused unless a benchmark is what is being run,
# so it cannot quietly turn the launch test into a non-hermetic one.
use_host_session=0
if [[ "${SPOOL_BENCH_HOST_SESSION:-0}" == "1" ]]; then
  if [[ -z "${SPOOL_BENCH:-}" ]]; then
    echo "error: SPOOL_BENCH_HOST_SESSION only applies to benchmark runs" >&2
    exit 1
  fi
  use_host_session=1
fi

if [[ "$use_host_session" == "1" ]]; then
  echo "note: benchmarking against this session's own profile and compositor" >&2
else
  export HOME="$work/home"
  export XDG_CACHE_HOME="$work/cache"
  export XDG_CONFIG_HOME="$work/config"
  export XDG_DATA_HOME="$work/data"
  export XDG_RUNTIME_DIR="$work/runtime"
fi
export SPOOL_DIAGNOSTICS_DIR="$work/diagnostics"
export QT_QPA_PLATFORM="${SPOOL_LAUNCH_TEST_QPA_PLATFORM:-offscreen}"
# Software rasterisation is the default because it is the only thing a headless
# CI runner can do, and the launch test only needs a frame to exist. Measuring
# what a page costs to paint needs a real GPU, so an explicitly set value wins
# here -- including an empty one, which is how Qt is asked for its default RHI
# backend. Hence +x rather than :-, which cannot tell empty from unset.
if [[ -z "${QT_QUICK_BACKEND+x}" ]]; then
  QT_QUICK_BACKEND=software
fi
export QT_QUICK_BACKEND
export QSG_RHI_BACKEND="${QSG_RHI_BACKEND:-opengl}"
if [[ "$bundled_app" == "1" ]]; then
  unset QT_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH NIXPKGS_QT6_QML_IMPORT_PATH QMAKEPATH
  unset DYLD_LIBRARY_PATH DYLD_FRAMEWORK_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_FALLBACK_FRAMEWORK_PATH
else
  configure_isolated_qt_paths
fi
export LC_NUMERIC=C

# Benchmark mode reuses this script's isolation wholesale -- same offscreen
# platform, same throwaway home -- because a measurement taken in a different
# environment from the launch test is not comparable to it.
app_args=(--launch-test)
if [[ -n "${SPOOL_BENCH:-}" ]]; then
  app_args=()
  if [[ -n "${SPOOL_BENCH_OUT:-}" ]]; then
    mkdir -p "$(dirname "$SPOOL_BENCH_OUT")"
    SPOOL_BENCH_OUT="$(cd "$(dirname "$SPOOL_BENCH_OUT")" && pwd)/$(basename "$SPOOL_BENCH_OUT")"
    export SPOOL_BENCH_OUT
  fi
fi

if [[ "$bundled_app" == "1" ]]; then
  env -i \
    HOME="$HOME" \
    PATH=/usr/bin:/bin \
    TMPDIR="$XDG_RUNTIME_DIR" \
    XDG_CACHE_HOME="$XDG_CACHE_HOME" \
    XDG_CONFIG_HOME="$XDG_CONFIG_HOME" \
    XDG_DATA_HOME="$XDG_DATA_HOME" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SPOOL_DIAGNOSTICS_DIR="$SPOOL_DIAGNOSTICS_DIR" \
    QT_QPA_PLATFORM="$QT_QPA_PLATFORM" \
    QT_QUICK_BACKEND="$QT_QUICK_BACKEND" \
    QSG_RHI_BACKEND="$QSG_RHI_BACKEND" \
    LC_NUMERIC="$LC_NUMERIC" \
    SPOOL_BENCH="${SPOOL_BENCH:-}" \
    SPOOL_BENCH_OUT="${SPOOL_BENCH_OUT:-}" \
    SPOOL_BENCH_COLD="${SPOOL_BENCH_COLD:-}" \
    SPOOL_BENCH_ITERATIONS="${SPOOL_BENCH_ITERATIONS:-}" \
    SPOOL_WINDOW_SIZE="${SPOOL_WINDOW_SIZE:-}" \
    "$candidate" "${app_args[@]}"
else
  "$candidate" "${app_args[@]}"
fi
