"""TLS origin that resets HTTP2 streams after valid representation bytes.

The HTTP1 path serves the identical strong-ETag representation. A successful
client must downgrade and retry the unread suffix, preserving exact PCM.
"""
import re
import socket
import ssl
import threading
import time


class ResetOrigin:
    def __init__(self, root, payload):
        self.payload = payload
        self.protocols = []
        self.ranges = []
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(root / 'cert.pem', root / 'key.pem')
        self.context.set_alpn_protocols(['h2', 'http/1.1'])
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(16)
        self.listener.settimeout(.1)
        self.worker = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.worker.start()
        return self

    def __exit__(self, *_):
        self.stop.set()
        self.worker.join(timeout=2)
        self.listener.close()

    def serve(self):
        while not self.stop.is_set():
            try:
                connection, _ = self.listener.accept()
            except socket.timeout:
                continue
            threading.Thread(target=self.handle, args=(connection,), daemon=True).start()

    def headers(self, values):
        match = re.fullmatch(r'bytes=(\d+)-(\d*)', values.get('range', ''))
        start = int(match[1]) if match else 0
        end = min(int(match[2]) + 1 if match and match[2] else len(self.payload), len(self.payload))
        with self.lock:
            self.ranges.append((start, end))
        return start, end, [('content-type', 'audio/wav'), ('etag', '"reset-fixture-v1"'),
                            ('accept-ranges', 'bytes'), ('content-length', str(end - start)),
                            ('content-range', f'bytes {start}-{end - 1}/{len(self.payload)}')]

    def handle(self, raw):
        try:
            with self.context.wrap_socket(raw, server_side=True) as connection:
                connection.settimeout(5)
                protocol = connection.selected_alpn_protocol() or 'http/1.1'
                with self.lock:
                    self.protocols.append(protocol)
                if protocol == 'h2':
                    self.http2(connection)
                else:
                    self.http1(connection)
        except (OSError, ssl.SSLError):
            raw.close()

    def http2(self, connection):
        import h2.config
        import h2.connection
        import h2.events
        h2conn = h2.connection.H2Connection(config=h2.config.H2Configuration(client_side=False, header_encoding='utf-8'))
        h2conn.initiate_connection()
        connection.sendall(h2conn.data_to_send())
        while not self.stop.is_set():
            data = connection.recv(65536)
            if not data:
                return
            for event in h2conn.receive_data(data):
                if isinstance(event, h2.events.RequestReceived):
                    start, end, headers = self.headers(dict(event.headers))
                    h2conn.send_headers(event.stream_id, [(':status', '206'), *headers])
                    # Real partial delivery, then a protocol-level reset. This
                    # cannot be mistaken for an HTTP status or empty body.
                    h2conn.send_data(event.stream_id, self.payload[start:min(start + 16384, end)])
                    connection.sendall(h2conn.data_to_send())
                    # Give curl a read turn with body bytes before the reset;
                    # otherwise one read can see only an already-reset stream.
                    time.sleep(.05)
                    h2conn.reset_stream(event.stream_id, error_code=2)
            connection.sendall(h2conn.data_to_send())

    def http1(self, connection):
        stream = connection.makefile('rb')
        while not self.stop.is_set():
            request = stream.readline()
            if not request:
                return
            values = {}
            while True:
                line = stream.readline()
                if line in (b'\r\n', b'\n', b''):
                    break
                key, value = line.decode().split(':', 1)
                values[key.lower()] = value.strip()
            start, end, headers = self.headers(values)
            connection.sendall(('HTTP/1.1 206 Partial Content\r\n' +
                                ''.join(f'{k}: {v}\r\n' for k, v in headers) + '\r\n').encode())
            for offset in range(start, end, 65536):
                connection.sendall(self.payload[offset:min(offset + 65536, end)])
                time.sleep(.002)
