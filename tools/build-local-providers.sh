#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dry_run=0
case "${1:-}" in
  --dry-run) dry_run=1; shift ;;
  --help|-h)
    printf '%s\n' 'Usage: bash tools/build-local-providers.sh [--dry-run]' \
      'Build the native checkout with sibling providers; never launch.'
    exit 0
    ;;
esac
if (( $# != 0 )); then
  echo "error: unexpected argument: $1" >&2
  exit 2
fi

# Keep provider builds away from normal native outputs and their old CMake paths.
case "$(uname -s)" in
  Linux)
    export BUILD_ROOT="$REPO_ROOT/build/linux-release-local-providers"
    build_script="$REPO_ROOT/tools/build-linux-release.sh"
    export APP_INSTALL="$BUILD_ROOT/install"
    ;;
  Darwin)
    export BUILD_ROOT="$REPO_ROOT/build/macos-local-providers"
    build_script="$REPO_ROOT/tools/build-macos.sh"
    export APP_INSTALL="$BUILD_ROOT/run-install"
    export DEPLOY_APP=0
    ;;
  *) echo 'error: local native providers require Linux or macOS' >&2; exit 1 ;;
esac
export APP_BUILD="$BUILD_ROOT/app"
export MPV_BUILD="$BUILD_ROOT/mpv"
export MPV_PREFIX="$BUILD_ROOT/mpv-prefix"
export MPV_SRC="$REPO_ROOT/mpv"
export SPOOL_PROVIDER_SOURCES=bundled
SPOOL_PROVIDER_OVERRIDES="$(python3 "$REPO_ROOT/tools/local-provider-overrides.py" --root "$REPO_ROOT")"
export SPOOL_PROVIDER_OVERRIDES
# This workflow owns provider policy; do not inherit unrelated CMake overrides.
unset SPOOL_CMAKE_EXTRA_ARGS

if (( dry_run )); then
  printf 'Repository: %s\nBuild root: %s\nProvider sources: %s\nProvider overrides: %s\nBuild command: bash %q\n' \
    "$REPO_ROOT" "$BUILD_ROOT" "$SPOOL_PROVIDER_SOURCES" "$SPOOL_PROVIDER_OVERRIDES" "$build_script"
  exit 0
fi

exec bash "$build_script"
