#!/usr/bin/env python3
"""Check the packaged Open Movies provider with synthetic HTTPS directory listings."""

import argparse
from contextlib import contextmanager
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
import os
from pathlib import Path
import select
import socket
from socketserver import BaseRequestHandler, ThreadingTCPServer
import ssl
import subprocess
import tempfile
from threading import Thread
import time

ARCHIVE_HOST = 'download.blender.org'
MOVIE = 'Synthetic.480p.mp4'


@contextmanager
def serve(server):
    with server:
        thread = Thread(target=server.serve_forever)
        thread.start()
        try:
            yield server.server_address[1]
        finally:
            server.shutdown()
            thread.join()


@contextmanager
def connect_proxy(archive_port, certificate, key):
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(certificate, key)

    class Proxy(BaseRequestHandler):
        def handle(self):
            raw = self.request
            raw.settimeout(8)
            with raw.makefile('rb') as request:
                line = request.readline(1024).split()
                if len(line) != 3 or line[:2] != [b'CONNECT', f'{ARCHIVE_HOST}:443'.encode()]:
                    raw.sendall(b'HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n')
                    return
                for _ in range(32):
                    header = request.readline(4096)
                    if header == b'\r\n':
                        break
                    if not header or not header.endswith(b'\r\n'):
                        return
                else:
                    return
            try:
                with socket.create_connection(('127.0.0.1', archive_port), timeout=8) as archive:
                    raw.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                    with tls.wrap_socket(raw, server_side=True) as client:
                        while True:
                            readable = [client] if client.pending() else select.select([client, archive], [], [], 8)[0]
                            if not readable:
                                return
                            for source in readable:
                                chunk = source.recv(65536)
                                if not chunk:
                                    return
                                (archive if source is client else client).sendall(chunk)
            except OSError:
                return

    server = ThreadingTCPServer(('127.0.0.1', 0), Proxy)
    server.daemon_threads = True
    with serve(server) as port:
        yield port


@contextmanager
def fake_archive(root):
    class Archive(SimpleHTTPRequestHandler):
        def __init__(self, *args):
            super().__init__(*args, directory=root)

        def log_message(self, *_args):
            pass

        def list_directory(self, path):
            rows = [f'<a href="{entry.name}">{entry.name}</a> 01-Jan-2026 00:00 {entry.stat().st_size}'
                    for entry in sorted(Path(path).iterdir())]
            listing = ('<html><pre>' + '\n'.join(rows) + '</pre></html>').encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(listing)))
            self.end_headers()
            return BytesIO(listing)

    with serve(ThreadingHTTPServer(('127.0.0.1', 0), Archive)) as port:
        yield port


@contextmanager
def nginx_archive(root):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    conf = root.parent / 'nginx.conf'
    conf.write_text(f'''pid {root.parent}/nginx.pid;
error_log stderr;
events {{}}
http {{
    access_log off;
    server {{
        listen 127.0.0.1:{port};
        root {root};
        autoindex on;
    }}
}}
''')
    with subprocess.Popen(['nginx', '-p', str(root.parent), '-c', str(conf), '-g', 'daemon off;'],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as server:
        try:
            for _ in range(40):
                if server.poll() is not None:
                    break
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=0.25):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError('directory server did not start')
            if server.poll() is not None:
                raise RuntimeError('directory server stopped before the test')
            yield port
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spool-tests', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    results = {}
    with tempfile.TemporaryDirectory(prefix='spool-open-movies-') as path:
        root = Path(path)
        certificate, key = root / 'ca.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', f'/CN={ARCHIVE_HOST}', '-addext', f'subjectAltName=DNS:{ARCHIVE_HOST}',
                        '-keyout', str(key), '-out', str(certificate)], check=True, timeout=15,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        clip = root / MOVIE
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'testsrc2=size=160x90:rate=12:duration=2', '-c:v', 'libx264',
                        '-pix_fmt', 'yuv420p', '-movflags', 'faststart', str(clip)], check=True, timeout=30)
        for name, run_archive in (('fake', fake_archive), ('nginx', nginx_archive)):
            files = root / name
            held = files / 'held'
            held.mkdir(parents=True)
            os.link(clip, held / MOVIE)
            (held / 'README.txt').write_text('Synthetic media fixture.\n')
            try:
                with run_archive(files) as archive_port, connect_proxy(archive_port, certificate, key) as proxy_port:
                    process = subprocess.run([str(args.spool_tests.resolve()), '--child', 'open-movies-provider'], timeout=75,
                                             env=os.environ | {'SPOOL_TEST_OPEN_MOVIES_PROXY': str(proxy_port),
                                                               'SPOOL_TEST_OPEN_MOVIES_CA': str(certificate),
                                                               'SPOOL_TEST_OPEN_MOVIES_ROOT': str(files)},
                                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                    for line in process.stderr.splitlines():
                        if line.startswith('spool-test-require: '):
                            print(f'{name}: {line}')
                    results[name] = {'selectorExitCode': process.returncode, 'passed': process.returncode == 0}
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                results[name] = {'passed': False, 'errorType': type(error).__name__}
            print(f"Open Movies {name}: {'passed' if results[name]['passed'] else 'failed'}")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'provider': 'open-movies', 'fixtures': results}, indent=2) + '\n', encoding='utf-8')
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
