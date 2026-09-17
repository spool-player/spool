#!/usr/bin/env bash
# Every workflow artifact is either something a person may be handed or a build
# input for a later job, and the release tells them apart by name alone.
#
# It downloads spool-* with merge-multiple, flattens whatever arrives into one
# directory, and publishes every file at the top of it. So an artifact that
# holds loose files -- a Gradle project, say -- would put build.gradle and
# AndroidManifest.xml on the release page, and two of them would collide.
#
# Hence the rule this checks: publishable artifacts are spool-*, everything
# else is internal-*, and the release download only ever asks for the former.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/.github/workflows/build-artifacts.yml"
release="$root/.github/workflows/release.yml"
status=0

# Artifact names, not the cachix action's own `name:` input, so only the lines
# that sit under an upload-artifact step count.
names="$(awk '
  /uses: actions\/upload-artifact/ { in_upload = 1; next }
  in_upload && /^[[:space:]]*name:/ {
    sub(/^[[:space:]]*name:[[:space:]]*/, "")
    print
    in_upload = 0
    next
  }
  in_upload && /^[[:space:]]*-[[:space:]]/ { in_upload = 0 }
' "$build")"

[[ -n "$names" ]] || {
  echo "error: found no uploaded artifacts to check in build-artifacts.yml" >&2
  exit 1
}

while IFS= read -r name; do
  case "$name" in
    spool-*|internal-*) ;;
    *)
      printf 'error: artifact %s is neither spool-* (published) nor internal-* (a build input)\n' \
        "$name" >&2
      status=1
      ;;
  esac
done <<<"$names"

grep -Fq 'pattern: spool-*' "$release" || {
  echo "error: the release download must ask for spool-* only, or internal artifacts reach the release page" >&2
  status=1
}

if [[ $status -eq 0 ]]; then
  printf 'checked %d workflow artifacts\n' "$(wc -l <<<"$names")"
fi
exit "$status"
