#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"
host="${TV_HOST:-}"
device="${ARES_DEVICE:-}"
app_id="${APP_ID:-com.sachk.spool}"
ipk=""
outdir=""
install=1
launch=0
screenshot=1
launch_wait="${LAUNCH_WAIT:-8}"
http_port="${HTTP_PORT:-18927}"
install_timeout="${INSTALL_TIMEOUT:-120}"
serve_pid=""
install_pid=""
remote_shot=""

usage() {
  cat <<EOF
Usage: tools/webos/verify-device.sh [options] [path/to/app.ipk]

Installs the native package and verifies registration. Launch and screenshot
capture require --launch. No app data or installed app files are changed.
--no-install without --launch only reads registration; it needs no IPK.

Options:
  --host HOST          SSH target (or TV_HOST)
  --device NAME        ares device for --launch (otherwise ares default)
  --app-id ID          app id (default: $app_id)
  --ipk PATH           package (default: newest matching build/dist IPK by mtime)
  --out DIR            artifact directory (default: build/webos/verify/<timestamp>)
  --no-install         skip installation
  --launch             explicitly launch and capture logs/screenshot
  --no-launch          install/verify only (the default)
  --skip-screenshot    skip screenshot when launching
  --launch-wait SEC    post-launch delay, 0..3600 (default: $launch_wait)
  --http-port PORT     package server port (default: $http_port)
  --install-timeout SEC  subscription limit, 1..3600 (default: $install_timeout)
  -h, --help           show help

SSH, scp and ares commands have WEBOS_COMMAND_TIMEOUT seconds each (default 30).
EOF
}
while (( $# )); do
  case "$1" in
    --host|--device|--app-id|--ipk|--out|--launch-wait|--http-port|--install-timeout)
      webos_require_value "$@"
      case "$1" in
        --host) host="$2" ;; --device) device="$2" ;; --app-id) app_id="$2" ;;
        --ipk) [[ -z "$ipk" ]] || webos_error 'IPK supplied more than once'; ipk="$2" ;;
        --out) outdir="$2" ;; --launch-wait) launch_wait="$2" ;;
        --http-port) http_port="$2" ;; --install-timeout) install_timeout="$2" ;;
      esac
      shift 2 ;;
    --no-install) install=0; shift ;;
    --launch) launch=1; shift ;;
    --no-launch) launch=0; shift ;;
    --skip-screenshot) screenshot=0; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) webos_error "unknown option: $1" ;;
    *) [[ -z "$ipk" ]] || webos_error "unexpected extra argument: $1"; ipk="$1"; shift ;;
  esac
done
[[ -n "$host" ]] || webos_error 'missing --host HOST (or TV_HOST)'
webos_app_id "$app_id"
webos_uint --launch-wait "$launch_wait" 0 3600
webos_uint --http-port "$http_port" 1 65535
webos_uint --install-timeout "$install_timeout" 1 3600
webos_transport_init "$host"
webos_require_command python3

if (( install )); then
  if [[ -z "$ipk" ]]; then
    ipk="$(python3 - "$ROOT" "$app_id" <<'PY'
import pathlib, sys
root, app = pathlib.Path(sys.argv[1]), sys.argv[2]
packages = [path for directory in (root / 'build', root / 'dist')
            for path in directory.rglob(f'{app}_*.ipk') if path.is_file()]
if packages:
    print(max(packages, key=lambda path: (path.stat().st_mtime_ns, str(path))))
PY
)"
  fi
  [[ -n "$ipk" && -f "$ipk" ]] || { echo 'no IPK found; pass --ipk PATH or --no-install' >&2; exit 1; }
  ipk="$(realpath -- "$ipk")"
  webos_require_command curl
  webos_require_command ip
  webos_require_command getent
  webos_require_command sha256sum
fi
if (( launch )); then
  webos_require_command ares-launch
  (( ! screenshot )) || webos_require_command scp
fi
[[ -n "$outdir" ]] || outdir="$ROOT/build/webos/verify/$(date -u +%Y%m%d-%H%M%S)"
mkdir -p "$outdir"

stop_process() {
  local pid="$1"
  [[ -n "$pid" ]] || return 0
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}
cleanup() {
  stop_process "$install_pid"
  stop_process "$serve_pid"
  if [[ -n "$remote_shot" ]]; then
    webos_ssh -n "$host" "rm -f $(webos_quote "$remote_shot")" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
response() { python3 "$ROOT/tools/webos/luna-response.py" "$@"; }

if (( install )); then
  package_name="$(basename "$ipk")"
  tv_name="${host##*@}"
  tv_name="${tv_name#[}"; tv_name="${tv_name%]}"
  tv_ip="$(webos_timeout getent ahosts "$tv_name" | awk 'NR == 1 { address=$1 } END { print address }')"
  [[ -n "$tv_ip" ]] || { echo "cannot resolve TV host $tv_name" >&2; exit 1; }
  host_ip="$(webos_timeout ip route get "$tv_ip" 2>/dev/null | awk '{ for (i=1; i<=NF; i++) if ($i == "src") { print $(i+1); exit } }' || true)"
  [[ -n "$host_ip" ]] || { echo 'cannot determine package server address for TV' >&2; exit 1; }
  url="$(python3 - "$host_ip" "$http_port" "$package_name" <<'PY'
import sys, urllib.parse
host, port, name = sys.argv[1:]
if ':' in host:
    host = f'[{host}]'
print(f'http://{host}:{port}/{urllib.parse.quote(name)}')
PY
)"
  package_hash="$(sha256sum "$ipk" | cut -d' ' -f1)"
  payload="$(python3 -c 'import json,sys; print(json.dumps(dict(ipkHash=sys.argv[1],ipkUrl=sys.argv[2],subscribe=True)))' "$package_hash" "$url")"
  python3 -m http.server "$http_port" --bind "$host_ip" --directory "$(dirname "$ipk")" >"$outdir/http-server.log" 2>&1 &
  serve_pid="$!"
  sleep 1
  if ! kill -0 "$serve_pid" 2>/dev/null || ! curl -fsSI --max-time 2 "$url" >/dev/null; then
    echo 'package server failed; see http-server.log' >&2
    exit 1
  fi
  luna_bin="${LUNA_BIN:-luna-send}"
  [[ "$luna_bin" =~ ^[a-zA-Z0-9_./-]+$ && "$luna_bin" != -* ]] || webos_error 'invalid LUNA_BIN'
  : >"$outdir/install.log"
  timeout --signal=TERM --kill-after=5s "${install_timeout}s" "${WEBOS_SSH[@]}" -n -- "$host" \
    "$luna_bin -i -f 'luna://org.webosbrew.hbchannel.service/install' $(webos_quote "$payload")" >"$outdir/install.log" 2>&1 &
  install_pid="$!"
  deadline=$(( SECONDS + 10#$install_timeout ))
  while :; do
    result=0
    response install <"$outdir/install.log" || result=$?
    if (( result == 0 )); then break; fi
    if (( result == 1 )); then echo 'installation rejected; see install.log' >&2; exit 1; fi
    if (( SECONDS >= deadline )) || ! kill -0 "$install_pid" 2>/dev/null; then
      echo 'installation ended without success; see install.log' >&2
      exit 1
    fi
    sleep 1
  done
  stop_process "$install_pid"; install_pid=""
  stop_process "$serve_pid"; serve_pid=""
fi

webos_luna "$host" 'luna://com.webos.applicationManager/listLaunchPoints' '{}' >"$outdir/listLaunchPoints.json" 2>"$outdir/listLaunchPoints.err"
if ! response registration "$app_id" <"$outdir/listLaunchPoints.json"; then
  echo "app $app_id is not registered (or LS2 rejected the query)" >&2
  exit 1
fi
webos_luna "$host" 'luna://com.webos.applicationManager/getAppInfo' "{\"id\":\"$app_id\"}" >"$outdir/getAppInfo.json" 2>"$outdir/getAppInfo.err"
response success <"$outdir/getAppInfo.json" || { echo 'getAppInfo failed' >&2; exit 1; }
if (( ! launch )); then
  echo "Verified $app_id; launch skipped. Artifacts: $outdir"
  exit 0
fi

ares_args=()
[[ -z "$device" ]] || ares_args+=(--device "$device")
webos_timeout ares-launch "${ares_args[@]}" "$app_id" | tee "$outdir/ares-launch.log"
sleep "$launch_wait"
webos_ssh -n "$host" "ps | grep -F '$app_id' | grep -v grep || true" >"$outdir/ps.txt"
for remote in "/tmp/$app_id/$app_id.log" "/tmp/$app_id/spool-mpv.log" \
  "/media/cryptofs/apps/usr/palm/applications/$app_id/.cache/logs/$app_id.log" \
  "/media/cryptofs/apps/usr/palm/applications/$app_id/.cache/logs/spool-mpv.log" \
  "/tmp/$app_id.log" /tmp/spool-mpv.log "/tmp/$app_id-diagnostics/current-instance.json" \
  "/var/palm/data/$app_id/diagnostics/current-instance.json"; do
  local_name="${remote#/}"; local_name="${local_name//\//_}"
  webos_ssh -n "$host" "if [ -f '$remote' ]; then cat '$remote'; fi" >"$outdir/$local_name"
done
if (( screenshot )); then
  remote_shot="/tmp/$app_id-verify-$(date -u +%Y%m%d-%H%M%S)-$$.png"
  payload="{\"path\":\"$remote_shot\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}"
  captured=0
  for service in com.webos.service.capture com.webos.service.tv.capture; do
    if webos_luna "$host" "luna://$service/executeOneShot" "$payload" >"$outdir/screenshot.json" 2>"$outdir/screenshot.err" \
        && response success <"$outdir/screenshot.json"; then captured=1; break; fi
  done
  (( captured )) || { echo 'screenshot capture failed' >&2; exit 1; }
  webos_scp "$host:$remote_shot" "$outdir/screenshot.png"
  [[ -s "$outdir/screenshot.png" ]] || { echo 'screenshot was empty' >&2; exit 1; }
fi
echo "Verification artifacts written to $outdir"
