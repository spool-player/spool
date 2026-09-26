#!/usr/bin/env bash
# Fails when core reaches into a native provider. Everything under src/ is
# core except src/providers/ and the composition root, src/main.cpp; core
# reaches media only through src/provider/.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

violations=$(grep -rnE '^\s*#\s*include\s+"([^"]*/)?providers/' src \
  --include='*.cpp' --include='*.h' --include='*.mm' \
  | grep -v -e '^src/providers/' -e '^src/main\.cpp:' || true)
if [[ -n "$violations" ]]; then
  echo "core includes a native provider:" >&2
  echo "$violations" >&2
  exit 1
fi
echo "module seam: ok"
