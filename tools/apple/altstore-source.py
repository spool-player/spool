#!/usr/bin/env python3
"""Write the SideStore/AltStore source for a release's unsigned iOS IPA.

SideStore and AltStore re-sign an IPA with the viewer's own Apple ID, so the
unsigned release IPA installs on an iPhone without a developer account. A
source is the JSON they poll for updates. Everything they compare against the
installed app -- bundle identifier, version, build number, minimum iOS -- is
read from the IPA's own Info.plist rather than restated, because a source
that disagrees with its IPA is refused at install time.
"""

import argparse
import json
import os
import plistlib
import sys
import zipfile

DESCRIPTION = (
    "Spool plays your own media: Jellyfin, Plex, Emby and more, with a fast library that stays smooth "
    "however large it grows. This build is installed and refreshed through SideStore or AltStore with "
    "your own Apple ID."
)


def bundle_info(ipa):
    with zipfile.ZipFile(ipa) as archive:
        names = [name for name in archive.namelist()
                 if name.startswith("Payload/") and name.endswith(".app/Info.plist") and name.count("/") == 2]
        if len(names) != 1:
            raise SystemExit(f"{ipa}: expected exactly one Payload/*.app/Info.plist, found {len(names)}")
        return plistlib.loads(archive.read(names[0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ipa")
    parser.add_argument("--download-url", required=True, help="where the IPA is published")
    parser.add_argument("--icon-url", required=True)
    parser.add_argument("--date", required=True, help="release date, YYYY-MM-DD")
    parser.add_argument("--notes", help="file with this version's release notes")
    parser.add_argument("--website", default="https://github.com/spool-player/spool")
    args = parser.parse_args()

    info = bundle_info(args.ipa)
    for key in ("CFBundleIdentifier", "CFBundleShortVersionString", "CFBundleVersion"):
        if not info.get(key):
            raise SystemExit(f"{args.ipa}: Info.plist has no {key}")
    notes = open(args.notes, encoding="utf-8").read().strip() if args.notes else ""
    version = {
        "version": info["CFBundleShortVersionString"],
        "buildVersion": info["CFBundleVersion"],
        "date": args.date,
        "downloadURL": args.download_url,
        "size": os.path.getsize(args.ipa),
        "minOSVersion": info.get("MinimumOSVersion", "17.0"),
    }
    if notes:
        version["localizedDescription"] = notes
    privacy = {key: value for key, value in info.items() if key.startswith("NS") and key.endswith("UsageDescription")}
    source = {
        "name": "Spool",
        "identifier": info["CFBundleIdentifier"] + ".source",
        "subtitle": "Your media, on every screen",
        "description": DESCRIPTION,
        "iconURL": args.icon_url,
        "website": args.website,
        "tintColor": "#0077A0",
        "apps": [{
            "name": info.get("CFBundleDisplayName", "Spool"),
            "bundleIdentifier": info["CFBundleIdentifier"],
            "developerName": "Spool",
            "subtitle": "Plays your Jellyfin, Plex and Emby libraries",
            "localizedDescription": DESCRIPTION,
            "iconURL": args.icon_url,
            "tintColor": "#0077A0",
            "category": "entertainment",
            "screenshots": [],
            "versions": [version],
            # Free Apple IDs can sign only ordinary apps: Spool asks for no
            # entitlements, which is what keeps it installable that way.
            "appPermissions": {"entitlements": [], "privacy": privacy},
        }],
        "news": [],
    }
    json.dump(source, sys.stdout, indent=2)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
