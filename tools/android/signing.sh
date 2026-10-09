# shellcheck shell=bash
# The Android signing identity, shared by every script that produces an APK.
#
# androiddeployqt emits an unsigned release APK, so something has to sign it.
#
# The upload key is the one that matters: a build signed with anything else
# cannot be installed over the app already on a device, so a locally built APK
# that is not signed with it is not a build of the same app. Release jobs pass
# the key through the environment. Locally, a credentials file is read if there
# is one, so the real key is simply what a local build uses -- there is nothing
# to remember to set. Without it the build falls back to a debug key, which is
# installable on a clean device and nowhere else.
#
# The file is JSON, beside the keystore, and belongs outside the repository:
#
#   {"keystore": "/abs/path/spool-upload.p12", "alias": "...",
#    "storePassword": "...", "keyPassword": "..."}
SPOOL_SIGNING_ROOT="${SPOOL_SIGNING_ROOT:-$ROOT}"
SPOOL_ANDROID_SIGNING_CREDENTIALS="${SPOOL_ANDROID_SIGNING_CREDENTIALS:-${XDG_DATA_HOME:-$HOME/.local/share}/spool/signing/android-upload-credentials.json}"

prepare_keystore() {
  # androiddeployqt's Gradle launcher honors JAVA_HOME, but the SDK's
  # apksigner executes `java` from PATH. Use the same selected JDK for both,
  # including incremental packaging launched outside the full Android shell.
  if [[ -n "${JAVA_HOME:-}" ]]; then
    export PATH="$JAVA_HOME/bin:$PATH"
  fi
  if [[ -z "${SPOOL_ANDROID_KEYSTORE_PATH:-}" && -f "$SPOOL_ANDROID_SIGNING_CREDENTIALS" ]]; then
    printf 'signing with the key named by %s\n' "$SPOOL_ANDROID_SIGNING_CREDENTIALS"
    local field
    while IFS=$'\t' read -r field value; do
      case "$field" in
        keystore) SPOOL_ANDROID_KEYSTORE_PATH="$value" ;;
        alias) SPOOL_ANDROID_KEYSTORE_ALIAS="$value" ;;
        storePassword) SPOOL_ANDROID_KEYSTORE_STORE_PASS="$value" ;;
        keyPassword) SPOOL_ANDROID_KEYSTORE_KEY_PASS="$value" ;;
      esac
    done < <(python3 -c '
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    for field, value in json.load(handle).items():
        print(field, value, sep="\t")
' "$SPOOL_ANDROID_SIGNING_CREDENTIALS")
  fi
  if [[ -n "${SPOOL_ANDROID_KEYSTORE_PATH:-}" ]]; then
    [[ -f "$SPOOL_ANDROID_KEYSTORE_PATH" ]] || {
      echo "error: release keystore is missing at $SPOOL_ANDROID_KEYSTORE_PATH" >&2
      exit 1
    }
    : "${SPOOL_ANDROID_KEYSTORE_ALIAS:?release keystore alias is required}"
    : "${SPOOL_ANDROID_KEYSTORE_STORE_PASS:?release keystore password is required}"
    : "${SPOOL_ANDROID_KEYSTORE_KEY_PASS:?release key password is required}"
    export QT_ANDROID_KEYSTORE_PATH="$SPOOL_ANDROID_KEYSTORE_PATH"
    export QT_ANDROID_KEYSTORE_ALIAS="$SPOOL_ANDROID_KEYSTORE_ALIAS"
    export QT_ANDROID_KEYSTORE_STORE_PASS="$SPOOL_ANDROID_KEYSTORE_STORE_PASS"
    export QT_ANDROID_KEYSTORE_KEY_PASS="$SPOOL_ANDROID_KEYSTORE_KEY_PASS"
    return
  fi

  printf 'no signing key configured (%s); signing with a debug key, which will not install over an existing build\n' \
    "$SPOOL_ANDROID_SIGNING_CREDENTIALS" >&2
  local keystore="$SPOOL_SIGNING_ROOT/build/android/debug.keystore"
  if [[ ! -f "$keystore" ]]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -alias spooldebug \
      -storepass spooldebug -keypass spooldebug \
      -keyalg RSA -keysize 2048 -validity 10000 \
      -dname "CN=Spool Debug, OU=Spool, O=Spool, L=None, ST=None, C=AU"
  fi
  export QT_ANDROID_KEYSTORE_PATH="$keystore"
  export QT_ANDROID_KEYSTORE_ALIAS=spooldebug
  export QT_ANDROID_KEYSTORE_STORE_PASS=spooldebug
  export QT_ANDROID_KEYSTORE_KEY_PASS=spooldebug
}

