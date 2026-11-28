#!/usr/bin/env bash
# Run without arguments for GitHub Actions outputs, with a dotted path for a
# single pin, or with --check to detect drift in the consumers.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/manifest-sources.sh
source "$ROOT/tools/lib/manifest-sources.sh"

case "${1:-}" in
  ""|--check|--check-nix-archives) exec "$MANIFEST_PYTHON" "$ROOT/tools/toolchain-versions.py" "$@" ;;
  *) toolchain_field "$ROOT" "$1" ;;
esac
