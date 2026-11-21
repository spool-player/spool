#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"
if [[ $# == 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  echo "Usage: $0 root@tv.local (APP_ID and OUT may be set)"
  echo 'Streams a read-only diagnostic archive; never stages files on the device.'
  exit 0
fi
[[ $# == 1 ]] || webos_error "Usage: $0 root@tv.local"
TARGET="$1"
APP_ID="${APP_ID:-com.sachk.spool}"
OUT="${OUT:-diagnostics-bundle-$(date -u +%Y%m%d-%H%M%S).tar.gz}"
webos_app_id "$APP_ID"
webos_transport_init "$TARGET"
mkdir -p "$(dirname "$OUT")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
webos_ssh -n "$TARGET" ps >"$work/ps.txt"
# tar reads only existing paths and streams straight to the desktop. No remote
# rm/mkdir/cp/archive is needed even when collection fails or is interrupted.
webos_ssh "$TARGET" "APP_ID='$APP_ID' sh -s" >"$work/logs.tar.gz" <<'REMOTE'
set -e
set --
for path in "tmp/$APP_ID-diagnostics" "var/palm/data/$APP_ID/diagnostics" \
  "tmp/$APP_ID" "media/cryptofs/apps/usr/palm/applications/$APP_ID/.cache/logs" \
  "tmp/$APP_ID.log" tmp/spool-mpv.log; do
  [ ! -e "/$path" ] || set -- "$@" "$path"
done
[ "$#" -gt 0 ] || { echo 'no diagnostic files found' >&2; exit 1; }
tar -czf - -C / "$@"
REMOTE
# Keep the remote archive nested: no extraction of device-controlled paths.
tar -czf "$work/bundle.tar.gz" -C "$work" ps.txt logs.tar.gz
mv "$work/bundle.tar.gz" "$OUT"
printf 'Wrote %s (ps.txt and logs.tar.gz; may contain sensitive logs)\n' "$OUT"
