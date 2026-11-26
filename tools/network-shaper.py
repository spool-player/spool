#!/usr/bin/env python3
"""Shape a TCP path in userspace for deterministic playback testing."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import json
import random
import signal
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Endpoint:
    host: str
    port: int


def parse_endpoint(value: str) -> Endpoint:
    host, separator, port_text = value.rpartition(":")
    host = host.removeprefix("[").removesuffix("]")
    if not separator or not host:
        raise argparse.ArgumentTypeError("expected HOST:PORT")
    try:
        port = int(port_text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("port must be an integer") from error
    if not 1 <= port <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return Endpoint(host, port)


class Metrics:
    def __init__(self) -> None:
        self.started_at = time.monotonic()
        self.active_connections = 0
        self.peak_active_connections = 0
        self.total_connections = 0
        self.upload_bytes = 0
        self.download_bytes = 0
        self.injected_stalls = 0

    def snapshot(self, previous: tuple[float, int, int]) -> tuple[dict[str, float | int], tuple[float, int, int]]:
        now = time.monotonic()
        previous_at, previous_upload, previous_download = previous
        elapsed = max(now - previous_at, 1e-9)
        snapshot: dict[str, float | int] = {
            "active_connections": self.active_connections,
            "peak_active_connections": self.peak_active_connections,
            "total_connections": self.total_connections,
            "upload_bytes": self.upload_bytes,
            "download_bytes": self.download_bytes,
            "upload_bps": round((self.upload_bytes - previous_upload) * 8 / elapsed),
            "download_bps": round((self.download_bytes - previous_download) * 8 / elapsed),
            "injected_stalls": self.injected_stalls,
            "uptime_seconds": round(now - self.started_at, 3),
        }
        return snapshot, (now, self.upload_bytes, self.download_bytes)

    def summary(self) -> dict[str, float | int]:
        now = time.monotonic()
        elapsed = max(now - self.started_at, 1e-9)
        return {
            "active_connections": self.active_connections,
            "peak_active_connections": self.peak_active_connections,
            "total_connections": self.total_connections,
            "upload_bytes": self.upload_bytes,
            "download_bytes": self.download_bytes,
            "average_download_bps": round(self.download_bytes * 8 / elapsed),
            "average_upload_bps": round(self.upload_bytes * 8 / elapsed),
            "injected_stalls": self.injected_stalls,
            "uptime_seconds": round(elapsed, 3),
        }


class Pacer:
    def __init__(self, bytes_per_second: float) -> None:
        self._bytes_per_second = bytes_per_second
        self._next_send_at = 0.0

    async def wait(self, byte_count: int) -> None:
        if self._bytes_per_second <= 0:
            return
        loop = asyncio.get_running_loop()
        now = loop.time()
        send_at = max(now, self._next_send_at)
        self._next_send_at = send_at + byte_count / self._bytes_per_second
        if send_at > now:
            await asyncio.sleep(send_at - now)


class SlowStartWindow:
    """Models per-connection TCP IW slow-start growth and receive-window ceiling.

    Within each RTT window a connection may transmit at most `cwnd` bytes.
    When an RTT window is exhausted, the sender waits for the remaining RTT
    duration (modeling ACK arrival) and doubles `cwnd` up to `max_window_bytes`.
    """

    def __init__(
        self,
        *,
        rtt_seconds: float,
        initial_window_bytes: int,
        max_window_bytes: int,
    ) -> None:
        self._rtt_seconds = rtt_seconds
        self._max_window_bytes = max_window_bytes
        if initial_window_bytes > 0:
            self._cwnd_bytes = initial_window_bytes
            if max_window_bytes > 0:
                self._cwnd_bytes = min(self._cwnd_bytes, max_window_bytes)
        elif max_window_bytes > 0:
            self._cwnd_bytes = max_window_bytes
        else:
            self._cwnd_bytes = 0
        self._window_started_at = 0.0
        self._sent_in_window = 0

    @property
    def enabled(self) -> bool:
        return self._cwnd_bytes > 0 and self._rtt_seconds > 0

    @property
    def cwnd_bytes(self) -> int:
        return self._cwnd_bytes

    async def wait(self, byte_count: int) -> None:
        if not self.enabled or byte_count <= 0:
            return
        loop = asyncio.get_running_loop()
        remaining = byte_count
        while remaining > 0:
            now = loop.time()
            if self._window_started_at <= 0.0:
                self._window_started_at = now
                self._sent_in_window = 0
            elif now - self._window_started_at >= self._rtt_seconds:
                # Idle or completed RTT: advance window; if bytes were sent in
                # the previous RTT, grow cwnd exponentially like TCP slow-start.
                if self._sent_in_window > 0:
                    self._grow_cwnd()
                self._window_started_at = now
                self._sent_in_window = 0

            available = self._cwnd_bytes - self._sent_in_window
            if available <= 0:
                delay = (self._window_started_at + self._rtt_seconds) - now
                if delay > 0:
                    await asyncio.sleep(delay)
                self._grow_cwnd()
                self._window_started_at = loop.time()
                self._sent_in_window = 0
                available = self._cwnd_bytes

            take = min(remaining, available)
            self._sent_in_window += take
            remaining -= take

    def _grow_cwnd(self) -> None:
        grown = self._cwnd_bytes * 2
        if self._max_window_bytes > 0:
            grown = min(grown, self._max_window_bytes)
        self._cwnd_bytes = max(self._cwnd_bytes, grown)


async def relay(
    reader: asyncio.StreamReader,
    writer: asyncio.StreamWriter,
    *,
    latency_seconds: float,
    jitter_seconds: float = 0.0,
    rng: random.Random | None = None,
    pacer: Pacer,
    connection_pacer: Pacer | None = None,
    window: SlowStartWindow | None = None,
    stall_after_bytes: int = 0,
    stall_seconds: float = 0.0,
    on_stall=None,
    chunk_bytes: int,
    on_bytes,
) -> None:
    queue: asyncio.Queue[tuple[float, bytes] | None] = asyncio.Queue(maxsize=64)
    loop = asyncio.get_running_loop()
    last_ready_at = 0.0

    async def receive() -> None:
        nonlocal last_ready_at
        while data := await reader.read(chunk_bytes):
            delay = latency_seconds
            if jitter_seconds > 0 and rng is not None:
                delay = max(0.0, delay + rng.uniform(-jitter_seconds, jitter_seconds))
            # Preserve TCP in-order delivery even with jitter.
            ready_at = max(last_ready_at, loop.time() + delay)
            last_ready_at = ready_at
            await queue.put((ready_at, data))
        await queue.put(None)

    async def transmit() -> None:
        transferred = 0
        stalled = False
        while queued := await queue.get():
            ready_at, data = queued
            delay = ready_at - loop.time()
            if delay > 0:
                await asyncio.sleep(delay)
            if (
                not stalled
                and stall_after_bytes > 0
                and stall_seconds > 0
                and transferred >= stall_after_bytes
            ):
                stalled = True
                if on_stall is not None:
                    on_stall()
                await asyncio.sleep(stall_seconds)
            if window is not None:
                await window.wait(len(data))
            if connection_pacer is not None:
                await connection_pacer.wait(len(data))
            await pacer.wait(len(data))
            writer.write(data)
            await writer.drain()
            transferred += len(data)
            on_bytes(len(data))
        if writer.can_write_eof():
            writer.write_eof()
            await writer.drain()

    async with asyncio.TaskGroup() as tasks:
        tasks.create_task(receive())
        tasks.create_task(transmit())


class Shaper:
    def __init__(
        self,
        upstream: Endpoint,
        *,
        latency_seconds: float,
        upload_bytes_per_second: float,
        download_bytes_per_second: float,
        chunk_bytes: int,
        per_connection_download_bytes_per_second: float = 0.0,
        initial_window_bytes: int = 0,
        max_window_bytes: int = 0,
        jitter_seconds: float = 0.0,
        jitter_seed: int = 1,
        stall_connection: int = 0,
        stall_after_bytes: int = 0,
        stall_seconds: float = 0.0,
    ) -> None:
        self.upstream = upstream
        self.latency_seconds = latency_seconds
        self.jitter_seconds = jitter_seconds
        self.rng = random.Random(jitter_seed)
        self.upload_pacer = Pacer(upload_bytes_per_second)
        self.download_pacer = Pacer(download_bytes_per_second)
        self.per_connection_download_bytes_per_second = per_connection_download_bytes_per_second
        self.initial_window_bytes = initial_window_bytes
        self.max_window_bytes = max_window_bytes
        self.stall_connection = stall_connection
        self.stall_after_bytes = stall_after_bytes
        self.stall_seconds = stall_seconds
        self.chunk_bytes = chunk_bytes
        self.metrics = Metrics()
        self.connections: set[asyncio.Task[None]] = set()

    async def handle(self, client_reader: asyncio.StreamReader, client_writer: asyncio.StreamWriter) -> None:
        task = asyncio.current_task()
        if task is not None:
            self.connections.add(task)
        self.metrics.active_connections += 1
        self.metrics.total_connections += 1
        connection_index = self.metrics.total_connections
        if self.metrics.active_connections > self.metrics.peak_active_connections:
            self.metrics.peak_active_connections = self.metrics.active_connections
        connection_download_pacer = (
            Pacer(self.per_connection_download_bytes_per_second)
            if self.per_connection_download_bytes_per_second > 0
            else None
        )
        window = (
            SlowStartWindow(
                rtt_seconds=self.latency_seconds * 2.0,
                initial_window_bytes=self.initial_window_bytes,
                max_window_bytes=self.max_window_bytes,
            )
            if (self.initial_window_bytes > 0 or self.max_window_bytes > 0)
            else None
        )
        should_stall = self.stall_connection > 0 and connection_index == self.stall_connection
        try:
            upstream_reader, upstream_writer = await asyncio.open_connection(
                self.upstream.host, self.upstream.port
            )
            try:
                async with asyncio.TaskGroup() as relays:
                    relays.create_task(
                        relay(
                            client_reader,
                            upstream_writer,
                            latency_seconds=self.latency_seconds,
                            jitter_seconds=self.jitter_seconds,
                            rng=self.rng,
                            pacer=self.upload_pacer,
                            chunk_bytes=self.chunk_bytes,
                            on_bytes=self._record_upload,
                        )
                    )
                    relays.create_task(
                        relay(
                            upstream_reader,
                            client_writer,
                            latency_seconds=self.latency_seconds,
                            jitter_seconds=self.jitter_seconds,
                            rng=self.rng,
                            pacer=self.download_pacer,
                            connection_pacer=connection_download_pacer,
                            window=window,
                            stall_after_bytes=self.stall_after_bytes if should_stall else 0,
                            stall_seconds=self.stall_seconds if should_stall else 0.0,
                            on_stall=self._record_stall,
                            chunk_bytes=self.chunk_bytes,
                            on_bytes=self._record_download,
                        )
                    )
            finally:
                upstream_writer.close()
                await upstream_writer.wait_closed()
        except* (ConnectionError, OSError) as errors:
            print(json.dumps({"event": "connection_error", "error": str(errors)}), flush=True)
        finally:
            client_writer.close()
            with contextlib.suppress(ConnectionError, OSError):
                await client_writer.wait_closed()
            self.metrics.active_connections -= 1
            if task is not None:
                self.connections.discard(task)

    def _record_upload(self, byte_count: int) -> None:
        self.metrics.upload_bytes += byte_count

    def _record_download(self, byte_count: int) -> None:
        self.metrics.download_bytes += byte_count

    def _record_stall(self) -> None:
        self.metrics.injected_stalls += 1


async def report_metrics(metrics: Metrics, interval: float) -> None:
    previous = (time.monotonic(), 0, 0)
    while True:
        await asyncio.sleep(interval)
        snapshot, previous = metrics.snapshot(previous)
        print(json.dumps({"event": "metrics", **snapshot}, sort_keys=True), flush=True)


async def run(args: argparse.Namespace) -> None:
    one_way_latency = args.rtt_ms / 2000
    one_way_jitter = args.jitter_ms / 2000
    shaper = Shaper(
        args.upstream,
        latency_seconds=one_way_latency,
        upload_bytes_per_second=args.upload_mbps * 1_000_000 / 8,
        download_bytes_per_second=args.download_mbps * 1_000_000 / 8,
        per_connection_download_bytes_per_second=args.per_connection_mbps * 1_000_000 / 8,
        initial_window_bytes=args.initial_window_bytes,
        max_window_bytes=args.max_window_bytes,
        jitter_seconds=one_way_jitter,
        jitter_seed=args.jitter_seed,
        stall_connection=args.stall_connection,
        stall_after_bytes=args.stall_after_bytes,
        stall_seconds=args.stall_seconds,
        chunk_bytes=args.chunk_bytes,
    )
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for caught_signal in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(caught_signal, stop.set)

    server = await asyncio.start_server(shaper.handle, args.listen.host, args.listen.port)
    sockets = server.sockets or []
    addresses = [socket.getsockname()[:2] for socket in sockets]
    print(
        json.dumps(
            {
                "event": "listening",
                "addresses": addresses,
                "upstream": [args.upstream.host, args.upstream.port],
                "rtt_ms": args.rtt_ms,
                "jitter_ms": args.jitter_ms,
                "upload_mbps": args.upload_mbps,
                "download_mbps": args.download_mbps,
                "per_connection_mbps": args.per_connection_mbps,
                "initial_window_bytes": args.initial_window_bytes,
                "max_window_bytes": args.max_window_bytes,
            },
            sort_keys=True,
        ),
        flush=True,
    )

    reporter = (
        asyncio.create_task(report_metrics(shaper.metrics, args.metrics_interval))
        if args.metrics_interval > 0
        else None
    )
    async with server:
        await stop.wait()

    if reporter is not None:
        reporter.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await reporter
    for connection in tuple(shaper.connections):
        connection.cancel()
    if shaper.connections:
        await asyncio.gather(*shaper.connections, return_exceptions=True)
    if args.summary_file:
        summary_path = Path(args.summary_file)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(json.dumps(shaper.metrics.summary(), indent=2, sort_keys=True) + "\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Forward TCP traffic with fixed RTT, slow-start window, and directional bandwidth limits."
    )
    parser.add_argument("--listen", type=parse_endpoint, default=parse_endpoint("127.0.0.1:18096"))
    parser.add_argument("--upstream", type=parse_endpoint, required=True)
    parser.add_argument("--rtt-ms", type=float, default=0, help="round-trip latency in milliseconds")
    parser.add_argument("--jitter-ms", type=float, default=0, help="bounded round-trip jitter in milliseconds")
    parser.add_argument("--jitter-seed", type=int, default=1, help="deterministic RNG seed for latency jitter")
    parser.add_argument("--upload-mbps", type=float, default=0, help="upload limit; zero is unlimited")
    parser.add_argument("--download-mbps", type=float, default=0, help="aggregate download limit; zero is unlimited")
    parser.add_argument(
        "--per-connection-mbps",
        type=float,
        default=0,
        help="per-connection download limit; zero is unlimited",
    )
    parser.add_argument(
        "--initial-window-bytes",
        type=int,
        default=0,
        help="per-connection TCP initial congestion window (e.g. 14600); doubles each RTT; zero disables",
    )
    parser.add_argument(
        "--max-window-bytes",
        type=int,
        default=0,
        help="per-connection receive/congestion window cap in bytes; zero is uncapped",
    )
    parser.add_argument("--stall-connection", type=int, default=0, help="1-based connection index to stall once")
    parser.add_argument("--stall-after-bytes", type=int, default=0, help="bytes downloaded before injecting stall")
    parser.add_argument("--stall-seconds", type=float, default=0, help="duration of injected stall in seconds")
    parser.add_argument("--chunk-bytes", type=int, default=16 * 1024)
    parser.add_argument("--metrics-interval", type=float, default=1.0, help="JSON metrics interval; zero disables")
    parser.add_argument("--summary-file", type=str, default="", help="optional path to write JSON metrics on exit")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if (
        args.rtt_ms < 0
        or args.jitter_ms < 0
        or args.upload_mbps < 0
        or args.download_mbps < 0
        or args.per_connection_mbps < 0
        or args.initial_window_bytes < 0
        or args.max_window_bytes < 0
        or args.stall_connection < 0
        or args.stall_after_bytes < 0
        or args.stall_seconds < 0
    ):
        parser.error("latency, window, stall, and bandwidth limits cannot be negative")
    if not 1024 <= args.chunk_bytes <= 1024 * 1024:
        parser.error("--chunk-bytes must be between 1024 and 1048576")
    if args.metrics_interval < 0:
        parser.error("--metrics-interval cannot be negative")
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
