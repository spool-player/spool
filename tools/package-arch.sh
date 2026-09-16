#!/usr/bin/env bash
set -euo pipefail

# Build the Arch binary package from the portable tarball, using the same
# PKGBUILD that packaging/aur ships. makepkg is what writes a pacman package
# anyone can trust, and it only runs on Arch, so an Arch container is the whole
# of that dependency -- everything else here is copying and checking.
#
# Run tools/package-linux-bundle.sh first; its tarball is the only input.

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARTIFACT_DIR="${ARTIFACT_DIR:-$APP_ROOT/dist}"
# shellcheck source=tools/lib/arch-container.sh
source "$APP_ROOT/tools/lib/arch-container.sh"
arch_container_reexec "$APP_ROOT" tools/package-arch.sh "$@"

# shellcheck source=tools/lib/build-common.sh
source "$APP_ROOT/tools/lib/build-common.sh"
APP_VERSION="$(read_project_version "$APP_ROOT")"
PKGBUILD="$APP_ROOT/packaging/aur/PKGBUILD"
BUNDLE="Spool-for-Jellyfin-${APP_VERSION}-linux-x86_64"
TARBALL="$ARTIFACT_DIR/$BUNDLE.tar.zst"

[[ -f "$TARBALL" ]] || {
  echo "error: portable bundle not found at $TARBALL; run tools/package-linux-bundle.sh first" >&2
  exit 1
}
# The published PKGBUILD names the release it downloads, so a version bump that
# misses it would ship an installer for the previous release.
grep -Fqx "pkgver=$APP_VERSION" "$PKGBUILD" || {
  echo "error: $PKGBUILD does not set pkgver=$APP_VERSION" >&2
  exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$PKGBUILD" "$TARBALL" "$work/"
mkdir -p "$work/out"
arch_container_ensure_builder
chown -R builder "$work"

# The tarball is already beside the PKGBUILD, so nothing is downloaded and the
# recorded checksum -- which belongs to the previous release until this one is
# published -- has nothing to say about the file being packaged.
arch_container_runuser env -C "$work" \
  HOME=/home/builder PKGDEST="$work/out" SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-0}" \
  makepkg --force --nodeps --skipchecksums --noconfirm

package="$(find "$work/out" -maxdepth 1 -name '*.pkg.tar.zst' -print -quit)"
[[ -n "$package" ]] || { echo 'error: makepkg produced no package' >&2; exit 1; }

# A package that installs nothing under either prefix still builds cleanly.
contents="$(bsdtar -tf "$package")"
for entry in usr/bin/spool opt/spool/AppRun usr/share/applications/com.sachk.spool.desktop; do
  grep -Fqx "$entry" <<<"$contents" || {
    echo "error: $entry is missing from $(basename "$package")" >&2
    exit 1
  }
done
pacman --query --info --file "$package"

mkdir -p "$ARTIFACT_DIR"
install -m 0644 "$package" "$ARTIFACT_DIR/"
arch_container_restore_owner "$APP_ROOT" "$ARTIFACT_DIR/$(basename "$package")"
printf '%s\n' "$ARTIFACT_DIR/$(basename "$package")"
