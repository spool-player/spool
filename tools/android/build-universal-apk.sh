#!/usr/bin/env bash
# Build the one APK that installs on any device.
#
#   tools/android/build-universal-apk.sh OUT.apk INPUTS_DIR IN.apk IN.apk [...]
#
# The release page offers this so somebody who does not know their device's
# architecture still downloads something that installs. The in-app updater
# never fetches it: the update manifest is keyed by ABI and this file is not in
# it, so an installed build only ever updates to the APK built for its own ABI.
#
# INPUTS_DIR holds the Gradle project each per-ABI leg packaged from, one
# directory per ABI, staged by tools/android/build-apks.sh. The APKs supply the
# native libraries, already stripped as they shipped.
#
# This packages rather than merges. Qt's loader reads res/values/libs.xml to
# decide which architecture it is running and which libraries to load in which
# order, and that file names every library with its ABI in front of it. A
# universal package needs the union of those arrays, and libs.xml is compiled
# into resources.arsc, so the union has to go in before aapt sees it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=tools/android/signing.sh
source "$ROOT/tools/android/signing.sh"

[[ $# -ge 4 ]] || {
  echo "usage: $0 OUT.apk INPUTS_DIR IN.apk IN.apk [...]" >&2
  exit 2
}
OUT="$1"
INPUTS="$2"
shift 2
for apk in "$@"; do
  [[ -f "$apk" ]] || {
    echo "error: input APK missing at $apk" >&2
    exit 1
  }
done
[[ -d "$INPUTS" ]] || {
  echo "error: staged Android projects missing at $INPUTS" >&2
  exit 1
}

: "${ANDROID_HOME:?run through nix develop .#android}"
BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-$ANDROID_HOME/build-tools/36.0.0}"
for tool in zipalign apksigner; do
  [[ -x "$BUILD_TOOLS/$tool" ]] || {
    echo "error: $tool missing at $BUILD_TOOLS/$tool" >&2
    exit 1
  }
done

project="$ROOT/build/android/universal"
mkdir -p "$(dirname "$OUT")"

python3 "$ROOT/tools/android/universal_project.py" \
  --output "$project" --inputs "$INPUTS" "$@"

printf 'sdk.dir=%s\n' "$ANDROID_HOME" >"$project/local.properties"
# Compress the multi-ABI download; Android extracts only the device's ABI.
(cd "$project" && ./gradlew --no-daemon -PlegacyPackaging=true assembleRelease)

# AGP names the output after the project directory, so find it rather than
# spell it: exactly one release APK is expected, and two would mean the project
# grew a variant nobody chose.
mapfile -t built < <(find "$project/build/outputs/apk/release" -maxdepth 1 -name '*.apk' | sort)
[[ ${#built[@]} -eq 1 ]] || {
  echo "error: expected one release APK from Gradle, found ${#built[@]}" >&2
  exit 1
}
unsigned="${built[0]}"

prepare_keystore
# Plain 4-byte alignment, not -p: the native libraries are deflated, so there
# is nothing to map on a page boundary. resources.arsc remains stored/aligned.
"$BUILD_TOOLS/zipalign" -f 4 "$unsigned" "$project/aligned.apk"
"$BUILD_TOOLS/apksigner" sign \
  --ks "$QT_ANDROID_KEYSTORE_PATH" \
  --ks-key-alias "$QT_ANDROID_KEYSTORE_ALIAS" \
  --ks-pass "pass:$QT_ANDROID_KEYSTORE_STORE_PASS" \
  --key-pass "pass:$QT_ANDROID_KEYSTORE_KEY_PASS" \
  --out "$OUT" \
  "$project/aligned.apk"
"$BUILD_TOOLS/zipalign" -c 4 "$OUT"
"$BUILD_TOOLS/apksigner" verify --verbose --print-certs "$OUT"

printf 'universal APK: %s (%s)\n' "$OUT" "$(du -h "$OUT" | cut -f1)"
