#!/usr/bin/env python3
"""Real libmpv + nginx TLS/HTTP2/QUIC correctness; no display or external server.

Protocol correctness never skips: missing tooling or a missing H3 build fails.
The separate packet harness may skip when user network namespaces are disabled.
"""
import argparse
import array
import importlib.util
import http.client
import hashlib
import ssl
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time


def free_port(host):
    # nginx/QUIC and silent-UDP fixtures need the same number free for both
    # transports; a free TCP port says nothing about an existing UDP listener.
    for _ in range(64):
        with socket.socket() as listener, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            listener.bind((host, 0))
            port = listener.getsockname()[1]
            try:
                udp.bind((host, port))
            except OSError:
                continue
            return port
    raise RuntimeError("could not reserve a TCP/UDP fixture port pair")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--libmpv', required=True)
    parser.add_argument('--transport-helper', type=Path, required=True)
    parser.add_argument('--performance', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--app-revision')
    parser.add_argument('--mpv-revision')
    parser.add_argument('--harness-revision')
    args = parser.parse_args()
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
    for tool in ['nginx', 'openssl']:
        if not shutil.which(tool):
            parser.error(f'{tool} required: run in nix develop .#native')
    spec = importlib.util.spec_from_file_location('curl_transport', args.transport_helper)
    transport = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(transport)
    rows = []
    with open(args.libmpv, 'rb') as library:
        library_sha256 = hashlib.file_digest(library, 'sha256').hexdigest()

    def record(row):
        rows.append(row)
        print(json.dumps(row), flush=True)
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
        broken_quic = free_port('127.0.0.1')
        while tls == tcp:
            tcp = free_port('127.0.0.1')
        while broken_quic in (tls, tcp):
            broken_quic = free_port('127.0.0.1')
        (root / 'nginx.conf').write_text(f'''user root root;
worker_processes 1;
pid {root}/nginx.pid;
error_log {root}/error.log info;
events {{ worker_connections 128; }}
http {{
  client_body_temp_path {root}/client_body;
  proxy_temp_path {root}/proxy;
  fastcgi_temp_path {root}/fastcgi;
  uwsgi_temp_path {root}/uwsgi;
  scgi_temp_path {root}/scgi;
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
    listen 127.0.0.1:{broken_quic} ssl;
    http2 on;
    add_header Alt-Svc 'h3=":{broken_quic}"';
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
        nginx = subprocess.Popen(['nginx', '-e', 'stderr', '-p', str(root), '-c', str(root / 'nginx.conf'),
                                  '-g', 'daemon off;'], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + (20 if args.performance else 5)
            while True:
                if nginx.poll() is not None:
                    raise AssertionError('nginx fixture failed: ' + nginx.stderr.read().decode())
                try:
                    with socket.create_connection(('127.0.0.1', tls), timeout=1 if args.performance else .1):
                        break
                except OSError:
                    if time.monotonic() > deadline:
                        raise AssertionError('nginx fixture not ready')
                    time.sleep(.025)
            counts = [1, 4, 0] if args.performance else [4]
            for mode, version, port in [('http1', 11, tls), ('tcp', 20, tls),
                                         ('prefer-h3', 30, tls), ('prefer-h3', 20, tcp)]:
                for count in counts:
                    log_start = (root / 'access.log').stat().st_size if (root / 'access.log').exists() else 0
                    output = root / 'actual.pcm'
                    options = {'tls-ca-file': root / 'cert.pem', 'ao': 'pcm',
                               'ao-pcm-file': output, 'ao-pcm-waveheader': 'no',
                               'audio-format': 's16', 'curl-http-mode': mode,
                               'curl-fixed-requests': str(count),
                               'curl-receive-mode': 'fixed', 'curl-receive-bytes': str(4 << 20)}
                    if args.performance:
                        # Correctness uses short ranges to exercise many joins.
                        # Performance must permit BDP-sized adaptive ranges.
                        options['curl-max-request-size'] = str(16 << 20)
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
                    log = (root / 'access.log').read_bytes()[log_start:].decode()
                    if f'{port} {server_protocol} 206 bytes=' not in log:
                        raise AssertionError(f'{mode}: nginx did not serve ranges over {server_protocol}')
                    record({'mode': mode, 'fixed-requests': count, 'effective-http-version': version,
                                 'seconds': result['seconds'], 'first-progress-seconds': result['first-progress-seconds'],
                                 'pcm-bytes': len(expected), 'bytes-per-second': len(expected) / result['seconds']})
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
                    h2_connections = reset.protocols.count('h2')
                    second = transport.Player(args.libmpv, **{'tls-ca-file': root / 'cert.pem',
                        'curl-http-mode': 'tcp', 'curl-fixed-requests': '1'})
                    try:
                        second.play(f'https://127.0.0.1:{reset.port}/fixture.wav', timeout=30)
                    finally:
                        second.close()
                    if reset.protocols.count('h2') != h2_connections:
                        raise AssertionError('new playback core retried cooled-down failed HTTP2 origin')
                    record({'scenario': 'h2-reset-with-zero-retry-budget',
                                      'http-fallbacks': fallback_count, 'cooldown-reused': True,
                                      'pcm-bytes': len(expected)})
                from quic_origin import QuicFailureOrigin
                # Both protocol paths identify the same representation. Obtain the
                # fixture's real validator before serving it over a second stack.
                metadata = http.client.HTTPSConnection('127.0.0.1', broken_quic,
                    context=ssl.create_default_context(cafile=root / 'cert.pem'))
                try:
                    metadata.request('HEAD', '/fixture.wav')
                    response = metadata.getresponse()
                    etag = response.getheader('ETag')
                    if response.status != 200 or not etag:
                        raise AssertionError('TCP fixture did not supply a strong representation validator')
                    response.read()
                finally:
                    metadata.close()
                with QuicFailureOrigin(root, broken_quic, header + expected, etag) as broken:
                    output = root / 'quic-reset.pcm'
                    player = transport.Player(args.libmpv, **{
                        'tls-ca-file': root / 'cert.pem', 'ao': 'pcm', 'ao-pcm-file': output,
                        'ao-pcm-waveheader': 'no', 'audio-format': 's16',
                        'curl-http-mode': 'prefer-h3', 'curl-fixed-requests': '1', 'curl-max-retries': '0'})
                    try:
                        player.configure(1, max_requests=4, fixed_requests=1, http_mode=3)
                        player.check(player.transport_control({'format': 2, 'op': 1, 'playback-epoch': 1,
                            'joint-extra-grant-bytes': 64 << 20, 'background-network': False}), 'grant')
                        result = player.play(f'https://127.0.0.1:{broken_quic}/fixture.wav', timeout=30)
                        # Reuse the process-wide origin cooldown in a new core.
                        # This first core must finish before creating its successor.
                    finally:
                        player.close()
                    actual = output.read_bytes()
                    if actual != expected or not broken.requests:
                        first_difference = next((index for index, (left, right) in enumerate(zip(actual, expected))
                                                 if left != right), min(len(actual), len(expected)))
                        metric_keys = ['effective-http-version', 'http-fallbacks', 'last-reason',
                                       'state', 'retained-bytes', 'readable-bytes']
                        record({'scenario': 'h3-failure-diagnostic', 'actual-bytes': len(actual),
                            'expected-bytes': len(expected), 'first-difference': first_difference,
                            'actual-sha256': hashlib.sha256(actual).hexdigest(),
                            'expected-sha256': hashlib.sha256(expected).hexdigest(),
                            'quic-ranges': broken.requests,
                            'stream-samples': [{key: row.get(key) for key in metric_keys}
                                for _, sample in result['samples'] for row in sample.get('streams', [])][-64:],
                            'fixture-requests': [line for line in (root / 'access.log').read_text().splitlines()
                                                 if line.startswith(f'{broken_quic} ')][-64:]})
                        artifact = args.output.parent / 'quic-reset-failure.pcm' if args.output else None
                        if artifact:
                            artifact.write_bytes(actual)
                        if not broken.requests:
                            raise AssertionError('HTTP3 failure fixture was never requested over QUIC')
                        raise AssertionError('HTTP3 connection failure did not preserve exact PCM')
                    fallback_count = max((row.get('http-fallbacks', 0) for _, sample in result['samples']
                                          for row in sample.get('streams', [])), default=0)
                    if fallback_count < 1:
                        raise AssertionError('HTTP3 connection failure was not reported as a downgrade')
                    if f'{broken_quic} HTTP/2.0 206 bytes=' not in (root / 'access.log').read_text():
                        raise AssertionError('HTTP3 connection failure did not retry over HTTP2')
                    before = len(broken.requests)
                    second = transport.Player(args.libmpv, **{'tls-ca-file': root / 'cert.pem',
                        'curl-http-mode': 'prefer-h3', 'curl-fixed-requests': '1'})
                    try:
                        second.play(f'https://127.0.0.1:{broken_quic}/fixture.wav', timeout=30)
                    finally:
                        second.close()
                    if len(broken.requests) != before:
                        raise AssertionError('new playback core retried cooled-down failed HTTP3 origin')
                    record({'scenario': 'h3-connection-failure-with-zero-retry-budget',
                                      'http-fallbacks': fallback_count, 'cooldown-reused': True,
                                      'pcm-bytes': len(expected)})
                # Verify curl-http-mode=auto escapes a window-capped high-RTT
                # HTTPS HTTP/2 single-socket bottleneck onto multiple HTTP/1.1
                # connections while preserving bit-exact decoded PCM.
                shaper_port = free_port('127.0.0.1')
                while shaper_port in (tls, tcp, broken_quic):
                    shaper_port = free_port('127.0.0.1')
                shaper_script = Path(__file__).resolve().parents[2] / 'tools' / 'network-shaper.py'
                shaper = subprocess.Popen(
                    ['python3', str(shaper_script),
                     '--listen', f'127.0.0.1:{shaper_port}',
                     '--upstream', f'127.0.0.1:{tcp}',
                     '--rtt-ms', '140', '--max-window-bytes', str(256 * 1024),
                     '--initial-window-bytes', str(10 * 1460)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
                try:
                    shaper_deadline = time.monotonic() + 5
                    while True:
                        if shaper.poll() is not None:
                            raise AssertionError('network-shaper failed: ' + shaper.stderr.read().decode())
                        try:
                            with socket.create_connection(('127.0.0.1', shaper_port), timeout=.1):
                                break
                        except OSError:
                            if time.monotonic() > shaper_deadline:
                                raise AssertionError('network-shaper not ready')
                            time.sleep(.025)
                    output = root / 'auto-h1-escape.pcm'
                    player = transport.Player(args.libmpv, **{
                        'tls-ca-file': root / 'cert.pem', 'ao': 'pcm', 'ao-pcm-file': output,
                        'ao-pcm-waveheader': 'no', 'audio-format': 's16',
                        'curl-http-mode': 'auto', 'curl-fixed-requests': '0',
                        'curl-max-request-size': str(2 << 20),
                        'curl-receive-mode': 'fixed', 'curl-receive-bytes': str(4 << 20)})
                    try:
                        player.configure(1, max_requests=4, fixed_requests=0, http_mode=0,
                                         receive_mode=2, receive_bytes=4 << 20,
                                         direct_file=True, source_size_bytes=len(header) + len(expected),
                                         source_duration_seconds=8, source_bitrate=16000000)
                        player.check(player.transport_control({'format': 2, 'op': 1, 'playback-epoch': 1,
                            'joint-extra-grant-bytes': 64 << 20, 'background-network': False}), 'grant')
                        result = player.play(f'https://127.0.0.1:{shaper_port}/fixture.wav', timeout=60)
                    finally:
                        player.close()
                    if output.read_bytes() != expected:
                        raise AssertionError('auto HTTPS multi-socket HTTP/1.1 escape altered decoded PCM')
                    snapshots = [sample for _, sample in result['samples']]
                    if result['metrics']:
                        snapshots.append(result['metrics'])
                    auto_protocols = {row.get('effective-http-version', 0) for sample in snapshots
                                      for row in sample.get('streams', [])}
                    auto_protocols.update(row.get('http-version', 0) for sample in snapshots
                                          for row in sample.get('connections', []))
                    peak_physical = max((sample.get('physical-connections', 0) for sample in snapshots), default=0)
                    if 11 not in auto_protocols or 30 in auto_protocols or peak_physical < 2:
                        raise AssertionError(
                            f'auto HTTPS did not escape to multi-socket HTTP/1.1: '
                            f'protocols={sorted(auto_protocols)} peak_physical={peak_physical}')
                    record({'scenario': 'auto-https-multisocket-h1-escape',
                                      'protocols': sorted(auto_protocols),
                                      'peak-physical-connections': peak_physical,
                                      'seconds': round(result['seconds'], 3),
                                      'pcm-bytes': len(expected)})
                finally:
                    shaper.terminate()
                    try:
                        shaper.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        shaper.kill()
                        shaper.wait()
        finally:
            nginx.terminate()
            try:
                nginx.wait(timeout=5)
            except subprocess.TimeoutExpired:
                nginx.kill()
                nginx.wait()
    if args.output:
        args.output.write_text(json.dumps({'format': 1, 'identity': {'libmpv_sha256': library_sha256,
            'app_revision': args.app_revision, 'mpv_revision': args.mpv_revision,
            'harness_revision': args.harness_revision}, 'performance': args.performance, 'scenarios': rows}, indent=2) + '\n')


if __name__ == '__main__':
    main()
