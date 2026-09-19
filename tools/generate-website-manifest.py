#!/usr/bin/env python3

"""Describe the complete set of final, installable release assets for the website."""

import argparse
import hashlib
import json
import re
from pathlib import Path
from urllib.parse import quote


PLATFORMS = (
    ("webos", "LG webOS TV", (
        ("arm", "IPK", "arm", "ipk", "com.sachk.spool_{version}_arm.ipk"),
    )),
    ("android", "Android phone or TV", (
        ("universal", "Universal", "universal", "apk", "spool-universal.apk"),
        ("arm64-v8a", "64-bit ARM", "arm64-v8a", "apk", "spool-arm64-v8a.apk"),
        ("armeabi-v7a", "32-bit ARM", "armeabi-v7a", "apk", "spool-armeabi-v7a.apk"),
        ("x86_64", "x86-64", "x86_64", "apk", "spool-x86_64.apk"),
    )),
    ("windows", "Windows", (
        ("installer", "Installer", "x86_64", "exe", "Spool-for-Jellyfin-{version}-Windows-x64-Setup.exe"),
        ("portable", "Portable", "x86_64", "exe", "Spool-for-Jellyfin-{version}-Windows-x64-Portable.exe"),
    )),
    ("macos", "macOS", (
        ("arm64", "Apple Silicon", "arm64", "dmg", "Spool-for-Jellyfin-{version}-macOS-arm64.dmg"),
        ("x86_64", "Intel", "x86_64", "dmg", "Spool-for-Jellyfin-{version}-macOS-x86_64.dmg"),
    )),
    ("linux", "Linux", (
        ("appimage", "AppImage", "x86_64", "AppImage", "Spool-for-Jellyfin-{version}-x86_64.AppImage"),
        ("portable", "Portable tarball", "x86_64", "tar.zst", "Spool-for-Jellyfin-{version}-linux-x86_64.tar.zst"),
        ("arch", "Arch Linux package", "x86_64", "pkg.tar.zst", "spool-bin-{version}-*-x86_64.pkg.tar.zst"),
    )),
)


def generate_manifest(assets: Path, tag: str, repository: str) -> dict:
    if not re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", tag):
        raise ValueError("release tag must be vMAJOR.MINOR.PATCH without leading zeros")
    if repository != "spool-player/spool":
        raise ValueError("website metadata must describe spool-player/spool releases")
    if not assets.is_dir():
        raise ValueError("final release asset directory is missing")
    version = tag[1:]
    release_url = f"https://github.com/{repository}/releases/tag/{tag}"
    download_base = f"https://github.com/{repository}/releases/download/{tag}"
    platforms = []
    selected = set()
    for platform_id, platform_label, packages in PLATFORMS:
        downloads = []
        for package_id, label, architecture, package_format, pattern in packages:
            matches = sorted(assets.glob(pattern.format(version=version)))
            if len(matches) != 1:
                raise ValueError(f"expected exactly one {platform_id}/{package_id} final package, found {len(matches)}")
            package = matches[0]
            if package.is_symlink() or not package.is_file():
                raise ValueError(f"{platform_id}/{package_id} is not a regular final package")
            if not re.fullmatch(r"[A-Za-z0-9_.+-]+", package.name):
                raise ValueError("release package filename contains unsupported characters")
            size = package.stat().st_size
            if size <= 0:
                raise ValueError(f"{platform_id}/{package_id} final package is empty")
            with package.open("rb") as source:
                checksum = hashlib.file_digest(source, "sha256").hexdigest()
            downloads.append({
                "id": package_id,
                "label": label,
                "architecture": architecture,
                "format": package_format,
                "name": package.name,
                "url": f"{download_base}/{quote(package.name, safe='')}",
                "sha256": checksum,
                "size": size,
            })
            selected.add(package)
        platforms.append({"id": platform_id, "label": platform_label, "downloads": downloads})
    installable_suffixes = (".ipk", ".apk", ".exe", ".dmg", ".AppImage", ".tar.zst")
    if any(path not in selected and path.name.endswith(installable_suffixes) for path in assets.iterdir()):
        raise ValueError("an unrecognized installable final package would be omitted from website metadata")
    return {
        "schemaVersion": 1,
        "release": {"version": version, "tag": tag, "releaseUrl": release_url, "platforms": platforms},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repository", default="spool-player/spool")
    args = parser.parse_args()
    manifest = generate_manifest(args.assets, args.tag, args.repository)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"website manifest generation failed: {error}") from None
