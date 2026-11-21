#!/usr/bin/env bash
# Explicit on-device measurement using a previously installed benchmark IPK.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"
HOST="${TV_HOST:-${SPOOL_TV_HOST:-}}"
APP_ID="${APP_ID:-com.sachk.spool}"
MODE=routes
ITERATIONS=6
LIBRARY=Movies
COLD=0
LIST_MODE=0
SCROLL_STEPS=0
OUT_DIR="${SPOOL_BENCH_DIR:-$ROOT/dist/tv-benchmark}"
WAIT_SECONDS=240
LUNA_BIN="${LUNA_BIN:-luna-send}"
usage() {
  cat <<EOF
Usage: tools/webos/bench-device.sh --host HOST [options]

Explicitly closes/launches the target app for a benchmark, then closes it.
Requires the IPK from tools/webos/package-heaptrack-profile.sh --benchmark-only
(or the heaptrack profiling IPK). Never patches installed files, chowns app data,
changes inputs, or wakes the panel. The panel must already be Active.

  --host HOST          SSH target (or TV_HOST / SPOOL_TV_HOST)
  --mode MODE          benchmark mode (default: routes)
  --script ROUTES      comma-separated custom route walk
  --iterations N       measured passes (default: 6)
  --library NAME       library to open (default: Movies)
  --cold               cold route passes
  --list               use list mode
  --scroll-steps N     scroll steps (default: 0)
  --out DIR            local reports (default: dist/tv-benchmark)
  --wait SEC           report deadline (default: 240)
  -h, --help           show help
EOF
}
while (( $# )); do
  case "$1" in
    --host|--mode|--script|--iterations|--library|--scroll-steps|--out|--wait)
      webos_require_value "$@"
      case "$1" in
        --host) HOST="$2" ;; --mode|--script) MODE="$2" ;;
        --iterations) ITERATIONS="$2" ;; --library) LIBRARY="$2" ;;
        --scroll-steps) SCROLL_STEPS="$2" ;; --out) OUT_DIR="$2" ;; --wait) WAIT_SECONDS="$2" ;;
      esac
      shift 2 ;;
    --cold) COLD=1; shift ;;
    --list) LIST_MODE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) webos_error "unknown argument: $1" ;;
  esac
done
webos_app_id "$APP_ID"
webos_uint --iterations "$ITERATIONS" 1 100
webos_uint --scroll-steps "$SCROLL_STEPS" 0 10000
webos_uint --wait "$WAIT_SECONDS" 1 3600
[[ "$MODE" =~ ^[a-zA-Z0-9,_-]+$ ]] || webos_error 'invalid benchmark mode/route walk'
[[ "$LIBRARY" != *$'\n'* && "$LIBRARY" != *$'\r'* ]] || webos_error 'library must be one line'
webos_transport_init "$HOST"
webos_require_command python3
webos_require_command scp
mkdir -p "$OUT_DIR"
label="${MODE//,/-}"
(( ! COLD )) || label+="-cold"
(( COLD )) || label+="-warm"
(( ! LIST_MODE )) || label+="-list"
remote_out="/tmp/spool-bench-$$.json"
marker=/tmp/spool-benchmark.on
local_out="$OUT_DIR/$label.json"
armed=0
launched=0
luna() {
  local result
  result="$(webos_luna "$HOST" "$1" "${2:-\{\}}")" || return
  python3 "$ROOT/tools/webos/luna-response.py" success <<<"$result"
}
cleanup() {
  local status=$?
  if (( armed )); then
    webos_ssh -n "$HOST" "rm -f '$marker' '$remote_out'" >/dev/null 2>&1 || status=1
  fi
  if (( launched )); then
    luna luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$APP_ID\"}" || status=1
  fi
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
power_state() {
  webos_luna "$HOST" luna://com.webos.service.tvpower/power/getPowerState '{}' | python3 -c '
import json,sys
reply=json.load(sys.stdin)
if reply.get("returnValue") is not True:
    raise SystemExit(1)
print(reply.get("state", "unknown"))'
}
[[ "$(power_state)" == Active ]] || { echo 'panel must already be Active; refusing to wake it' >&2; exit 1; }
webos_ssh -n "$HOST" "test -x /media/cryptofs/apps/usr/palm/applications/$APP_ID/bin/spool.real && test -f /media/cryptofs/apps/usr/palm/applications/$APP_ID/bin/.spool-benchmark-v1" \
  || { echo 'install the benchmark/profiling IPK first; installed files are never patched' >&2; exit 1; }
luna luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$APP_ID\"}"
config=""
for value in spool-benchmark-v1 "$MODE" "$ITERATIONS" "$LIBRARY" "$COLD" "$LIST_MODE" "$SCROLL_STEPS" "$remote_out"; do
  config+=" $(webos_quote "$value")"
done
armed=1
webos_ssh -n "$HOST" "umask 022; printf '%s\\n'$config > '$marker'"
launched=1
luna luna://com.webos.applicationManager/launch "{\"id\":\"$APP_ID\"}"
deadline=$(( SECONDS + 10#$WAIT_SECONDS ))
while ! webos_ssh -n "$HOST" "test -s '$remote_out'"; do
  (( SECONDS < deadline )) || { echo 'benchmark report deadline expired' >&2; exit 1; }
  sleep 2
done
webos_scp "$HOST:$remote_out" "$local_out"
[[ "$(power_state)" == Active ]] || { echo "panel slept during run; discard $local_out" >&2; exit 1; }
python3 "$ROOT/tools/compare-render-benchmark.py" "$local_out"
python3 "$ROOT/tools/webos/summarise-device-benchmark.py" "$local_out"
echo "Report: $local_out"
