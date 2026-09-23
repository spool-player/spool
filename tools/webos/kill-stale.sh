#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-}"
[[ -n "$TARGET" ]] || { echo "Usage: $0 root@tv.local" >&2; exit 2; }
APP_ID="${APP_ID:-com.sachk.spool}"

ssh -tt "$TARGET" "pids=\$(ps | grep '$APP_ID\|spool' | grep -v grep | awk '{print \$1}' || true); if [ -z \"\$pids\" ]; then echo 'No matching processes'; exit 0; fi; echo \"Killing stale processes: \$pids\"; kill \$pids; sleep 2; for p in \$pids; do kill -0 \$p 2>/dev/null && kill -9 \$p || true; done"
