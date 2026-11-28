#!/usr/bin/env python3
"""Exercise the packaged Plex provider against fake and unclaimed local PMS instances."""

from compatibility_receipt import compatibility_runner, fixture_failure, receipt_results, set_phase

import argparse
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
from threading import Thread
import time
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, urlopen

VERSION = '1.43.5.11079-b3b13e506'
DIGEST = 'sha256:32dead43c01e8ff6189504ff0d048b16cf084a1dc37d8906fef8f3537d067e4d'
IMAGE = 'docker.io/plexinc/pms-docker'


@contextmanager
def fake_server(media, version):
    video = media.read_bytes()
    state = {'rating': 0, 'starts': 0, 'stops': 0, 'mediaReads': 0}
    machine = {'machineIdentifier': 'spool-fixture-server', 'version': version, 'claimed': False}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def reply(self, status, value=None):
            data = b'' if value is None else json.dumps({'MediaContainer': value}).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            if data:
                self.wfile.write(data)

        def do_GET(self):
            request = urlsplit(self.path)
            path, query = request.path, parse_qs(request.query)
            if path == '/identity':
                return self.reply(200, machine)
            if self.headers.get('X-Plex-Token') != 'fixture-token':
                return self.reply(401)
            part = {'id': 1, 'key': '/library/parts/1/Synthetic.mkv', 'size': len(video), 'container': 'mkv'}
            edition = {'id': 1, 'container': 'mkv', 'videoCodec': 'h264', 'bitrate': 500,
                       'width': 160, 'height': 90, 'duration': 15000, 'Part': [part]}
            item = {'ratingKey': '1', 'type': 'movie', 'title': 'Synthetic Film', 'duration': 15000,
                    'Media': [edition], 'userRating': state['rating']}
            if path == '/':
                return self.reply(200, machine)
            if path == '/library/sections':
                return self.reply(200, {'Directory': [{'key': '1', 'type': 'movie', 'title': 'Movies'}]})
            if path in ('/library/sections/1/all', '/library/metadata/1'):
                return self.reply(200, {'size': 1, 'totalSize': 1, 'Metadata': [item]})
            if path == '/hubs/search':
                return self.reply(200, {'Hub': [{'type': 'movie', 'Metadata': [item]}]})
            if path == '/:/rate' and query.get('key') == ['1']:
                state['rating'] = int(query['rating'][0])
                return self.reply(200, {})
            if path == '/:/timeline' and query.get('ratingKey') == ['1']:
                key = {'playing': 'starts', 'stopped': 'stops'}.get(query.get('state', [''])[0])
                if key:
                    state[key] += 1
                return self.reply(200, {})
            if path == part['key'] and query.get('download') == ['1']:
                match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
                start = int(match[1]) if match else 0
                end = min(int(match[2]), len(video) - 1) if match and match[2] else len(video) - 1
                if start > end:
                    return self.reply(416)
                self.send_response(206 if match else 200)
                if match:
                    self.send_header('Content-Range', f'bytes {start}-{end}/{len(video)}')
                self.send_header('Content-Type', 'video/x-matroska')
                self.send_header('Accept-Ranges', 'bytes')
                self.send_header('Content-Length', str(end - start + 1))
                self.end_headers()
                if self.command != 'HEAD':
                    state['mediaReads'] += 1
                    self.wfile.write(video[start:end + 1])
                return
            return self.reply(404)

        do_PUT = do_HEAD = do_GET

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = Thread(target=server.serve_forever)
        thread.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}', state
        finally:
            server.shutdown()
            thread.join()


def provision(origin, version, engine, container):
    def call(path):
        headers = {'Accept': 'application/json', 'X-Plex-Client-Identifier': 'spool-pms-scanner-fixture'}
        with urlopen(Request(origin + path, headers=headers), timeout=8) as response:
            return json.load(response)['MediaContainer']

    deadline = time.monotonic() + 50
    while time.monotonic() < deadline:
        try:
            identity = call('/identity')
            if identity.get('version'):
                break
        except OSError:
            pass
        time.sleep(0.25)
    else:
        raise RuntimeError('PMS startup timeout')
    if identity['version'] != version or identity.get('claimed') not in (False, 0, '0'):
        raise RuntimeError('PMS must be unclaimed and report the pinned version')

    # The vendor's local scanner provisions the same library database that PMS
    # serves. No claim code, account login or Plex cloud request is needed.
    subprocess.run([engine, 'exec', '--user', str(os.getuid()),
                    '-e', 'PLEX_MEDIA_SERVER_APPLICATION_SUPPORT_DIR=/config/Library/Application Support',
                    '-e', 'LD_LIBRARY_PATH=/usr/lib/plexmediaserver', container,
                    '/usr/lib/plexmediaserver/Plex Media Scanner', '--add-section', 'Spool Fixture Movies',
                    '--type', '1', '--agent', 'tv.plex.agents.movie', '--location', '/data', '--lang', 'en-US'],
                   check=True, timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sections = call('/library/sections')['Directory']
    if len(sections) != 1:
        raise RuntimeError('PMS scanner must create exactly one local section')
    section = sections[0]['key']
    with urlopen(f'{origin}/library/sections/{section}/refresh', timeout=8):
        pass
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        media = call(f'/library/sections/{section}/all').get('Metadata', [])
        if any(item.get('Media') for item in media):
            return
        time.sleep(0.25)
    raise RuntimeError('PMS synthetic media scan timeout')


@contextmanager
def upstream_server(root, version, image_digest, engine):
    image = f'{IMAGE}@{image_digest}'
    container = root.name
    volume = f'{container}-config'
    podman = ['--userns=keep-id', '--user', '0:0'] if Path(engine).name == 'podman' else []
    quiet = {'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    subprocess.run([engine, 'pull', image], check=True, timeout=180, **quiet)
    subprocess.run([engine, 'volume', 'create', volume], check=True, timeout=15, **quiet)
    try:
        subprocess.run([engine, 'run', '--detach', '--rm', '--pull=never', '--no-healthcheck', '--name', container,
                        *podman, '--publish', '127.0.0.1::32400', '--volume', f'{volume}:/config',
                        '--volume', f'{root / "movies"}:/data:ro', '--env', f'PLEX_UID={os.getuid()}',
                        '--env', f'PLEX_GID={os.getgid()}', '--env', 'CHANGE_CONFIG_DIR_OWNERSHIP=false',
                        '--env', 'ALLOWED_NETWORKS=127.0.0.1/32,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16', image],
                       check=True, timeout=60, **quiet)
        port = subprocess.check_output([engine, 'port', container, '32400/tcp'], text=True, timeout=15).strip()
        if not re.fullmatch(r'127\.0\.0\.1:[0-9]+', port):
            raise RuntimeError('PMS container did not assign a loopback port')
        origin = f'http://{port}'
        provision(origin, version, engine, container)
        yield origin, None
    finally:
        subprocess.run([engine, 'rm', '--force', container], check=False, timeout=15, **quiet)
        subprocess.run([engine, 'volume', 'rm', volume], check=True, timeout=15, **quiet)


@compatibility_runner('plex')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spool-tests', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--version', default=VERSION)
    parser.add_argument('--image-digest', default=DIGEST)
    parser.add_argument('--container-engine', default='docker')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){3}-[0-9a-f]{9}', args.version) or not re.fullmatch(r'sha256:[0-9a-f]{64}', args.image_digest):
        parser.error('configure a stable PMS build version and an immutable sha256 digest')
    results = receipt_results()
    set_phase("synthetic-media")
    with tempfile.TemporaryDirectory(prefix='spool-plex-compat-') as data:
        root = Path(data)
        media = root / 'movies' / 'Synthetic Film (2026).mkv'
        media.parent.mkdir()
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'testsrc2=size=160x90:rate=12:duration=15', '-c:v', 'libx264', str(media)],
                       check=True, timeout=30)
        fixtures = {'fake': fake_server(media, args.version),
                    'upstream': upstream_server(root, args.version, args.image_digest, args.container_engine)}
        for name, fixture in fixtures.items():
            try:
                set_phase("server-startup")
                with fixture as (origin, counts):
                    set_phase("selector")
                    process = subprocess.run([str(args.spool_tests.resolve()), '--child', 'plex-provider-compatibility'], timeout=100,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                             env=os.environ | {'SPOOL_TEST_PLEX_ORIGIN': origin,
                                                               'NO_PROXY': '127.0.0.1,localhost', 'no_proxy': '127.0.0.1,localhost'})
                    for line in process.stderr.splitlines():
                        if line.startswith('spool-test-require: '):
                            print(f'{name}: {line}')
                    passed = process.returncode == 0
                    results[name] = {'selectorExitCode': process.returncode, 'passed': passed}
                    if counts is not None:
                        results[name]['wireCounts'] = {key: counts[key] for key in ('starts', 'stops', 'mediaReads')}
                        results[name]['passed'] = passed and all(counts[key] > 0 for key in ('starts', 'stops', 'mediaReads'))
            except (OSError, RuntimeError, KeyError, ValueError, subprocess.SubprocessError) as error:
                fixture_failure(results, name, error)
            print(f"Plex {name}: {'passed' if results[name]['passed'] else 'failed'}")
            set_phase("cleanup")
    set_phase("cleanup")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'version': args.version, 'imageDigest': args.image_digest,
                                      'fixtures': results}, indent=2) + '\n', encoding='utf-8')
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
