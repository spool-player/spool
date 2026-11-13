#!/usr/bin/env python3
"""Real libmpv + nginx TLS/HTTP2/QUIC correctness; no display or external server.

Protocol correctness never skips: missing tooling or a missing H3 build fails.
The separate packet harness may skip when user network namespaces are disabled.
"""
import argparse
import array
import importlib.util
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


def free_port(host):
    with socket.socket() as listener:
        listener.bind((host, 0))
        return listener.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libmpv', required=True)
    parser.add_argument('--transport-helper', type=Path, required=True)
    parser.add_argument('--performance', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    for tool in ['nginx', 'openssl']:
        if not shutil.which(tool):
            parser.error(f'{tool} required: run in nix develop .#native')
    spec = importlib.util.spec_from_file_location('curl_transport', args.transport_helper)
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)
    rows = []
    with tempfile.TemporaryDirectory(prefix='spool-protocols-') as directory:
        root = Path(directory)
        # Nonperiodic frame counters catch duplicated or misplaced aligned ranges.
        pcm = array.array('I', range((256 if args.performance else 16) * 1024 * 1024 // 4))
        import sys
        if sys.byteorder != 'little':
            pcm.byteswap()
        expected = pcm.tobytes()
        del pcm
        payload = (b'RIFF' + struct.pack('<I', len(expected) + 36) + b'WAVEfmt ' +
                   struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16) +
                   b'data' + struct.pack('<I', len(expected)) + expected)
        (root / 'fixture.wav').write_bytes(payload)
        del payload
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem'),
                        '-days', '1', '-subj', '/CN=127.0.0.1', '-addext',
                        'subjectAltName=IP:127.0.0.1'], check=True, capture_output=True)
        tls, tcp = free_port('127.0.0.1'), free_port('127.0.0.1')
        while tls == tcp:
            tcp = free_port('127.0.0.1')
        (root / 'nginx.conf').write_text(f'''worker_processes 1;
pid {root}/nginx.pid;
error_log {root}/error.log info;
events {{ worker_connections 128; }}
http {{
  log_format protocols '$server_port $server_protocol $status $http_range';
  access_log {root}/access.log protocols;
  ssl_certificate {root}/cert.pem;
  ssl_certificate_key {root}/key.pem;
  ssl_protocols TLSv1.2 TLSv1.3;
  ssl_session_cache shared:SSL:1m;
  sendfile on;
  limit_rate 4m;
  server {{
    listen 127.0.0.1:{tls} ssl;
    listen 127.0.0.1:{tls} quic reuseport;
    http2 on;
    add_header Alt-Svc 'h3=":{tls}"';
    root {root};
  }}
  server {{
    listen 127.0.0.1:{tcp} ssl;
    http2 on;
    add_header Alt-Svc 'h3=":{tcp}"';
    root {root};
  }}
}}
''')
        nginx = subprocess.Popen(['nginx', '-p', str(root), '-c', str(root / 'nginx.conf'),
                                  '-g', 'daemon off;'], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while True:
                if nginx.poll() is not None:
                    raise AssertionError('nginx fixture failed: ' + nginx.stderr.read().decode())
                try:
                    with socket.create_connection(('127.0.0.1', tls), timeout=.1):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise AssertionError('nginx fixture not ready')
                    time.sleep(.025)
            counts = [1, 4, 0] if args.performance else [4]
            for mode, version, port in [('http1', 11, tls), ('tcp', 20, tls),
                                         ('prefer-h3', 30, tls), ('prefer-h3', 20, tcp)]:
                for count in counts:
                    output = root / 'actual.pcm'
                    options = {'tls-ca-file': root / 'cert.pem', 'ao': 'pcm',
                               'ao-pcm-file': output, 'ao-pcm-waveheader': 'no',
                               'audio-format': 's16', 'curl-http-mode': mode,
                               'curl-fixed-requests': str(count),
                               'curl-receive-mode': 'fixed', 'curl-receive-bytes': str(4 << 20)}
                    player = transport.Player(args.libmpv, **options)
                    try:
                        capabilities = player.get('curl-transport-capabilities')
                        if not capabilities or not capabilities.get('http2') or not capabilities.get('http3'):
                            raise AssertionError('production libmpv must advertise HTTP2 and HTTP3')
                        player.configure(1, max_requests=4, fixed_requests=count,
                                         http_mode={'http1': 2, 'tcp': 1, 'prefer-h3': 3}[mode],
                                         receive_mode=2, receive_bytes=4 << 20)
                        player.check(player.transport_control({'format': 2, 'op': 1, 'playback-epoch': 1,
                            'joint-extra-grant-bytes': 64 << 20, 'background-network': False}), 'grant')
                        result = player.play(f'https://127.0.0.1:{port}/fixture.wav', timeout=180)
                    finally:
                        player.close()
                    if output.read_bytes() != expected:
                        raise AssertionError(f'{mode}: decoded PCM differs from independently generated frames')
                    snapshots = [sample for _, sample in result['samples']]
                    if result['metrics']:
                        snapshots.append(result['metrics'])
                    protocols = {row['http-version'] for sample in snapshots for row in sample.get('connections', [])}
                    protocols.update(row.get('effective-http-version', 0) for sample in snapshots
                                     for row in sample.get('streams', []))
                    if version not in protocols:
                        raise AssertionError(f'{mode}: expected negotiated {version}, observed {sorted(protocols)}')
                    # The independent server log confirms ALPN/QUIC rather than
                    # trusting a request preference reported as effective.
                    server_protocol = {11: 'HTTP/1.1', 20: 'HTTP/2.0', 30: 'HTTP/3.0'}[version]
                    time.sleep(.05)
                    log = (root / 'access.log').read_text()
                    if f'{port} {server_protocol} 206 bytes=' not in log:
                        raise AssertionError(f'{mode}: nginx did not serve ranges over {server_protocol}')
                    rows.append({'mode': mode, 'fixed-requests': count, 'effective-http-version': version,
                                 'seconds': result['seconds'], 'first-progress-seconds': result['first-progress-seconds'],
                                 'pcm-bytes': len(expected), 'bytes-per-second': len(expected) / result['seconds']})
                    print(json.dumps(rows[-1]), flush=True)
            if not args.performance:
                from reset_origin import ResetOrigin
                header = (b'RIFF' + struct.pack('<I', len(expected) + 36) + b'WAVEfmt ' +
                          struct.pack('<IHHIIHH', 16, 1, 2, 48000, 192000, 4, 16) +
                          b'data' + struct.pack('<I', len(expected)))
                with ResetOrigin(root, header + expected) as reset:
                    output = root / 'reset.pcm'
                    player = transport.Player(args.libmpv, **{
                        'tls-ca-file': root / 'cert.pem', 'ao': 'pcm', 'ao-pcm-file': output,
                        'ao-pcm-waveheader': 'no', 'audio-format': 's16',
                        'curl-http-mode': 'tcp', 'curl-fixed-requests': '1', 'curl-max-retries': '0'})
                    try:
                        player.configure(1, max_requests=4, fixed_requests=1, http_mode=1)
                        player.check(player.transport_control({'format': 2, 'op': 1, 'playback-epoch': 1,
                            'joint-extra-grant-bytes': 64 << 20, 'background-network': False}), 'grant')
                        result = player.play(f'https://127.0.0.1:{reset.port}/fixture.wav', timeout=30)
                    finally:
                        player.close()
                    if output.read_bytes() != expected:
                        raise AssertionError('HTTP2 reset downgrade changed decoded PCM')
                    if not {'h2', 'http/1.1'}.issubset(reset.protocols):
                        raise AssertionError('HTTP2 reset did not cause a fresh HTTP1 connection')
                    if not any(start > 0 for start, _ in reset.ranges):
                        raise AssertionError('HTTP2 reset did not retry an unfinished suffix')
                    fallback_count = max((row.get('http-fallbacks', 0) for _, sample in result['samples']
                                          for row in sample.get('streams', [])), default=0)
                    if fallback_count < 1:
                        raise AssertionError('HTTP2 reset downgrade was not reported')
                    print(json.dumps({'scenario': 'h2-reset-with-zero-retry-budget',
                                      'http-fallbacks': fallback_count, 'pcm-bytes': len(expected)}), flush=True)
        finally:
            nginx.terminate()
            try:
                nginx.wait(timeout=5)
            except subprocess.TimeoutExpired:
                nginx.kill()
                nginx.wait()
    if args.output:
        args.output.write_text(json.dumps(rows, indent=2) + '\n')


if __name__ == '__main__':
    main()
