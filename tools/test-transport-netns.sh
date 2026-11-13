#!/usr/bin/env bash
set -euo pipefail
# All shaping stays inside a fresh user+network namespace. Never changes host
# routes, qdiscs, sysctls or socket ceilings and never invokes sudo.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
usage() { echo "Usage: $0 --libmpv ABS [--output ABS] [--rtt-ms N] [--loss-percent N]"; }
LIBRARY= OUTPUT= RTT=150 LOSS=0
while (( $# )); do
  case "$1" in
    --help|-h) usage; exit 0 ;;
    --libmpv|--output|--rtt-ms|--loss-percent)
      (( $# >= 2 )) || { usage >&2; exit 2; }
      case "$1" in --libmpv) LIBRARY="$2";; --output) OUTPUT="$2";; --rtt-ms) RTT="$2";; --loss-percent) LOSS="$2";; esac
      shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
[[ "$LIBRARY" == /* && -f "$LIBRARY" && "$RTT" =~ ^[0-9]+([.][0-9]+)?$ && "$LOSS" =~ ^[0-9]+([.][0-9]+)?$ ]] || { usage >&2; exit 2; }
for command in unshare ip tc ping python3 nginx openssl; do
  command -v "$command" >/dev/null || { echo "SKIP: missing $command (use nix develop .#native)" >&2; exit 77; }
done
if ! unshare --user --map-root-user --net true 2>/dev/null; then
  echo 'SKIP: unprivileged user/network namespaces unavailable' >&2
  exit 77
fi
exec unshare --user --map-root-user --net bash -s -- "$ROOT" "$LIBRARY" "$OUTPUT" "$RTT" "$LOSS" <<'INNER'
set -euo pipefail
root="$1" library="$2" output="$3" rtt="$4" loss="$5"
ip link set lo up mtu 1500
# A loopback packet traverses this qdisc once in each direction, so half the
# desired round-trip delay applies to each packet; ping records the real RTT.
delay="$(python3 -c 'import sys; print(float(sys.argv[1])/2)' "$rtt")"
if ! tc qdisc add dev lo root netem delay "${delay}ms" loss "${loss}%" rate 200mbit limit 10000; then
  echo 'SKIP: netem qdisc unavailable in isolated namespace' >&2
  exit 77
fi
ping -n -c 3 -i .2 127.0.0.1
args=(--libmpv "$library" --transport-helper "$root/mpv/test/curl_transport.py" --performance)
[[ -z "$output" ]] || args+=(--output "$output")
python3 "$root/tests/transport/protocols.py" "${args[@]}"
INNER
