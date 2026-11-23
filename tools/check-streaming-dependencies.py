#!/usr/bin/env python3
"""Audit installed cross-built transport metadata; does not execute target code."""
import argparse
import json
import re
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--prefix', type=Path, required=True)
parser.add_argument('--manifest', type=Path, required=True)
parser.add_argument('--bash', default='bash', help='Host shell for generated curl-config')
parser.add_argument('--require-native-ca', action='store_true')
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())['sources']
config = args.prefix / 'bin/curl-config'
features = set(subprocess.check_output([args.bash, str(config).replace('\\', '/'), '--features'], text=True).split())
required = {'HTTP2', 'HTTP3', 'alt-svc'}
if args.require_native_ca:
    required.add('NativeCA')
missing = required - features
if missing:
    parser.error('curl is missing required features: ' + ', '.join(sorted(missing)))
version = subprocess.check_output([args.bash, str(config).replace('\\', '/'), '--version'], text=True).strip().split()[-1]
if version != manifest['curl']['version']:
    parser.error(f'curl version {version} differs from pin')
for package, library in [('openssl', 'openssl'), ('nghttp2', 'libnghttp2'),
                         ('nghttp3', 'libnghttp3'), ('ngtcp2', 'libngtcp2')]:
    pc = args.prefix / 'lib/pkgconfig' / (library + '.pc')
    if pc.exists():
        match = re.search(r'^Version:\s*(\S+)', pc.read_text(), re.M)
    elif package == 'openssl':
        # OpenSSL's MSVC install writes no pkg-config file; its installed
        # version header states the same release.
        header = args.prefix / 'include/openssl/opensslv.h'
        match = re.search(r'^#\s*define\s+OPENSSL_VERSION_STR\s+"([^"]+)"', header.read_text(), re.M)
    else:
        match = None
    if not match or match[1] != manifest[package]['version']:
        parser.error(f'{package} version does not match pin')
for name in ['ssl', 'crypto']:
    files = [p for p in (args.prefix / 'lib').glob(f'lib{name}.*') if p.is_file() and not p.is_symlink()]
    print(f'{name}: ' + ', '.join(f'{p.name}={p.stat().st_size}' for p in files))
print('Transport metadata: HTTP2 HTTP3 alt-svc; pinned versions verified')
