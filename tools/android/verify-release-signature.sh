#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$root/tools/lib/manifest-sources.sh"
apk="${1:?usage: verify-release-signature.sh APK}"
: "${ANDROID_HOME:?set ANDROID_HOME to the pinned SDK}"
build_tools="${ANDROID_BUILD_TOOLS:-$ANDROID_HOME/build-tools/$(toolchain_field "$root" android.buildTools)}"
expected_cert='0c9deff0513eed6d43521df31ad6d2d8e928c029b146d902f4bcc25a883c1fb8'

signature="$("$build_tools/apksigner" verify --verbose --print-certs "$apk")"
printf '%s\n' "$signature"
grep -Fxq "Signer #1 certificate SHA-256 digest: $expected_cert" <<<"$signature" || {
    printf 'error: %s does not use the release signing certificate\n' "$apk" >&2
    exit 1
}
