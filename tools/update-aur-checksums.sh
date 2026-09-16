#!/usr/bin/env bash
set -euo pipefail

# Record a published tarball's checksum in the AUR recipe and regenerate its
# .SRCINFO. The recipe cannot carry the checksum before the release exists, so
# the release workflow runs this once the assets are up and commits the result;
# it is also what to run by hand after uploading a release manually.
#
# usage: update-aur-checksums.sh SHA256|TARBALL

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/arch-container.sh
source "$APP_ROOT/tools/lib/arch-container.sh"

checksum="${1:?usage: update-aur-checksums.sh SHA256|TARBALL}"
if [[ ! "$checksum" =~ ^[0-9a-f]{64}$ ]]; then
  [[ -f "$checksum" ]] || {
    echo "error: not a sha256 or a readable file: $checksum" >&2
    exit 1
  }
  checksum="$(sha256sum "$checksum" | cut -d' ' -f1)"
fi

# .SRCINFO is generated, never edited, so the whole update runs where makepkg is.
arch_container_reexec "$APP_ROOT" tools/update-aur-checksums.sh "$checksum"

# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
APP_VERSION="$(read_project_version "$APP_ROOT")"
PKGBUILD="$APP_ROOT/packaging/aur/PKGBUILD"
SRCINFO="$APP_ROOT/packaging/aur/.SRCINFO"

# A checksum written against the wrong pkgver would fail for every user of the
# recipe and for nobody here, so it is checked before anything is written.
grep -Fqx "pkgver=$APP_VERSION" "$PKGBUILD" || {
  echo "error: $PKGBUILD does not set pkgver=$APP_VERSION" >&2
  exit 1
}
[[ "$(grep -c '^sha256sums=' "$PKGBUILD")" == 1 ]] || {
  echo "error: expected exactly one sha256sums line in $PKGBUILD" >&2
  exit 1
}
sed -i "s|^sha256sums=.*|sha256sums=('$checksum')|" "$PKGBUILD"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$PKGBUILD" "$work/"
arch_container_ensure_builder
chown -R builder "$work"
arch_container_runuser env -C "$work" HOME=/home/builder makepkg --printsrcinfo >"$SRCINFO"

grep -Fqx "	sha256sums = $checksum" "$SRCINFO"
grep -Fqx "	pkgver = $APP_VERSION" "$SRCINFO"
arch_container_restore_owner "$APP_ROOT" "$PKGBUILD" "$SRCINFO"
printf 'recorded %s for Spool-for-Jellyfin-%s-linux-x86_64.tar.zst\n' "$checksum" "$APP_VERSION"
