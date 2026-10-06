#!/usr/bin/env bash
set -euo pipefail

asset_dir="${1:?usage: analyze-release-artifacts.sh ASSET_DIR REPORT_DIR}"
report_dir="${2:?usage: analyze-release-artifacts.sh ASSET_DIR REPORT_DIR}"
: "${SYFT:=syft}"
: "${GRYPE:=grype}"
mkdir -p "$report_dir"
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' <"$root_dir/VERSION")"
asset_dir="$(cd "$asset_dir" && pwd)"
report_dir="$(cd "$report_dir" && pwd)"
# The unsigned IPA is a required device release, never a simulator or loose app.
[[ -f "$asset_dir/Spool-$version-tvOS-arm64.ipa" ]] || {
  echo "missing tvOS device release package: Spool-$version-tvOS-arm64.ipa" >&2
  exit 1
}
work=""
trap '[[ -z "$work" ]] || rm -rf "$work"' EXIT
# Grype's commit-based FFmpeg advisories do not recognize upstream release
# backports. Keep the exact component version and fix evidence in OpenVEX;
# every other finding still goes through the unchanged high-severity gate.
jq --arg product "pkg:generic/spool@$version" \
  '.statements[].products |= map({"@id": $product, "subcomponents": [.]})' \
  "$root_dir/tools/manifests/ffmpeg-vex.json" >"$report_dir/ffmpeg-vex.json"
shopt -s nullglob
assets=("$asset_dir"/*.AppImage "$asset_dir"/*.dmg "$asset_dir"/*-Portable.exe "$asset_dir"/*-Setup.exe "$asset_dir"/*.ipk "$asset_dir"/*.ipa)
(( ${#assets[@]} > 0 )) || { echo 'no release packages found' >&2; exit 1; }

for asset in "${assets[@]}"; do
  base="$(basename "$asset")"
  work="$(mktemp -d)"
  root="$work/root"
  mkdir -p "$root"
  case "$asset" in
    *.AppImage)
      chmod +x "$asset"
      (cd "$work" && "$asset" --appimage-extract >/dev/null)
      if [[ -d "$work/AppDir" ]]; then
        mv "$work/AppDir" "$root/appimage"
      else
        mv "$work/squashfs-root" "$root/appimage"
      fi
      ;;
    *.dmg)
      analysis_input="${asset}.app.tar.gz"
      [[ -f "$analysis_input" ]] || {
        echo "missing macOS analysis input: $(basename "$analysis_input")" >&2
        exit 1
      }
      tar -xzf "$analysis_input" -C "$root"
      ;;
    *-Portable.exe|*-Setup.exe)
      7z x -y -o"$root" "$asset" >/dev/null
      ;;
    *.ipk)
      (cd "$work" && ar x "$asset")
      data_archive="$(find "$work" -maxdepth 1 -name 'data.tar.*' -print -quit)"
      [[ -n "$data_archive" ]] || { echo "missing IPK data archive: $base" >&2; exit 1; }
      tar -xf "$data_archive" -C "$root"
      ;;
    *.ipa)
      # Audit archive paths, embedded version, device platform and Mach-O closure
      # before letting scanners read any extracted unsigned/signable payload.
      python3 "$root_dir/tools/package-audit.py" tvos-ipa "$asset" \
        --version "$version" --extract "$root"
      python3 "$root_dir/tools/package-audit.py" inventory "$root" \
        --output "$report_dir/$base.inventory.tsv"
      ;;
  esac
  "$SYFT" "dir:$root" --source-name spool --source-version "$version" \
    -o "cyclonedx-json=$report_dir/$base.cdx.json" \
    -o "spdx-json=$report_dir/$base.spdx.json" \
    -o "syft-json=$report_dir/$base.syft.json"
  "$GRYPE" "sbom:$report_dir/$base.syft.json" --vex "$report_dir/ffmpeg-vex.json" \
    -o json >"$report_dir/$base.grype.json"
  "$GRYPE" "sbom:$report_dir/$base.syft.json" --vex "$report_dir/ffmpeg-vex.json" --fail-on high --only-fixed
  rm -rf "$work"
  work=""
done
