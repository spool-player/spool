#!/usr/bin/env python3

from __future__ import annotations

import asyncio
import importlib.util
import sys
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch


SCRIPT_PATH = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0]]
SPEC = importlib.util.spec_from_file_location("network_shaper", SCRIPT_PATH)
assert SPEC and SPEC.loader
network_shaper = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = network_shaper
SPEC.loader.exec_module(network_shaper)

PAYLOAD = b"x" * (128 * 1024)


class NetworkShaperTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        async def serve(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            try:
                await reader.readuntil(b"\r\n\r\n")
                writer.write(
                    f"HTTP/1.1 200 OK\r\nContent-Length: {len(PAYLOAD)}\r\nConnection: close\r\n\r\n".encode()
                    + PAYLOAD
                )
                await writer.drain()
            finally:
                writer.close()
                await writer.wait_closed()

        self.upstream = await asyncio.start_server(serve, "127.0.0.1", 0)
        upstream_port = self.upstream.sockets[0].getsockname()[1]
        self.upstream_endpoint = network_shaper.Endpoint("127.0.0.1", upstream_port)
        self.proxy: asyncio.Server | None = None
        self.shaper = None

    async def asyncTearDown(self) -> None:
        if self.proxy is not None:
            self.proxy.close()
            await self.proxy.wait_closed()
        if self.shaper is not None:
            for connection in tuple(self.shaper.connections):
                connection.cancel()
            if self.shaper.connections:
                await asyncio.gather(*self.shaper.connections, return_exceptions=True)
        self.upstream.close()
        await self.upstream.wait_closed()

    async def start_proxy(self, *, latency: float = 0, download_rate: float = 0) -> int:
        self.shaper = network_shaper.Shaper(
            self.upstream_endpoint,
            latency_seconds=latency,
            upload_bytes_per_second=0,
            download_bytes_per_second=download_rate,
            chunk_bytes=4096,
        )
        self.proxy = await asyncio.start_server(self.shaper.handle, "127.0.0.1", 0)
        return self.proxy.sockets[0].getsockname()[1]

    async def fetch(self, port: int) -> tuple[float, bytes]:
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        started_at = time.monotonic()
        writer.write(b"GET /payload HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        await writer.drain()
        await reader.readuntil(b"\r\n\r\n")
        first_byte = await reader.readexactly(1)
        first_byte_at = time.monotonic()
        body = first_byte + await reader.read()
        writer.close()
        await writer.wait_closed()
        return first_byte_at - started_at, body

    async def test_applies_configured_round_trip_latency(self) -> None:
        port = await self.start_proxy(latency=0.1)
        time_to_first_byte, body = await self.fetch(port)

        self.assertEqual(body, PAYLOAD)
        self.assertGreaterEqual(time_to_first_byte, 0.18)

    async def test_limits_aggregate_bandwidth_across_connections(self) -> None:
        port = await self.start_proxy(download_rate=125_000)
        started_at = time.monotonic()
        results = await asyncio.gather(self.fetch(port), self.fetch(port))
        elapsed = time.monotonic() - started_at

        self.assertTrue(all(body == PAYLOAD for _, body in results))
        self.assertGreaterEqual(elapsed, 1.8)
        # A rate limit bounds throughput, not runner scheduling or socket delays.
        self.assertGreaterEqual(self.shaper.metrics.download_bytes, len(PAYLOAD) * 2)
        self.assertEqual(self.shaper.metrics.total_connections, 2)


class PacerTest(unittest.IsolatedAsyncioTestCase):
    async def test_reserves_bandwidth_before_waiting(self) -> None:
        now = 100.0
        sleepers: list[tuple[float, asyncio.Future[None]]] = []
        sent: list[tuple[int, float]] = []

        async def sleep(delay: float) -> None:
            ready = asyncio.get_running_loop().create_future()
            sleepers.append((now + delay, ready))
            await ready

        async def advance(to: float) -> None:
            nonlocal now
            now = to
            for deadline, ready in sleepers:
                if deadline <= now and not ready.done():
                    ready.set_result(None)
            await asyncio.sleep(0)

        async def send(byte_count: int) -> None:
            await pacer.wait(byte_count)
            sent.append((byte_count, now))

        # Control only the pacer's clock and timer; tasks still run concurrently.
        clock = SimpleNamespace(time=lambda: now)
        timers = SimpleNamespace(get_running_loop=lambda: clock, sleep=sleep)
        pacer = network_shaper.Pacer(1000)
        with patch.object(network_shaper, "asyncio", timers):
            async with asyncio.TaskGroup() as tasks:
                for byte_count in (250, 500, 100):
                    tasks.create_task(send(byte_count))
                await asyncio.sleep(0)
                self.assertEqual(sent, [(250, 100.0)])

                await advance(100.249)
                self.assertEqual(sent, [(250, 100.0)])
                await advance(100.25)
                self.assertEqual(sent, [(250, 100.0), (500, 100.25)])

                await advance(100.749)
                self.assertEqual(sent, [(250, 100.0), (500, 100.25)])
                await advance(100.75)
                self.assertEqual(sent, [(250, 100.0), (500, 100.25), (100, 100.75)])


if __name__ == "__main__":
    unittest.main()
