#!/usr/bin/env python3
"""A SideStore/AltStore source must describe its IPA exactly, or installs are refused."""

from __future__ import annotations

import json
import plistlib
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def ipa(path: Path, info: dict, payload: bytes = b"\0" * 4096) -> Path:
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("Payload/Spool.app/Info.plist", plistlib.dumps(info))
        archive.writestr("Payload/Spool.app/Spool", payload)
        archive.writestr("Payload/Spool.app/Frameworks/Qt.framework/Info.plist", plistlib.dumps({}))
    return path


def source(script: Path, package: Path, notes: Path | None = None) -> subprocess.CompletedProcess:
    command = [sys.executable, str(script), str(package), "--download-url", "https://example.org/Spool.ipa",
               "--icon-url", "https://example.org/icon.png", "--date", "2026-10-22"]
    if notes:
        command += ["--notes", str(notes)]
    return subprocess.run(command, capture_output=True, text=True)


def main() -> None:
    script = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        info = {"CFBundleIdentifier": "com.sachk.spool", "CFBundleShortVersionString": "0.9.2",
                "CFBundleVersion": "0.9.2", "CFBundleDisplayName": "Spool", "MinimumOSVersion": "17.0",
                "NSLocalNetworkUsageDescription": "Servers at home."}
        package = ipa(root / "Spool.ipa", info)
        notes = root / "notes.md"
        notes.write_text("Faster libraries.\n")
        result = source(script, package, notes)
        assert result.returncode == 0, result.stderr
        document = json.loads(result.stdout)
        app = document["apps"][0]
        version = app["versions"][0]
        assert app["bundleIdentifier"] == "com.sachk.spool", "bundle identifier comes from the IPA"
        assert (version["version"], version["buildVersion"]) == ("0.9.2", "0.9.2"), "versions come from the IPA"
        assert version["minOSVersion"] == "17.0", "minimum iOS comes from the IPA"
        assert version["size"] == package.stat().st_size, "size is the published file's"
        assert version["downloadURL"] == "https://example.org/Spool.ipa"
        assert version["localizedDescription"] == "Faster libraries."
        assert app["appPermissions"]["entitlements"] == [], "a free Apple ID can sign no entitlements"
        assert app["appPermissions"]["privacy"] == {"NSLocalNetworkUsageDescription": "Servers at home."}

        # An IPA without its versions, or with an ambiguous payload, is not published.
        broken = ipa(root / "Broken.ipa", {"CFBundleIdentifier": "com.sachk.spool"})
        assert source(script, broken).returncode != 0, "missing versions fail the release"
        ambiguous = root / "Ambiguous.ipa"
        with zipfile.ZipFile(ambiguous, "w") as archive:
            archive.writestr("Payload/A.app/Info.plist", plistlib.dumps(info))
            archive.writestr("Payload/B.app/Info.plist", plistlib.dumps(info))
        assert source(script, ambiguous).returncode != 0, "two apps in one IPA fail the release"
    print("AltStore source checks passed")


if __name__ == "__main__":
    main()
