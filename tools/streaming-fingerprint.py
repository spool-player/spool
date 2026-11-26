#!/usr/bin/env python3
"""Content identity for a target's private TLS/HTTP closure."""
import argparse
import hashlib
import json
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--manifest', type=Path)
parser.add_argument('--target', required=True, help='ABI, API/SDK, linkage and compiler identity')
parser.add_argument('--wrapper', type=Path, action='append', default=[])
args = parser.parse_args()
root = args.root.resolve()
manifest = args.manifest or root / 'tools/manifests/streaming-third-party.json'
files = [manifest, root / 'tools/prepare-streaming-sources.sh',
         root / 'tools/lib/manifest-sources.sh', root / 'tools/cmake/StreamingDependencies.cmake',
         Path(__file__).resolve(), *args.wrapper]
for source in json.loads(manifest.read_text())['sources'].values():
    files.extend(root / patch for patch in source.get('patches', []))
identity = hashlib.sha256(args.target.encode())
for path in sorted(set(files)):
    identity.update(path.name.encode() + b'\0' + path.read_bytes() + b'\0')
print(identity.hexdigest())
