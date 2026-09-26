#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

host="${TV_HOST:-}"
device="${ARES_DEVICE:-}"
app_id="${APP_ID:-com.sachk.spool}"
ipk=""
outdir=""
install=1
launch=1
screenshot=1
launch_wait="${LAUNCH_WAIT:-8}"
http_port="${HTTP_PORT:-18927}"
install_timeout="${INSTALL_TIMEOUT:-120}"
luna_bin="${LUNA_BIN:-luna-send}"
serve_pid=""

usage() {
  cat <<EOF
Usage: tools/webos/verify-device.sh [options] [path/to/app.ipk]

Installs and launches the native webOS package, verifies app registration,
captures current app logs, and captures a screenshot using supported webOS
services. It does not remove app data, patch TV files, or restart services.

Options:
  --host HOST          required SSH target unless TV_HOST is set
  --device NAME        ares device name; omitted means ares default
  --app-id ID          app id (default: $app_id)
  --ipk PATH           IPK to install; defaults to newest matching build IPK
  --out DIR            output directory (default: build/webos/verify/<timestamp>)
  --no-install         skip install and only verify/launch/capture
  --no-launch          install and verify registration without launching
  --skip-screenshot    skip screenshot capture
  --launch-wait SEC    seconds to wait after launch before capture (default: $launch_wait)
  --http-port PORT     temporary package server port (default: $http_port)
  --install-timeout SEC
                       install subscription timeout (default: $install_timeout)
  -h, --help           show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) host="$2"; shift 2 ;;
    --device) device="$2"; shift 2 ;;
    --app-id) app_id="$2"; shift 2 ;;
    --ipk) ipk="$2"; shift 2 ;;
    --out) outdir="$2"; shift 2 ;;
    --no-install) install=0; shift ;;
    --no-launch) launch=0; shift ;;
    --skip-screenshot) screenshot=0; shift ;;
    --launch-wait) launch_wait="$2"; shift 2 ;;
    --http-port) http_port="$2"; shift 2 ;;
    --install-timeout) install_timeout="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      if [[ -n "$ipk" ]]; then
        echo "unexpected extra argument: $1" >&2
        usage >&2
        exit 2
      fi
      ipk="$1"
      shift
      ;;
  esac
done
[[ -n "$host" ]] || { echo "missing required --host HOST (or TV_HOST)" >&2; usage >&2; exit 2; }

require_command() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required command: $1" >&2
    exit 1
  }
}

find_default_ipk() {
  local candidate
  candidate="$(
    find "$ROOT/build" "$ROOT/dist" -type f -name "${app_id}_*.ipk" -print 2>/dev/null \
      | sort \
      | tail -n 1
  )"
  [[ -n "$candidate" ]] && printf '%s\n' "$candidate"
}

run_luna() {
  local uri="$1"
  local payload="${2:-}"
  [[ -n "$payload" ]] || payload="{}"
  ssh -F /dev/null -tt "$host" "$luna_bin -n 1 '$uri' '$payload'"
}

stop_ipk_server() {
  if [[ -n "$serve_pid" ]]; then
    kill "$serve_pid" 2>/dev/null || true
    wait "$serve_pid" 2>/dev/null || true
    serve_pid=""
  fi
}
trap stop_ipk_server EXIT

monitored_luna() {
  local uri="$1"
  local payload="$2"
  local log_file="$3"

  {
    timeout "${install_timeout}s" ssh -F /dev/null -tt "$host" \
      "$luna_bin -i -f '$uri' '$payload'" 2>&1 &
    echo "$!"
  } | {
    read -r pid
    awk -v pid="$pid" '
      BEGIN { result = 2 }
      {
        print
        fflush()
        if ($0 ~ /"finished"[[:space:]]*:[[:space:]]*true/ ||
            $0 ~ /"statusText"[[:space:]]*:[[:space:]]*"Finished."/ ||
            $0 ~ /"state"[[:space:]]*:[[:space:]]*"installed"/) {
          result = 0
          system("kill -TERM " pid " 2>/dev/null")
          exit
        }
        if ($0 ~ /"returnValue"[[:space:]]*:[[:space:]]*false/ ||
            $0 ~ /"errorText"[[:space:]]*:/) {
          result = 1
          system("kill -TERM " pid " 2>/dev/null")
          exit
        }
      }
      END { exit result }
    '
  } | tee "$log_file"
}

install_ipk() {
  local package="$1"
  local package_dir package_name package_hash tv_ip host_ip url payload

  package_dir="$(dirname "$package")"
  package_name="$(basename "$package")"
  package_hash="$(sha256sum "$package" | cut -d' ' -f1)"
  tv_ip="${host##*@}"
  tv_ip="${tv_ip%%:*}"
  host_ip="$(ip route get "$tv_ip" 2>/dev/null |
    awk '{ for (i = 1; i <= NF; ++i) if ($i == "src") { print $(i + 1); exit } }' || true)"
  [[ -n "$host_ip" ]] || host_ip="$(hostname -I | awk '{ print $1 }')"
  url="http://${host_ip}:${http_port}/${package_name}"
  payload="$(printf '{"ipkHash":"%s","ipkUrl":"%s","subscribe":true}' "$package_hash" "$url")"

  echo "Serving $package_name at $url"
  python3 -m http.server "$http_port" --bind "$host_ip" --directory "$package_dir" \
    >"$outdir/http-server.log" 2>&1 &
  serve_pid="$!"
  sleep 1
  if ! kill -0 "$serve_pid" 2>/dev/null || ! curl -fsSI --max-time 2 "$url" >/dev/null; then
    cat "$outdir/http-server.log" >&2
    echo "failed to serve $package at $url" >&2
    exit 1
  fi

  echo "Installing through Homebrew Channel"
  monitored_luna "luna://org.webosbrew.hbchannel.service/install" "$payload" \
    "$outdir/install.log"
  stop_ipk_server
}

ares_args=()
if [[ -n "$device" ]]; then
  ares_args+=(--device "$device")
fi

if [[ "$install" == "1" ]]; then
  [[ -n "$ipk" ]] || ipk="$(find_default_ipk || true)"
  if [[ -z "$ipk" || ! -f "$ipk" ]]; then
    echo "no IPK found; pass --ipk PATH or use --no-install" >&2
    exit 1
  fi
  require_command curl
  require_command ip
  require_command python3
  require_command sha256sum
  require_command timeout
fi

if [[ "$launch" == "1" ]]; then
  require_command ares-launch
fi
require_command ssh
if [[ "$launch" == "1" && "$screenshot" == "1" ]]; then
  require_command scp
fi

if [[ -z "$outdir" ]]; then
  outdir="$ROOT/build/webos/verify/$(date -u +%Y%m%d-%H%M%S)"
fi
mkdir -p "$outdir"

echo "Output: $outdir"


if [[ "$install" == "1" ]]; then
  echo "Installing $ipk"
  install_ipk "$ipk"
fi

echo "Checking app registration for $app_id"
run_luna "luna://com.webos.applicationManager/listLaunchPoints" "{}" \
  >"$outdir/listLaunchPoints.json" 2>"$outdir/listLaunchPoints.err"
if ! grep -Fq "$app_id" "$outdir/listLaunchPoints.json"; then
  echo "app id $app_id was not present in applicationManager launch points" >&2
  exit 1
fi
run_luna "luna://com.webos.applicationManager/getAppInfo" "{\"id\":\"$app_id\"}" \
  >"$outdir/getAppInfo.json" 2>"$outdir/getAppInfo.err" || true

if [[ "$launch" != "1" ]]; then
  echo "Installed and verified $app_id; launch skipped"
  exit 0
fi

echo "Launching $app_id"
ares-launch "${ares_args[@]}" "$app_id" | tee "$outdir/ares-launch.log"
sleep "$launch_wait"

echo "Capturing process snapshot and logs"
ssh -F /dev/null -o BatchMode=yes "$host" "ps | grep '$app_id\|spool' | grep -v grep || true" \
  >"$outdir/ps.txt"
for remote in \
  "/tmp/${app_id}/${app_id}.log" \
  "/tmp/${app_id}/spool-mpv.log" \
  "/media/cryptofs/apps/usr/palm/applications/${app_id}/.cache/logs/${app_id}.log" \
  "/media/cryptofs/apps/usr/palm/applications/${app_id}/.cache/logs/spool-mpv.log" \
  "/tmp/${app_id}.log" \
  "/tmp/spool-mpv.log" \
  "/tmp/${app_id}-diagnostics/current-instance.json" \
  "/var/palm/data/${app_id}/diagnostics/current-instance.json"; do
  local_name="${remote#/}"
  local_name="${local_name//\//_}"
  if ssh -F /dev/null -o BatchMode=yes "$host" "test -f '$remote'"; then
    ssh -F /dev/null -o BatchMode=yes "$host" "cat '$remote'" >"$outdir/$local_name"
  fi
done

if [[ "$screenshot" == "1" ]]; then
  remote_shot="/tmp/${app_id}-verify-$(date -u +%Y%m%d-%H%M%S).png"
  payload="{\"path\":\"$remote_shot\",\"method\":\"DISPLAY\",\"format\":\"PNG\"}"
  echo "Capturing screenshot"
  if ! run_luna "luna://com.webos.service.capture/executeOneShot" "$payload" >"$outdir/screenshot.json" 2>"$outdir/screenshot.err"; then
    run_luna "luna://com.webos.service.tv.capture/executeOneShot" "$payload" \
      >"$outdir/screenshot.json"
  fi
  scp -F /dev/null -q "$host:$remote_shot" "$outdir/screenshot.png"
fi

echo "Verification artifacts written to $outdir"
