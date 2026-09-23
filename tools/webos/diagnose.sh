#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-}"
[[ -n "$TARGET" ]] || { echo "Usage: $0 root@tv.local" >&2; exit 2; }
APP_ID="${APP_ID:-com.sachk.spool}"

# Logs live in /tmp/$APP_ID/ since the log-dir refactor; older builds wrote
# flat /tmp/*.log files and the persistent fallback is <approot>/.cache/logs.
ssh -tt "$TARGET" "set -e; echo '== app processes =='; ps | grep '$APP_ID\|spool' | grep -v grep || true; echo '== temp logs =='; ls -l /tmp/${APP_ID}/ /media/cryptofs/apps/usr/palm/applications/${APP_ID}/.cache/logs/ /tmp/${APP_ID}.log /tmp/spool-mpv.log 2>/dev/null || true; echo '== current instance =='; for p in /media/cryptofs/apps/usr/palm/applications/$APP_ID/diagnostics/current-instance.json /var/palm/data/$APP_ID/diagnostics/current-instance.json /tmp/$APP_ID-diagnostics/current-instance.json; do [ -f \"\$p\" ] && { echo \"--- \$p\"; cat \"\$p\"; }; done"
