#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The default Cachix installer follows a nixpkgs release that has dropped
# x86_64-darwin. Resolve the client from the same lock as the native shell.
url="$(python3 - "$root/flake.lock" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    pin = json.load(source)["nodes"]["nixpkgs-x86-darwin"]["locked"]
print(f"github:{pin['owner']}/{pin['repo']}/{pin['rev']}")
PY
)"
cachix="$(nix build --no-link --print-out-paths "$url#cachix")"
test -x "$cachix/bin/cachix"
printf '%s\n' "$cachix/bin" >>"${GITHUB_PATH:?GITHUB_PATH is required}"
