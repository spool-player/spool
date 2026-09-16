#!/usr/bin/env bash
# Detach a mounted disk image, tolerating the brief window in which macOS still
# holds the volume.
#
# Usage: tools/detach-dmg.sh <mountpoint>
#
# Anything that walked the volume -- Spotlight indexing, the quarantine
# scanner, or a process that has only just exited -- can keep the mount busy
# for a few seconds after the work is done, and hdiutil reports that as
# "couldn't unmount ... Resource busy" (exit 16). Retry until the volume
# settles rather than failing the build over a transient holder.
set -euo pipefail

mountpoint="${1:-}"
if [[ -z "$mountpoint" ]]; then
  echo "usage: detach-dmg.sh <mountpoint>" >&2
  exit 2
fi

attempts="${DMG_DETACH_ATTEMPTS:-12}"
delay="${DMG_DETACH_DELAY:-5}"
# Escalating to -force straight away can tear the volume down while a reader
# still has it open, so ask politely for the first few rounds.
force_after="${DMG_DETACH_FORCE_AFTER:-4}"

mounted() {
  mount | grep -qF " on $mountpoint ("
}

# Callers run this from a trap as well as directly, so a mountpoint that is
# already gone is success, not an error.
if ! mounted; then
  exit 0
fi

for (( attempt = 1; attempt <= attempts; attempt++ )); do
  detach_args=(detach "$mountpoint")
  if (( attempt > force_after )); then
    detach_args+=(-force)
  fi
  if hdiutil "${detach_args[@]}"; then
    exit 0
  fi
  if ! mounted; then
    exit 0
  fi
  if (( attempt == attempts )); then
    break
  fi
  printf 'warning: %s is still busy; retrying detach in %ss (attempt %d/%d)\n' \
    "$mountpoint" "$delay" "$attempt" "$attempts" >&2
  sleep "$delay"
done

printf 'error: could not detach %s after %d attempts; open files:\n' \
  "$mountpoint" "$attempts" >&2
# -S bounds the stat calls so a wedged volume cannot hang the report.
lsof -w -n -S 2 +D "$mountpoint" >&2 || true
exit 16
