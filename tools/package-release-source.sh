#!/usr/bin/env bash
set -euo pipefail

output_dir="${1:?Usage: package-release-source.sh OUTPUT_DIR}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/build-common.sh
source "$root/tools/lib/build-common.sh"
version="$(read_project_version "$root")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
source_root="$work/Spool-$version-source"
mkdir -p "$source_root/mpv" "$output_dir"

git -C "$root" archive --format=tar HEAD | tar -xf - -C "$source_root"
git -C "$root/mpv" archive --format=tar HEAD | tar -xf - -C "$source_root/mpv"
{
  printf 'spool %s\n' "$(git -C "$root" rev-parse HEAD)"
  printf 'mpv %s\n' "$(git -C "$root/mpv" rev-parse HEAD)"
  printf 'version %s\n' "$version"
} >"$source_root/SOURCE-REVISIONS.txt"

archive="$output_dir/Spool-$version-corresponding-source.tar.gz"
TZ=UTC tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
  -czf "$archive" -C "$work" "$(basename "$source_root")"
tar -tzf "$archive" >/dev/null
sha256sum "$archive" >"$archive.sha256"
printf '%s\n' "$archive"
