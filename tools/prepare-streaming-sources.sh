#!/usr/bin/env bash
set -euo pipefail
usage() { echo "Usage: $0 --root ABS --output ABS [--manifest ABS]"; }
ROOT= OUTPUT= MANIFEST=
while (( $# )); do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --root|--output|--manifest)
      (( $# >= 2 )) || { usage >&2; exit 2; }
      case "$1" in --root) ROOT="$2";; --output) OUTPUT="$2";; --manifest) MANIFEST="$2";; esac
      shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
[[ "$ROOT" == /* && "$OUTPUT" == /* && ( -z "$MANIFEST" || "$MANIFEST" == /* ) ]] || { usage >&2; exit 2; }
MANIFEST="${MANIFEST:-$ROOT/tools/manifests/streaming-third-party.json}"
[[ -f "$MANIFEST" && -f "$ROOT/tools/lib/manifest-sources.sh" ]] || { echo 'error: missing manifest or source helpers' >&2; exit 2; }
source "$ROOT/tools/lib/manifest-sources.sh"
# A changed patch must re-extract pristine source; reverse-applying an older
# patch cannot establish that the new patch's preimage still exists.
fingerprint="$("$MANIFEST_PYTHON" "$ROOT/tools/streaming-fingerprint.py" --root "$ROOT" --manifest "$MANIFEST" --target sources)"
for name in openssl nghttp2 nghttp3 ngtcp2 curl; do
  marker="$OUTPUT/$name/.spool-preparation-fingerprint"
  if [[ -f "$OUTPUT/$name/.jellyfin-source-sha256" && ( ! -f "$marker" || "$(<"$marker")" != "$fingerprint" ) ]]; then
    rm -f "$OUTPUT/$name/.jellyfin-source-sha256"
  fi
  prepare_manifest_source "$ROOT" "$MANIFEST" "$name" "$OUTPUT/$name"
  printf '%s\n' "$fingerprint" >"$marker"
done
