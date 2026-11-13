#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export SPOOL_APPLE_TEST_PLATFORM=ios
exec bash "$ROOT/tools/apple/smoke-tvos.sh" "$@"
