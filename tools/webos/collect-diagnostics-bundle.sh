#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-}"
[[ -n "$TARGET" ]] || { echo "Usage: $0 root@tv.local" >&2; exit 2; }
APP_ID="${APP_ID:-com.sachk.spool}"
OUT="${OUT:-diagnostics-bundle-$(date -u +%Y%m%d-%H%M%S).tar.gz}"

# Logs live in /tmp/$APP_ID/ since the log-dir refactor; also grab the legacy
# flat /tmp/*.log paths and the persistent fallback dir for older builds.
ssh -tt "$TARGET" "rm -rf /tmp/${APP_ID}-diagnostics-bundle; mkdir -p /tmp/${APP_ID}-diagnostics-bundle; cp -a /tmp/${APP_ID}-diagnostics /tmp/${APP_ID}-diagnostics-bundle/tmp-diagnostics 2>/dev/null || true; cp -a /var/palm/data/${APP_ID}/diagnostics /tmp/${APP_ID}-diagnostics-bundle/appdata-diagnostics 2>/dev/null || true; cp -a /tmp/${APP_ID}/. /tmp/${APP_ID}-diagnostics-bundle/tmp-logs 2>/dev/null || true; cp -a /media/cryptofs/apps/usr/palm/applications/${APP_ID}/.cache/logs/. /tmp/${APP_ID}-diagnostics-bundle/appdir-logs 2>/dev/null || true; cp -a /tmp/${APP_ID}.log /tmp/spool-mpv.log /tmp/${APP_ID}-diagnostics-bundle/ 2>/dev/null || true; ps > /tmp/${APP_ID}-diagnostics-bundle/ps.txt; tar -czf /tmp/${APP_ID}-diagnostics-bundle.tar.gz -C /tmp ${APP_ID}-diagnostics-bundle"
scp "$TARGET:/tmp/${APP_ID}-diagnostics-bundle.tar.gz" "$OUT"
printf 'Wrote %s\n' "$OUT"
