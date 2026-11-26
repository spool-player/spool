#!/usr/bin/env python3
"""Verify the bundled Emby provider against fake HTTP and an official server image."""

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
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

VERSION = '4.10.2.0'
DIGEST = 'sha256:1acca0c874c825c3194b2ef298938b1e310af7763e7c16d9d78d4e726de516ef'
IMAGE = 'docker.io/emby/embyserver'
USER = 'spool-fixture'
PASSWORD = 'spool-fixture-password'


@contextmanager
def fake_server(media, version):
    video = media.read_bytes()
    state = {'favorite': False, 'starts': 0, 'stops': 0, 'mediaReads': 0}
    source = {'Id': 'source', 'Container': 'mkv', 'Size': len(video), 'SupportsDirectPlay': True,
              'MediaStreams': [{'Type': 'Video', 'Codec': 'h264', 'Width': 160, 'Height': 90, 'Index': 0}]}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def reply(self, status, value=None):
            data = b'' if value is None else json.dumps(value).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            if data:
                self.wfile.write(data)

        def do_GET(self):
            self.dispatch()

        do_POST = do_DELETE = do_HEAD = do_GET

        def dispatch(self):
            path = urlsplit(self.path).path
            size = int(self.headers.get('Content-Length', 0))
            body = json.loads(self.rfile.read(size)) if size else {}
            if path == '/emby/System/Info/Public':
                return self.reply(200, {'Id': 'fixture', 'ServerName': 'Emby Fixture', 'Version': version, 'ProductName': 'Emby'})
            if path == '/emby/Users/Public':
                return self.reply(200, [{'Id': 'user', 'Name': USER, 'HasPassword': True}])
            if path == '/emby/Users/AuthenticateByName':
                if (body.get('Username'), body.get('Pw')) != (USER, PASSWORD):
                    return self.reply(401)
                return self.reply(200, {'AccessToken': 'fixture-token', 'User': {'Id': 'user', 'Name': USER}, 'ServerId': 'fixture'})
            if self.headers.get('X-Emby-Token') != 'fixture-token':
                return self.reply(401)
            item = {'Id': 'film', 'Name': 'Synthetic Film', 'Type': 'Movie', 'MediaType': 'Video',
                    'RunTimeTicks': 150_000_000, 'MediaSources': [source], 'UserData': {'IsFavorite': state['favorite']}}
            if path == '/emby/Users/user/Views':
                return self.reply(200, {'Items': [{'Id': 'library', 'Name': 'Movies', 'CollectionType': 'movies'}]})
            if path == '/emby/Users/user/Items':
                return self.reply(200, {'Items': [item], 'TotalRecordCount': 1})
            if path == '/emby/Users/user/Items/film':
                return self.reply(200, item)
            if path == '/emby/Users/user/FavoriteItems/film':
                state['favorite'] = self.command == 'POST'
                return self.reply(204)
            if path == '/emby/Items/film/PlaybackInfo' and body.get('EnableDirectPlay') is True:
                return self.reply(200, {'MediaSources': [source], 'PlaySessionId': 'synthetic-session'})
            if path == '/emby/Videos/film/stream':
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
            if path == '/emby/Sessions/Playing' and body.get('ItemId') == 'film':
                state['starts'] += 1
                return self.reply(204)
            if path == '/emby/Sessions/Playing/Stopped' and body.get('ItemId') == 'film':
                state['stops'] += 1
                return self.reply(204)
            if path in ('/emby/Sessions/Capabilities/Full', '/emby/Sessions/Logout'):
                return self.reply(204)
            return self.reply(404)

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        thread = Thread(target=server.serve_forever)
        thread.start()
        try:
            yield f'http://127.0.0.1:{server.server_port}', state
        finally:
            server.shutdown()
            thread.join()


def provision(origin, version):
    token = None

    def call(path, payload=None):
        headers = {'X-Emby-Authorization': 'MediaBrowser Client="Spool test", Device="Fixture", '
                   'DeviceId="spool-emby-admin", Version="1.0"', 'Content-Type': 'application/json'}
        if token:
            headers['X-Emby-Token'] = token
        body = json.dumps(payload).encode() if payload is not None else None
        with urlopen(Request(f'{origin}/emby{path}', data=body, headers=headers), timeout=8) as response:
            data = response.read()
            return json.loads(data) if data and 'json' in response.headers.get('Content-Type', '') else {}

    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        try:
            info = call('/System/Info/Public')
            if info.get('Version'):
                break
        except OSError:
            pass
        time.sleep(0.25)
    else:
        raise RuntimeError('Emby startup timeout')
    if info['Version'] != version:
        raise RuntimeError('Emby image reported a different version')

    call('/Startup/Configuration', {'UICulture': 'en-US', 'MetadataCountryCode': 'US', 'PreferredMetadataLanguage': 'en'})
    call('/Startup/User')
    call('/Startup/User', {'Name': USER, 'Password': PASSWORD})
    call('/Startup/RemoteAccess', {'EnableRemoteAccess': False, 'EnableAutomaticPortMapping': False})
    call('/Startup/Complete', {})
    auth = call('/Users/AuthenticateByName', {'Username': USER, 'Pw': PASSWORD})
    token = auth['AccessToken']
    user = auth['User']['Id']
    call('/Library/VirtualFolders?name=Movies&collectionType=movies&refreshLibrary=true', {
        'LibraryOptions': {'PathInfos': [{'Path': '/media'}], 'EnableRealtimeMonitor': False,
                           'TypeOptions': [{'Type': 'Movie', 'MetadataFetchers': [], 'ImageFetchers': []}]}})
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        page = call(f'/Users/{user}/Items?Recursive=true&IncludeItemTypes=Movie&Fields=MediaSources')
        if any(item.get('MediaSources') for item in page.get('Items', [])):
            return
        time.sleep(0.25)
    raise RuntimeError('Emby media scan timeout')


@contextmanager
def upstream_server(root, version, image_digest, engine):
    image = f'{IMAGE}@{image_digest}'
    config = root / 'config'
    config.mkdir()
    container = root.name
    podman = ['--userns=keep-id', '--user', '0:0'] if Path(engine).name == 'podman' else []
    quiet = {'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    subprocess.run([engine, 'pull', image], check=True, timeout=180, **quiet)
    try:
        subprocess.run([engine, 'run', '--detach', '--rm', '--pull=never', '--no-healthcheck', '--name', container,
                        *podman, '--publish', '127.0.0.1::8096', '--volume', f'{config}:/config',
                        '--volume', f'{root / "movies"}:/media:ro', '--env', f'UID={os.getuid()}',
                        '--env', f'GID={os.getgid()}', image], check=True, timeout=60, **quiet)
        port = subprocess.check_output([engine, 'port', container, '8096/tcp'], text=True, timeout=15).strip()
        if not re.fullmatch(r'127\.0\.0\.1:[0-9]+', port):
            raise RuntimeError('Emby container did not assign a loopback port')
        origin = f'http://{port}'
        provision(origin, version)
        yield origin, None
    finally:
        subprocess.run([engine, 'rm', '--force', container], check=False, timeout=15, **quiet)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spool-tests', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--version', default=VERSION)
    parser.add_argument('--image-digest', default=DIGEST)
    parser.add_argument('--container-engine', default='docker')
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]+(?:\.[0-9]+){3}', args.version) or not re.fullmatch(r'sha256:[0-9a-f]{64}', args.image_digest):
        parser.error('configure a four-part stable Emby version and an immutable sha256 digest')
    results = {}
    with tempfile.TemporaryDirectory(prefix='spool-emby-compat-') as data:
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
                with fixture as (origin, counts):
                    process = subprocess.run([str(args.spool_tests.resolve()), '--child', 'emby-provider-compatibility'], timeout=100,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                             env=os.environ | {'SPOOL_TEST_EMBY_ORIGIN': origin, 'SPOOL_TEST_EMBY_VERSION': args.version,
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
                results[name] = {'passed': False, 'errorType': type(error).__name__}
            print(f"Emby {name}: {'passed' if results[name]['passed'] else 'failed'}")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'version': args.version, 'imageDigest': args.image_digest,
                                      'fixtures': results}, indent=2) + '\n', encoding='utf-8')
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
