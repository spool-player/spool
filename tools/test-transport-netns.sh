#!/usr/bin/env bash
set -euo pipefail
# All shaping stays inside a fresh user+network namespace. Never changes host
# routes, qdiscs, sysctls or socket ceilings and never invokes sudo.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
usage() { echo "Usage: $0 --libmpv ABS [--output ABS] [--rtt-ms N] [--loss-percent N] [--counts 1,4,auto] [--protocols http1,tcp,prefer-h3]"; }
LIBRARY= OUTPUT= RTT=150 LOSS=0 COUNTS=1,4,auto PROTOCOLS=http1,tcp,prefer-h3
while (( $# )); do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --libmpv|--output|--rtt-ms|--loss-percent|--counts|--protocols)
      (( $# >= 2 )) || { usage >&2; exit 2; }
      case "$1" in --libmpv) LIBRARY="$2";; --output) OUTPUT="$2";; --rtt-ms) RTT="$2";; --loss-percent) LOSS="$2";; --counts) COUNTS="$2";; --protocols) PROTOCOLS="$2";; esac
      shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
[[ ( -z "$OUTPUT" || "$OUTPUT" == /* ) && "$LIBRARY" == /* && -f "$LIBRARY" && "$RTT" =~ ^[0-9]+([.][0-9]+)?$ && "$LOSS" =~ ^[0-9]+([.][0-9]+)?$ ]] || { usage >&2; exit 2; }
OUTPUT="${OUTPUT:-$ROOT/build/transport-packet-performance/loss-$LOSS.json}"
for command in unshare ip tc ping python3 nginx openssl; do
  command -v "$command" >/dev/null || { echo "SKIP: missing $command (use nix develop .#native)" >&2; exit 77; }
done
if ! unshare --user --map-root-user --net true 2>/dev/null; then
  echo 'SKIP: unprivileged user/network namespaces unavailable' >&2
  exit 77
fi
exec unshare --user --map-root-user --net bash -s -- "$ROOT" "$LIBRARY" "$OUTPUT" "$RTT" "$LOSS" "$COUNTS" "$PROTOCOLS" <<'INNER'
set -euo pipefail
root="$1" library="$2" output="$3" rtt="$4" loss="$5" counts="$6" protocols="$7"
ip link set lo up mtu 1500
# A loopback packet traverses this qdisc once in each direction, so half the
# desired round-trip delay applies to each packet; ping records the real RTT.
delay="$(python3 -c 'import sys; print(float(sys.argv[1])/2)' "$rtt")"
if ! tc qdisc add dev lo root netem delay "${delay}ms" loss "${loss}%" rate 200mbit limit 10000; then
  echo 'SKIP: netem qdisc unavailable in isolated namespace' >&2
  exit 77
fi
ping_report="$(LC_ALL=C ping -n -c 3 -i .2 127.0.0.1)"
printf '%s\n' "$ping_report"
measured_rtt="$(python3 -c 'import math, sys
lines = [line for line in sys.stdin.read().splitlines() if line.startswith("rtt ")]
if not lines:
    raise SystemExit("Could not measure private path RTT")
value = float(lines[-1].split("=", 1)[1].strip().split("/")[1])
if not math.isfinite(value) or value < 0:
    raise SystemExit("Invalid private path RTT")
print(value)' <<<"$ping_report")"
# Each loss level has an independent report; zero loss is the capacity and
# continuous-playback acceptance path. Run --loss-percent 0.1 / 1 separately
# to retain packet-loss evidence without conflating it with baseline capacity.
export SPOOL_TRANSPORT_NETNS=1 SPOOL_TRANSPORT_RTT_MS="$rtt" SPOOL_TRANSPORT_RATE_MBIT=200
export SPOOL_TRANSPORT_MEASURED_RTT_MS="$measured_rtt"
python3 "$root/tests/transport/high_bitrate.py" --libmpv "$library" \
  --transport-helper "$root/mpv/test/curl_transport.py" --output "$output" \
  --counts "$counts" --protocols "$protocols" --loss-percent "$loss"

INNER
