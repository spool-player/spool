#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"
if [[ $# == 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  echo "Usage: $0 root@tv.local"
  echo 'Explicitly terminates only processes executing this app bundle binary.'
  exit 0
fi
[[ $# == 1 ]] || webos_error "Usage: $0 root@tv.local"
TARGET="$1"
APP_ID="${APP_ID:-com.sachk.spool}"
webos_app_id "$APP_ID"
webos_transport_init "$TARGET"
webos_ssh "$TARGET" "APP_ID='$APP_ID' sh -s" <<'REMOTE'
set -e
matches() {
  exe=$(readlink "/proc/$1/exe" 2>/dev/null) || return 1
  case "$exe" in
    */usr/palm/applications/"$APP_ID"/bin/spool|*/usr/palm/applications/"$APP_ID"/bin/spool.real) return 0 ;;
    *) return 1 ;;
  esac
}
pids=
for proc in /proc/[0-9]*; do
  pid=${proc##*/}
  if matches "$pid"; then pids="$pids $pid"; fi
done
[ -n "$pids" ] || { echo 'No matching app processes'; exit 0; }
echo "Terminating app processes:$pids"
for pid in $pids; do kill "$pid" 2>/dev/null || true; done
sleep 2
# Recheck the executable before escalation in case a PID has been reused.
for pid in $pids; do
  if matches "$pid"; then kill -KILL "$pid" 2>/dev/null || true; fi
done
REMOTE
