#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY_RUN=0
case "${1:-}" in
  --dry-run) DRY_RUN=1 ;;
  --help|-h)
    printf 'Usage: %s [--dry-run]\nBuild the full webOS IPK with bundled sibling provider working trees only.\n' "$0"
    exit 0
    ;;
  '') ;;
  *) printf 'error: unsupported argument: %s\n' "$1" >&2; exit 2 ;;
esac
if (( $# > 1 )); then
  printf 'error: expected at most --dry-run\n' >&2
  exit 2
fi

SPOOL_PROVIDER_OVERRIDES="$(python3 "$ROOT/tools/local-provider-overrides.py" --root "$ROOT")"
export SPOOL_PROVIDER_SOURCES=bundled
export SPOOL_PROVIDER_OVERRIDES
IFS=';' read -r -a OVERRIDES <<< "$SPOOL_PROVIDER_OVERRIDES"

if (( DRY_RUN )); then
  printf 'Working directory: %s\n' "$ROOT"
  printf 'Provider sources: %s\n' "$SPOOL_PROVIDER_SOURCES"
  if (( ${#OVERRIDES[@]} )); then
    printf 'Working tree: %s\n' "${OVERRIDES[@]}"
  else
    printf 'No sibling provider manifests found; using bundled release pins.\n'
  fi
  printf 'Command: cd %q && env SPOOL_PROVIDER_SOURCES=%q SPOOL_PROVIDER_OVERRIDES=%q ./build-ipk.sh\n' \
    "$ROOT" "$SPOOL_PROVIDER_SOURCES" "$SPOOL_PROVIDER_OVERRIDES"
  printf 'Pipeline: fetch -> qt-host -> qt-target -> dependencies -> app -> stage -> package\n'
  printf 'Dry run only: no configure, build, download, staging, packaging or deployment.\n'
  exit 0
fi

cd "$ROOT"
exec ./build-ipk.sh
