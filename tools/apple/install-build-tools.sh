#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

formulae=()
packages=()
while IFS= read -r entry; do
  case "$entry" in
    ''|\#*) ;;
    *==*) packages+=("$entry") ;;
    *) formulae+=("$entry") ;;
  esac
done <"$ROOT/tools/manifests/macos-tools.txt"

missing=()
for formula in "${formulae[@]}"; do
  if ! brew list --formula "$formula" >/dev/null 2>&1; then
    missing+=("$formula")
  fi
done
if [[ -n "${missing[0]:-}" ]]; then
  HOMEBREW_NO_AUTO_UPDATE=1 brew install "${missing[@]}"
fi
python3 -m venv "$RUNNER_TEMP/apple-build-tools"
"$RUNNER_TEMP/apple-build-tools/bin/pip" install "${packages[@]}"
echo "$RUNNER_TEMP/apple-build-tools/bin" >>"$GITHUB_PATH"
