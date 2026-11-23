#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"

host="${TV_HOST:-}"
app_id="com.sachk.spool"
interval="1"
duration="300"
wait_timeout="600"
out=""
plot=1
wait_for_process=0
stop_on_exit=0
wait_for_absent=0

usage() {
  cat <<'EOF'
Usage: tools/webos/capture-memory.sh [options]

Streams memory samples from the TV over SSH and writes a local CSV plus SVG.
No listener or endpoint is opened on the desktop, and the TV is only read.

Options:
  --host HOST        required SSH host unless TV_HOST is set
  --app-id ID       app id/process match (default: com.sachk.spool)
  --interval SEC    sample interval (default: 1)
  --duration SEC    bounded capture window, 1..86400 (default: 300)
  --wait-timeout SEC maximum process-lifetime wait (default: 600)
  --out PATH        output CSV path (default: build/memory/<timestamp>.csv)
  --wait-for-process
                    wait until the app process exists before writing samples
  --stop-on-exit    stop after the first observed app process exits
  --process-lifetime
                    shorthand for --wait-for-process --stop-on-exit
  --new-process-lifetime
                    wait for any current app process to exit, then capture the
                    next process until it exits
  --no-plot         skip SVG graph generation
  -h, --help        show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host|--app-id|--interval|--duration|--out|--wait-timeout) webos_require_value "$@" ;;
  esac
  case "$1" in
    --host) host="$2"; shift 2 ;;
    --app-id) app_id="$2"; shift 2 ;;
    --interval) interval="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --wait-timeout) wait_timeout="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --wait-for-process) wait_for_process=1; shift ;;
    --stop-on-exit) stop_on_exit=1; shift ;;
    --process-lifetime) wait_for_process=1; stop_on_exit=1; shift ;;
    --new-process-lifetime) wait_for_absent=1; wait_for_process=1; stop_on_exit=1; shift ;;
    --no-plot) plot=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done
[[ -n "$host" ]] || { echo "missing required --host HOST (or TV_HOST)" >&2; usage >&2; exit 2; }
webos_app_id "$app_id"
webos_uint --interval "$interval" 1 3600
webos_uint --duration "$duration"
webos_uint --wait-timeout "$wait_timeout"
interval=$((10#$interval))
duration=$((10#$duration))
wait_timeout=$((10#$wait_timeout))
webos_transport_init "$host"
(( ! plot )) || webos_require_command python3

if [[ -z "$out" ]]; then
  mkdir -p "$ROOT/build/memory"
  out="$ROOT/build/memory/tv-memory-$(date +%Y%m%d-%H%M%S).csv"
else
  mkdir -p "$(dirname "$out")"
fi

tmp="${out}.tmp"
trap 'rm -f "$tmp"' EXIT

echo "Writing $out"

timeout --signal=TERM --kill-after=5s "$((wait_timeout + duration + 15))s" "${WEBOS_SSH[@]}" -- "$host" \
  "APP_ID='$app_id' INTERVAL='$interval' DURATION='$duration' WAIT_TIMEOUT='$wait_timeout' WAIT_FOR_ABSENT='$wait_for_absent' WAIT_FOR_PROCESS='$wait_for_process' STOP_ON_EXIT='$stop_on_exit' sh -s" >"$tmp" <<'REMOTE'
app_id=${APP_ID:-com.sachk.spool}
interval=${INTERVAL:-1}
duration=${DURATION:-0}
wait_for_absent=${WAIT_FOR_ABSENT:-0}
wait_for_process=${WAIT_FOR_PROCESS:-0}
stop_on_exit=${STOP_ON_EXIT:-0}
start=$(date +%s)
wait_deadline=$((start + WAIT_TIMEOUT))
observed_pid=

printf '%s\n' 'epoch,pid,vmrss_kb,vmsize_kb,vmdata_kb,vmswap_kb,pss_kb,threads,mem_total_kb,mem_free_kb,mem_available_kb,buffers_kb,cached_kb,swap_total_kb,swap_free_kb'

find_pid() {
  (ps -ef 2>/dev/null || ps 2>/dev/null) | awk -v app="$app_id" '
    BEGIN {
      path = "/applications/" app "/"
      app_json = "\"appId\":\"" app "\""
    }
    index($0, path) || index($0, app_json) {
      if ($2 ~ /^[0-9]+$/) {
        pid=$2
      } else if ($1 ~ /^[0-9]+$/) {
        pid=$1
      }
    }
    END { if (pid) print pid }
  '
}

status_value() {
  key=$1
  file=$2
  awk -v k="$key" '$1 == k ":" { print $2; found=1; exit } END { if (!found) print "" }' "$file" 2>/dev/null
}

meminfo_value() {
  key=$1
  awk -v k="$key" '$1 == k ":" { print $2; found=1; exit } END { if (!found) print "" }' /proc/meminfo 2>/dev/null
}

if [ "$wait_for_absent" = "1" ]; then
  while [ -n "$(find_pid)" ]; do
    [ "$(date +%s)" -lt "$wait_deadline" ] || { echo 'timed out waiting for current process to exit' >&2; exit 1; }
    remaining=$((wait_deadline - $(date +%s)))
    pause=$interval
    [ "$pause" -le "$remaining" ] || pause=$remaining
    [ "$pause" -le 0 ] || sleep "$pause"
  done
fi

while :; do
  now=$(date +%s)
  if { [ "$wait_for_process" != "1" ] || [ -n "$observed_pid" ]; } && [ $((now - start)) -ge "$duration" ]; then
    break
  fi

  pid=$(find_pid)
  if [ -z "$observed_pid" ]; then
    if [ -z "$pid" ] && [ "$wait_for_process" = "1" ]; then
      [ "$now" -lt "$wait_deadline" ] || { echo 'timed out waiting for app process' >&2; exit 1; }
      remaining=$((wait_deadline - now))
      pause=$interval
      [ "$pause" -le "$remaining" ] || pause=$remaining
      [ "$pause" -le 0 ] || sleep "$pause"
      continue
    fi
    if [ -n "$pid" ]; then
      observed_pid=$pid
      [ "$wait_for_process" != "1" ] || start=$now
    fi
  elif [ "$stop_on_exit" = "1" ]; then
    if [ ! -r "/proc/$observed_pid/status" ]; then
      break
    fi
    pid=$observed_pid
  fi

  vmrss= vmsize= vmdata= vmswap= pss= threads=
  if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
    vmrss=$(status_value VmRSS "/proc/$pid/status")
    vmsize=$(status_value VmSize "/proc/$pid/status")
    vmdata=$(status_value VmData "/proc/$pid/status")
    vmswap=$(status_value VmSwap "/proc/$pid/status")
    threads=$(status_value Threads "/proc/$pid/status")
    if [ -r "/proc/$pid/smaps_rollup" ]; then
      pss=$(status_value Pss "/proc/$pid/smaps_rollup")
    fi
  fi

  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$now" "$pid" "$vmrss" "$vmsize" "$vmdata" "$vmswap" "$pss" "$threads" \
    "$(meminfo_value MemTotal)" "$(meminfo_value MemFree)" \
    "$(meminfo_value MemAvailable)" "$(meminfo_value Buffers)" \
    "$(meminfo_value Cached)" "$(meminfo_value SwapTotal)" \
    "$(meminfo_value SwapFree)"

  remaining=$((duration - ($(date +%s) - start)))
  pause=$interval
  [ "$pause" -le "$remaining" ] || pause=$remaining
  [ "$pause" -le 0 ] || sleep "$pause"
done
REMOTE

mv "$tmp" "$out"
trap - EXIT

if [[ "$plot" -eq 1 ]]; then
  svg="${out%.csv}.svg"
  python3 - "$out" "$svg" <<'PY'
import csv
import math
import sys

csv_path, svg_path = sys.argv[1], sys.argv[2]
rows = []
with open(csv_path, newline="") as f:
    for row in csv.DictReader(f):
        try:
            epoch = int(row["epoch"])
        except (KeyError, ValueError):
            continue
        def kb(name):
            try:
                return float(row.get(name) or "nan")
            except ValueError:
                return math.nan
        rows.append({
            "t": epoch,
            "rss": kb("vmrss_kb") / 1024.0,
            "pss": kb("pss_kb") / 1024.0,
            "avail": kb("mem_available_kb") / 1024.0,
        })

if not rows:
    raise SystemExit("no rows to plot")

t0 = rows[0]["t"]
for row in rows:
    row["x"] = row["t"] - t0

series = [
    ("rss", "App RSS MB", "#d1495b"),
    ("pss", "App PSS MB", "#2e86ab"),
    ("avail", "MemAvailable MB", "#2a9d8f"),
]
width, height = 1100, 620
left, right, top, bottom = 82, 28, 34, 72
plot_w, plot_h = width - left - right, height - top - bottom
max_x = max(row["x"] for row in rows) or 1
values = [row[key] for key, _, _ in series for row in rows if not math.isnan(row[key])]
max_y = max(values) if values else 1
max_y = max(64, math.ceil(max_y / 64) * 64)

def sx(x):
    return left + (x / max_x) * plot_w

def sy(y):
    return top + plot_h - (y / max_y) * plot_h

def path_for(key):
    pts = [(sx(row["x"]), sy(row[key])) for row in rows if not math.isnan(row[key])]
    if not pts:
        return ""
    return "M " + " L ".join(f"{x:.1f} {y:.1f}" for x, y in pts)

parts = [
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
    '<rect width="100%" height="100%" fill="#101417"/>',
    f'<text x="{left}" y="24" fill="#e8edf0" font-family="sans-serif" font-size="18">TV memory capture</text>',
    f'<rect x="{left}" y="{top}" width="{plot_w}" height="{plot_h}" fill="#151b1f" stroke="#34424a"/>',
]

for i in range(6):
    yv = max_y * i / 5
    y = sy(yv)
    parts.append(f'<line x1="{left}" x2="{left + plot_w}" y1="{y:.1f}" y2="{y:.1f}" stroke="#26343b"/>')
    parts.append(f'<text x="{left - 10}" y="{y + 4:.1f}" text-anchor="end" fill="#9fb0b8" font-family="sans-serif" font-size="12">{yv:.0f}</text>')

for i in range(6):
    xv = max_x * i / 5
    x = sx(xv)
    parts.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{top + plot_h}" stroke="#1f2b31"/>')
    parts.append(f'<text x="{x:.1f}" y="{height - 34}" text-anchor="middle" fill="#9fb0b8" font-family="sans-serif" font-size="12">{xv:.0f}s</text>')

for key, label, color in series:
    path = path_for(key)
    if path:
        parts.append(f'<path d="{path}" fill="none" stroke="{color}" stroke-width="2.2"/>')

legend_x = left
for key, label, color in series:
    parts.append(f'<rect x="{legend_x}" y="{height - 24}" width="16" height="4" fill="{color}"/>')
    parts.append(f'<text x="{legend_x + 22}" y="{height - 18}" fill="#d7e0e4" font-family="sans-serif" font-size="13">{label}</text>')
    legend_x += 170

parts.append(f'<text x="{left + plot_w / 2:.1f}" y="{height - 8}" text-anchor="middle" fill="#9fb0b8" font-family="sans-serif" font-size="12">seconds</text>')
parts.append(f'<text x="22" y="{top + plot_h / 2:.1f}" transform="rotate(-90 22 {top + plot_h / 2:.1f})" text-anchor="middle" fill="#9fb0b8" font-family="sans-serif" font-size="12">MB</text>')
parts.append('</svg>')

with open(svg_path, "w") as f:
    f.write("\n".join(parts))
print(f"Wrote {svg_path}")
PY
fi

echo "Done"
