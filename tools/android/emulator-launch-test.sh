#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APK="${1:-$ROOT/dist/android/spool-x86_64.apk}"
EMULATOR_LOG="${ANDROID_EMULATOR_LOG:-$ROOT/build/android/emulator.log}"
ARTIFACT_DIR="${ANDROID_LAUNCH_TEST_DIR:-$ROOT/build/android/launch-test}"
# The app launches through its own QtActivity subclass, which owns the launch
# screen's exit. Naming it here is what catches a manifest that fell back to
# Qt's stock activity and dropped that handover.
SPOOL_ACTIVITY=com.sachk.spool.SpoolActivity
SPOOL_PACKAGE=com.sachk.spool

: "${ANDROID_HOME:?run through nix develop .#android}"
ADB="$ANDROID_HOME/platform-tools/adb"
[[ -x "$ADB" ]] || {
  echo "error: adb missing at $ADB" >&2
  exit 1
}
[[ -f "$APK" ]] || {
  echo "error: APK missing at $APK" >&2
  exit 1
}

# Realize the emulator and system image before the adb registration deadline.
# A cold CI runner may spend minutes downloading them.
nix build --no-link "$ROOT#android-emulator"

cleanup() {
  "$ADB" emu kill >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "$(dirname "$EMULATOR_LOG")" "$ARTIFACT_DIR"
: >"$EMULATOR_LOG"
nix run "$ROOT#android-emulator" >"$EMULATOR_LOG" 2>&1 &
for _ in $(seq 1 30); do
  ANDROID_SERIAL="$("$ADB" devices | sed -n 's/^\(emulator-[0-9]*\)[[:space:]].*/\1/p' | sed -n '1p')"
  [[ -n "$ANDROID_SERIAL" ]] && break
  sleep 1
done
[[ -n "${ANDROID_SERIAL:-}" ]] || {
  cat "$EMULATOR_LOG" >&2
  echo "error: Android emulator did not register with adb" >&2
  exit 1
}
export ANDROID_SERIAL

for _ in $(seq 1 90); do
  if "$ADB" wait-for-device >/dev/null 2>&1 &&
    [[ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]]; then
    break
  fi
  sleep 2
done
[[ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] || {
  cat "$EMULATOR_LOG" >&2
  echo "error: Android emulator did not boot" >&2
  exit 1
}

# Resolves through each launcher category in turn. One package now has to
# answer to both, so a manifest that lost either entry point fails here rather
# than passing on the activity class name alone.
launcher_component() {
  local package="$1" category="$2"
  "$ADB" shell cmd package resolve-activity --brief \
    -a android.intent.action.MAIN -c "$category" "$package" |
    tr -d '\r' | sed -n "s#^\($package/[A-Za-z0-9_.\$]*\)\$#\1#p" | sed -n '1p'
}

fail() {
  local package="$1"
  shift
  "$ADB" logcat -d >"$ARTIFACT_DIR/$package-logcat.txt" 2>/dev/null || true
  "$ADB" exec-out screencap -p >"$ARTIFACT_DIR/$package-screen.png" 2>/dev/null || true
  sed -n '1,200p' "$ARTIFACT_DIR/$package-logcat.txt" >&2 || true
  echo "error: $*" >&2
  echo "note: full logcat and screenshot under $ARTIFACT_DIR" >&2
  exit 1
}

launch_category() {
  local package="$1" category="$2" component activity
  component="$(launcher_component "$package" "$category")"
  [[ -n "$component" ]] || fail "$package" "$package advertises no $category launcher activity"
  # resolve-activity abbreviates a class that sits under the package's own
  # namespace, so this reports .SpoolActivity rather than the full class name.
  activity="${component#*/}"
  if [[ "$activity" == .* ]]; then
    activity="$package$activity"
  fi
  [[ "$activity" == "$SPOOL_ACTIVITY" ]] ||
    fail "$package" "$package launcher is $activity, expected $SPOOL_ACTIVITY"

  "$ADB" shell am force-stop "$package"
  "$ADB" logcat -c
  "$ADB" shell am start -W -n "$component"
  for _ in $(seq 1 30); do
    "$ADB" logcat -d -s Spool:V | grep -qF 'startup: QML source loaded' && break
    sleep 1
  done

  [[ -n "$("$ADB" shell pidof "$package" | tr -d '\r')" ]] ||
    fail "$package" "$package exited during launch"
  "$ADB" shell dumpsys activity activities | grep -F "$component" >/dev/null ||
    fail "$package" "$package activity is not present"
  "$ADB" logcat -d -s Spool:V | grep -qF 'startup: QML source loaded' ||
    fail "$package" "$package did not load its QML scene"
  ! "$ADB" logcat -d -s Spool:V | grep -qE '\[qml\] |\[qt:(crit|fatal)\] ' ||
    fail "$package" "$package reported QML errors"
  ! "$ADB" logcat -d -b crash -b main | grep -qE 'FATAL EXCEPTION|Fatal signal' ||
    fail "$package" "Android reported a fatal launch failure for $package"

  local shot="$ARTIFACT_DIR/$package-${category##*.}-screen.png"
  "$ADB" exec-out screencap -p >"$shot"
  "$ADB" shell am force-stop "$package"
  printf '%s launched through %s; screenshot at %s\n' "$package" "$category" "$shot"
}

"$ADB" install -r "$APK"
# One package, both entry points. A handset resolves the first and a television
# the second, and the universal APK is only universal if it answers to both.
launch_category "$SPOOL_PACKAGE" android.intent.category.LAUNCHER
launch_category "$SPOOL_PACKAGE" android.intent.category.LEANBACK_LAUNCHER
printf 'universal APK launch test passed for both launcher categories\n'
