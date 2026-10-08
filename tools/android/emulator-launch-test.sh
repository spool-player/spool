#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
APK="${1:-$ROOT/dist/android/spool-e2e-app-x86_64.apk}"
RELEASE_APK="${ANDROID_RELEASE_TEST_APK:-$ROOT/dist/android/spool-x86_64.apk}"
SPOOL_ANDROID_FORM_FACTOR="${SPOOL_ANDROID_FORM_FACTOR:-phone}"
case "$SPOOL_ANDROID_FORM_FACTOR" in
  phone) emulator=android-emulator ;;
  tv) emulator=android-tv-emulator ;;
  *) echo 'error: SPOOL_ANDROID_FORM_FACTOR must be phone or tv' >&2; exit 2 ;;
esac
export SPOOL_ANDROID_FORM_FACTOR
BUILD_DIR="${ANDROID_TEST_BUILD_DIR:-$ROOT/build/android/app-x86_64}"
TEST_APK="${ANDROID_TRADITIONAL_TEST_APK:-$ROOT/dist/android/spool-tests-x86_64.apk}"
E2E_APK="${ANDROID_E2E_TEST_APK:-$ROOT/dist/android/spool-e2e-tests-x86_64.apk}"
EMULATOR_LOG="${ANDROID_EMULATOR_LOG:-$ROOT/build/android/emulator-$SPOOL_ANDROID_FORM_FACTOR.log}"
ARTIFACT_DIR="${ANDROID_LAUNCH_TEST_DIR:-$ROOT/build/android/launch-test/$SPOOL_ANDROID_FORM_FACTOR}"
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
for test_apk in "$TEST_APK" "$E2E_APK" "$RELEASE_APK"; do
  [[ -f "$test_apk" ]] || { echo "error: required APK missing: $test_apk" >&2; exit 1; }
done
if [[ -z "${SPOOL_DEVICE_HOST_E2E:-}" ]]; then
  host_build="${SPOOL_DEVICE_HOST_BUILD_DIR:-$ROOT/build/linux-release/app}"
  [[ -f "$host_build/native-spool-e2e-path.txt" ]] || {
    echo 'error: build the native host journey controller with tools/build-linux-release.sh first' >&2
    exit 1
  }
  read -r SPOOL_DEVICE_HOST_E2E <"$host_build/native-spool-e2e-path.txt"
fi
export SPOOL_DEVICE_HOST_E2E
export SPOOL_E2E_ISOLATED_DEVICE=1

# Realize the emulator and system image before the adb registration deadline.
# A cold CI runner may spend minutes downloading them.
nix build --no-link "$ROOT#$emulator"
emulator_work="$(mktemp -d -t "spool-android-$SPOOL_ANDROID_FORM_FACTOR.XXXXXXXX")"

cleanup() {
  if [[ -n "${ANDROID_SERIAL:-}" ]]; then
    "$ADB" emu kill >/dev/null 2>&1 || true
  fi
  if [[ -n "${launcher_pid:-}" ]]; then
    kill "$launcher_pid" 2>/dev/null || true
    wait "$launcher_pid" 2>/dev/null || true
  fi
  rm -rf "$emulator_work"
}
trap cleanup EXIT

mkdir -p "$(dirname "$EMULATOR_LOG")" "$ARTIFACT_DIR"
: >"$EMULATOR_LOG"
TMPDIR="$emulator_work" nix run "$ROOT#$emulator" >"$EMULATOR_LOG" 2>&1 &
launcher_pid=$!
for _ in $(seq 1 30); do
  # Nix logs the port it allocated. Never attach to somebody else's emulator.
  port="$(sed -n 's/^We have a free TCP port: \([0-9]*\)$/\1/p' "$EMULATOR_LOG")"
  ANDROID_SERIAL="${port:+emulator-$port}"
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
  if [[ "$("$ADB" get-state 2>/dev/null)" == device ]] &&
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
# Derive the journey's input mode from the actual OS feature, not an app flag.
# A TV lane must really be a TV image; handset mode must not borrow TV proof.
if "$ADB" shell pm has-feature android.software.leanback | tr -d '\r' | grep -qx true; then
  SPOOL_E2E_ANDROID_TV=1
else
  SPOOL_E2E_ANDROID_TV=0
fi
expected_tv=0
[[ "$SPOOL_ANDROID_FORM_FACTOR" == phone ]] || expected_tv=1
[[ "$SPOOL_E2E_ANDROID_TV" == "$expected_tv" ]] || {
  echo 'error: emulator OS form factor does not match its configured test lane' >&2
  exit 1
}
export SPOOL_E2E_ANDROID_TV

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
  # Raw logcat can contain provider credentials; archive it, never print it.
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
"$ADB" install -r "$TEST_APK"
"$ADB" install -r "$E2E_APK"
status=0
# The shared driver finishes every traditional native selector before starting
# real GUI selectors and retains both phase failures.
# The native shell supplies real host Qt, FFmpeg CLI and English OCR; the
# controller renders nothing on the host and observes the private emulator.
nix develop "$ROOT#native" -c python "$ROOT/tools/run-tests.py" \
  --build-dir "$BUILD_DIR" --workers 1 || status=1
# Replace the developer application with the exact signed release artifact.
# Uninstall only from this owned emulator to avoid signature/version conflicts
# and developer profile state making a cold shipped launch appear successful.
if "$ADB" uninstall "$SPOOL_PACKAGE" && "$ADB" install "$RELEASE_APK"; then
  (launch_category "$SPOOL_PACKAGE" android.intent.category.LAUNCHER) || status=1
  (launch_category "$SPOOL_PACKAGE" android.intent.category.LEANBACK_LAUNCHER) || status=1
else
  echo 'error: installing the actual signed release APK for launcher proof failed' >&2
  status=1
fi
exit "$status"
