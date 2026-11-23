#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/tools/webos/device-common.sh"
if [[ $# == 1 && ( "$1" == -h || "$1" == --help ) ]]; then
  echo "Usage: $0 root@tv.local"
  echo 'Discovers the running app data root; prints safe metadata or reports metadata unavailable.'
  exit 0
fi
[[ $# == 1 ]] || webos_error "Usage: $0 root@tv.local"
TARGET="$1"
APP_ID="${APP_ID:-com.sachk.spool}"
webos_app_id "$APP_ID"
webos_transport_init "$TARGET"
webos_require_command python3
# No credential/config readers or guessed legacy diagnostic directories.
webos_discover "$TARGET" diagnostics | python3 -c '
import json, sys
text = sys.stdin.read()
if not text.strip():
    raise SystemExit("no current instance metadata found")
value = json.loads(text)
allowed = {key: item for key, item in value.items()
           if key in ("pid", "uptimeMs", "uptimeSeconds") and isinstance(item, (int, float))}
for key in ("running", "metadataAvailable"):
    if isinstance(value.get(key), bool):
        allowed[key] = value[key]
allowed.setdefault("metadataAvailable", True)
if value.get("state") in ("starting", "running", "stopping", "stopped", "hung"):
    allowed["state"] = value["state"]
print(json.dumps(allowed, indent=2))'
