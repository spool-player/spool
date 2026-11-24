#!/usr/bin/env bash
set -euo pipefail

[[ $# -gt 0 ]] || { echo 'usage: retry-gradle-download.sh COMMAND [ARG...]' >&2; exit 2; }
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
for delay in 30 90 0; do
  if "$@" 2>&1 | tee "$log"; then
    exit 0
  else
    status=("${PIPESTATUS[@]}")
  fi
  if (( status[1] != 0 )); then
    exit "${status[1]}"
  fi
  if (( delay == 0 )) || ! grep -Eq \
      "Could not GET 'https://repo\.maven\.apache\.org/[^']+'\. Received status code 429 from server" "$log"; then
    exit "${status[0]}"
  fi
  printf 'Maven Central rate-limited Gradle; retrying the incremental build in %s seconds.\n' "$delay" >&2
  sleep "$delay"
done
