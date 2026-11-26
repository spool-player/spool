#!/usr/bin/env python3
"""Find stable provider server releases newer than the checked-in baseline."""

import argparse
import json
import re
import sys
from urllib.error import HTTPError
from urllib.request import urlopen

UPSTREAM = {
    'jellyfin': ('12.4', r'v([0-9]+\.[0-9]+(?:\.[0-9]+)?)', 'jellyfin/jellyfin'),
    'emby': ('4.10.2.0', r'([0-9]+(?:\.[0-9]+){3})', 'emby/embyserver'),
    'plex': ('1.43.5.11079-b3b13e506', r'([0-9]+(?:\.[0-9]+){3}-[0-9a-f]{9})', 'plexinc/pms-docker'),
}


def version_parts(version):
    parts = tuple(map(int, version.split('-', 1)[0].split('.')))
    return parts + (0,) * (4 - len(parts))


def candidates(releases, baseline, tag_pattern):
    candidates = set()
    for release in releases:
        if release['draft'] or release['prerelease']:
            continue
        match = re.fullmatch(tag_pattern, release['tag_name'])
        if match and version_parts(match[1]) > version_parts(baseline):
            candidates.add(match[1])
    return sorted(candidates, key=version_parts)


def image_digest(repository, version):
    try:
        with urlopen(f'https://hub.docker.com/v2/repositories/{repository}/tags/{version}', timeout=30) as response:
            images = json.load(response)['images']
    except HTTPError as error:
        if error.code != 404:
            raise
        return None
    for image in images:
        if image['os'] == 'linux' and image['architecture'] == 'amd64':
            digest = image['digest']
            if re.fullmatch(r'sha256:[0-9a-f]{64}', digest):
                return digest
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('provider', choices=UPSTREAM)
    parser.add_argument('--baseline')
    args = parser.parse_args()
    baseline, tag_pattern, repository = UPSTREAM[args.provider]
    releases = json.load(sys.stdin)
    if args.provider == 'plex':
        # Plex's public downloads index reports the current stable Linux PMS.
        releases = [{'tag_name': releases['computer']['Linux']['version'], 'draft': False, 'prerelease': False}]
    for version in candidates(releases, args.baseline or baseline, tag_pattern):
        if digest := image_digest(repository, version):
            print(args.provider, version, digest, sep='\t')
        else:
            print(f'{args.provider} {version}: waiting for the official Linux x86-64 image', file=sys.stderr)


if __name__ == '__main__':
    main()
