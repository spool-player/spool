#!/usr/bin/env bash
set -euo pipefail

expected="${1:?usage: check-release-version.sh VERSION}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$expected" =~ ^0\.[0-9]+\.[0-9]+$ ]] || {
  printf 'release version must be 0.x.y, got %s\n' "$expected" >&2
  exit 1
}
actual="$(tr -d '[:space:]' <"$root/VERSION")"
[[ "$actual" == "$expected" ]] || {
  printf 'VERSION is %s, tag requests %s\n' "$actual" "$expected" >&2
  exit 1
}

grep -Fq 'project(JellyfinNativeWebOS VERSION "${JELLYFIN_VERSION}"' "$root/CMakeLists.txt"
grep -Fq 'JELLYFIN_VERSION="${PROJECT_VERSION}"' "$root/CMakeLists.txt"
grep -Fq '"version": "@VERSION@"' "$root/app/appinfo.json.in"
grep -Fq "<release version=\"$expected\"" "$root/app/com.sachk.spool.metainfo.xml"
grep -Fq 'VIProductVersion "${VERSION}.0"' "$root/tools/windows/installer.nsi"
grep -Fq 'VIProductVersion "${VERSION}.0"' "$root/tools/windows/portable.nsi"
grep -Fq 'read_project_version "$APP_ROOT"' "$root/tools/package-appimage.sh"
grep -Fq 'read_project_version "$APP_ROOT"' "$root/tools/package-macos-dmg.sh"
grep -Fq "(Join-Path \$root 'VERSION')" "$root/tools/windows/package.ps1"
# The AUR recipe names the release it downloads rather than reading VERSION,
# so it is the one file a bump can silently leave behind.
grep -Fqx "pkgver=$expected" "$root/packaging/aur/PKGBUILD"
grep -Fqx "	pkgver = $expected" "$root/packaging/aur/.SRCINFO"
printf 'validated release version %s across native, webOS, macOS, AppImage, Arch, and Windows metadata\n' "$expected"
