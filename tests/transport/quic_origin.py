"""HTTP3 origin closing a QUIC connection after partial valid media bytes.

TCP on the same port belongs to nginx and serves the identical representation.
The production client must retry over TCP, with its TLS validation intact.
"""
import asyncio
import re
import threading


class QuicFailureOrigin:
    def __init__(self, root, port, payload, etag):
        self.root, self.port, self.payload, self.etag = root, port, payload, etag
        self.requests = []
        self.ready = threading.Event()
        self.error = None
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def __enter__(self):
        import aioquic  # Fail in the caller, not an unobserved worker thread.
        self.thread.start()
        if not self.ready.wait(timeout=5):
            raise AssertionError('QUIC fixture did not become ready')
        if self.error:
            raise self.error
        return self

    def __exit__(self, *_):
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=5)

    def run(self):
        from aioquic.asyncio import serve
        from aioquic.asyncio.protocol import QuicConnectionProtocol
        from aioquic.h3.connection import H3_ALPN, H3Connection
        from aioquic.h3.events import HeadersReceived
        from aioquic.quic.configuration import QuicConfiguration
        from aioquic.quic.events import ProtocolNegotiated
        origin = self

        class Protocol(QuicConnectionProtocol):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, **kwargs)
                self.http = None
                self.failed = False

            def quic_event_received(self, event):
                if isinstance(event, ProtocolNegotiated):
                    self.http = H3Connection(self._quic)
                if self.http is None:
                    return
                for request in self.http.handle_event(event):
                    if not isinstance(request, HeadersReceived) or self.failed:
                        continue
                    values = dict(request.headers)
                    match = re.fullmatch(rb'bytes=(\d+)-(\d*)', values.get(b'range', b''))
                    start = int(match[1]) if match else 0
                    end = min(int(match[2]) + 1 if match and match[2] else len(origin.payload), len(origin.payload))
                    origin.requests.append((start, end))
                    self.http.send_headers(request.stream_id, [
                        (b':status', b'206'), (b'content-type', b'audio/wav'),
                        (b'etag', origin.etag.encode()), (b'accept-ranges', b'bytes'),
                        (b'content-length', str(end - start).encode()),
                        (b'content-range', f'bytes {start}-{end - 1}/{len(origin.payload)}'.encode())])
                    self.http.send_data(request.stream_id, origin.payload[start:min(start + 16384, end)], end_stream=False)
                    self.transmit()
                    self.failed = True
                    # A real HTTP3 connection error, not an HTTP error status.
                    origin.loop.call_later(.05, lambda: self.close(error_code=0x102,
                                                                  reason_phrase='fixture connection failure'))

        async def start():
            configuration = QuicConfiguration(is_client=False, alpn_protocols=H3_ALPN)
            configuration.load_cert_chain(self.root / 'cert.pem', self.root / 'key.pem')
            return await serve('127.0.0.1', self.port, configuration=configuration, create_protocol=Protocol)

        asyncio.set_event_loop(self.loop)
        server = None
        try:
            server = self.loop.run_until_complete(start())
            self.ready.set()
            self.loop.run_forever()
        except Exception as error:
            self.error = error
            self.ready.set()
        finally:
            if server:
                server.close()
                self.loop.run_until_complete(asyncio.sleep(.1))
            self.loop.close()
