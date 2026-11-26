#!/usr/bin/env python3
"""Exercise the bundled provider against a disposable, version-checked Jellyfin server."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as etree

from jellyfin_latency_e2e import JellyfinTestServer, generate_high_bitrate_movie

IMAGE_REPOSITORY = 'docker.io/jellyfin/jellyfin'


class ContainerJellyfin(JellyfinTestServer):
    def __init__(self, root, image_digest, media, engine):
        self.engine = engine
        self.image = f'{IMAGE_REPOSITORY}@{image_digest}'
        self.container = root.name
        super().__init__(root, '', media, username='spool-fixture', password='spool-fixture-password')

    def server_command(self, arguments):
        config_path = self.root / 'jf-config' / 'network.xml'
        tree = etree.parse(config_path)
        config = tree.getroot()
        # The bridge forwards a loopback host port to the container interface.
        # Jellyfin must accept on that interface inside its own namespace.
        config.remove(config.find('LocalNetworkAddresses'))
        tree.write(config_path, encoding='utf-8', xml_declaration=True)
        uid = f'{os.getuid()}:{os.getgid()}'
        user_namespace = ['--userns=keep-id'] if Path(self.engine).name == 'podman' else []
        return [self.engine, 'run', '--rm', '--pull=never', '--no-healthcheck', '--name', self.container, *user_namespace,
                '--user', uid, '--cap-drop=all', '--security-opt=no-new-privileges',
                '--publish', f'127.0.0.1:{self.port}:{self.port}',
                '--volume', f'{self.root}:{self.root}',
                '--env', f'XDG_CACHE_HOME={self.root / "jf-cache"}', self.image, *arguments]

    def stop(self):
        try:
            super().stop()
        finally:
            subprocess.run([self.engine, 'rm', '--force', self.container], check=False, timeout=15,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def release_tuple(version):
    parts = [int(part) for part in version.split('.')]
    while len(parts) > 2 and parts[-1] == 0:
        parts.pop()
    return parts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--jellyfin-bin', type=Path)
    source.add_argument('--image-digest')
    parser.add_argument('--container-engine', default='docker')
    parser.add_argument('--version', required=True)
    parser.add_argument('--spool-tests', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,2}', args.version):
        parser.error('version must be a stable Jellyfin release')
    if args.image_digest and not re.fullmatch(r'sha256:[0-9a-f]{64}', args.image_digest):
        parser.error('image digest must be an immutable sha256 digest')
    expected = release_tuple(args.version)
    report = {'provider': 'jellyfin', 'expectedVersion': args.version, 'passed': False, 'phase': 'prerequisites'}
    try:
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg or (args.jellyfin_bin and not args.jellyfin_bin.is_file()) or not args.spool_tests.is_file():
            raise RuntimeError('required test executable missing')
        with tempfile.TemporaryDirectory(prefix='spool-jellyfin-provider-') as data:
            root = Path(data)
            media = root / 'movies'
            report['phase'] = 'synthetic-media'
            generate_high_bitrate_movie(ffmpeg, media / 'Compatibility Fixture (2026).mkv', duration_s=8, video_mbps=2)
            report['phase'] = 'server-startup-and-scan'
            if args.image_digest:
                report['phase'] = 'pinned-container-image'
                report['imageDigest'] = args.image_digest
                server = ContainerJellyfin(root, args.image_digest, media, args.container_engine)
                subprocess.run([server.engine, 'pull', server.image], check=True, timeout=180,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            else:
                server = JellyfinTestServer(root, str(args.jellyfin_bin.resolve()), media,
                                            username='spool-fixture', password='spool-fixture-password')
            report['phase'] = 'server-startup-and-scan'
            try:
                server.start()
                report['phase'] = 'server-version'
                observed = release_tuple(server.server_version)
                report['observedVersion'] = '.'.join(map(str, observed))
                if observed != expected:
                    raise RuntimeError('server version does not match the selected release')
                report['phase'] = 'app-provider-contract'
                env = os.environ | {
                    'SPOOL_LIVE_JELLYFIN': server.base_url,
                    'SPOOL_LIVE_USER': server.username,
                    'SPOOL_LIVE_PASSWORD': server.password,
                    'NO_PROXY': '127.0.0.1,localhost',
                    'no_proxy': '127.0.0.1,localhost',
                }
                # The selector and the server may log authenticated traffic. Publish
                # only the phase and exit status, with no raw process output.
                result = subprocess.run([str(args.spool_tests.resolve()), 'live-jellyfin'], env=env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
                report['selectorExitCode'] = result.returncode
                report['passed'] = result.returncode == 0
            finally:
                server.stop()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        report['errorType'] = type(error).__name__
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('Jellyfin provider compatibility:', 'passed' if report['passed'] else f"failed in {report['phase']}")
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
