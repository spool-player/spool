#!/usr/bin/env python3
"""Extract pinned source archives where Windows inbox tar is unavailable."""

import argparse
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("archive", type=Path)
parser.add_argument("destination", type=Path)
parser.add_argument("--strip-components", type=int, default=0)
args = parser.parse_args()
if args.strip_components < 0:
    parser.error("--strip-components must be nonnegative")
args.destination.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.archive) as archive:
    members = []
    for member in archive:
        parts = member.name.split("/")[args.strip_components:]
        if not parts or not any(parts):
            continue
        member = member.replace(name="/".join(parts))
        if member.islnk():
            member = member.replace(linkname="/".join(member.linkname.split("/")[args.strip_components:]))
        members.append(member)
    archive.extractall(args.destination, members=members, filter="data")
