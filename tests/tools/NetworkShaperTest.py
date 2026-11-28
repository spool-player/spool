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

    async def start_proxy(
        self,
        *,
        latency: float = 0,
        download_rate: float = 0,
        per_connection_rate: float = 0,
        initial_window: int = 0,
        max_window: int = 0,
        stall_connection: int = 0,
        stall_after_bytes: int = 0,
        stall_seconds: float = 0.0,
    ) -> int:
        self.shaper = network_shaper.Shaper(
            self.upstream_endpoint,
            latency_seconds=latency,
            upload_bytes_per_second=0,
            download_bytes_per_second=download_rate,
            per_connection_download_bytes_per_second=per_connection_rate,
            initial_window_bytes=initial_window,
            max_window_bytes=max_window,
            stall_connection=stall_connection,
            stall_after_bytes=stall_after_bytes,
            stall_seconds=stall_seconds,
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
        self.assertGreaterEqual(self.shaper.metrics.peak_active_connections, 2)

    async def test_per_connection_limit_scales_with_parallel_connections(self) -> None:
        paced_bytes: dict[object, int] = {}

        class ObservedPacer(network_shaper.Pacer):
            async def wait(self, byte_count: int) -> None:
                if self._bytes_per_second > 0:
                    paced_bytes[self] = paced_bytes.get(self, 0) + byte_count
                await super().wait(byte_count)

        with patch.object(network_shaper, "Pacer", ObservedPacer):
            port = await self.start_proxy(per_connection_rate=256_000)
            started_at = time.monotonic()
            results = await asyncio.gather(self.fetch(port), self.fetch(port))
            elapsed = time.monotonic() - started_at

        self.assertTrue(all(body == PAYLOAD for _, body in results))
        # Both payloads must traverse separate rate reservations. A wall-clock
        # upper bound also measures unrelated runner scheduling and socket I/O.
        self.assertGreaterEqual(elapsed, 0.42)
        self.assertEqual(len(paced_bytes), 2)
        self.assertTrue(all(count >= len(PAYLOAD) for count in paced_bytes.values()))
        self.assertEqual(self.shaper.metrics.peak_active_connections, 2)

    async def test_slow_start_window_delays_single_connection_warmup(self) -> None:
        # 128 KiB over 50ms RTT starting at 16 KiB cwnd requires ~4 RTTs (16+32+64+128).
        port = await self.start_proxy(latency=0.025, initial_window=16 * 1024, max_window=128 * 1024)
        started_at = time.monotonic()
        _, body = await self.fetch(port)
        elapsed = time.monotonic() - started_at

        self.assertEqual(body, PAYLOAD)
        self.assertGreaterEqual(elapsed, 0.18)

    async def test_injects_one_shot_connection_stall(self) -> None:
        port = await self.start_proxy(stall_connection=1, stall_after_bytes=8192, stall_seconds=0.25)
        started_at = time.monotonic()
        _, body1 = await self.fetch(port)
        first_elapsed = time.monotonic() - started_at

        _, body2 = await self.fetch(port)

        self.assertEqual(body1, PAYLOAD)
        self.assertEqual(body2, PAYLOAD)
        self.assertGreaterEqual(first_elapsed, 0.23)
        self.assertEqual(self.shaper.metrics.injected_stalls, 1)


class PacerTest(unittest.IsolatedAsyncioTestCase):
    async def test_independent_pacers_do_not_reserve_a_shared_timeline(self) -> None:
        now = 100.0
        deadlines = []

        async def sleep(delay):
            deadlines.append(now + delay)

        timers = SimpleNamespace(get_running_loop=lambda: SimpleNamespace(time=lambda: now), sleep=sleep)
        with patch.object(network_shaper, "asyncio", timers):
            first, second = network_shaper.Pacer(1000), network_shaper.Pacer(1000)
            await first.wait(500)
            await second.wait(500)
            await first.wait(500)
            await second.wait(500)
            self.assertEqual(deadlines, [100.5, 100.5])
            deadlines.clear()
            shared = network_shaper.Pacer(1000)
            for _ in range(4):
                await shared.wait(500)
            self.assertEqual(deadlines, [100.5, 101.0, 101.5])

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


class SlowStartWindowTest(unittest.IsolatedAsyncioTestCase):
    async def test_doubles_cwnd_each_rtt_and_caps_at_max_window(self) -> None:
        now = 10.0

        async def fake_sleep(delay: float) -> None:
            nonlocal now
            now += delay

        clock = SimpleNamespace(time=lambda: now)
        timers = SimpleNamespace(get_running_loop=lambda: clock, sleep=fake_sleep)
        window = network_shaper.SlowStartWindow(
            rtt_seconds=0.1,
            initial_window_bytes=10_000,
            max_window_bytes=30_000,
        )
        with patch.object(network_shaper, "asyncio", timers):
            # First 10 KB fits in IW10 immediately (t=10.0).
            await window.wait(10_000)
            self.assertAlmostEqual(now, 10.0, places=6)
            self.assertEqual(window.cwnd_bytes, 10_000)

            # Next 20 KB triggers 1 RTT wait (t=10.1), cwnd doubles to 20 KB.
            await window.wait(20_000)
            self.assertAlmostEqual(now, 10.1, places=6)
            self.assertEqual(window.cwnd_bytes, 20_000)

            # Next 30 KB triggers 2nd RTT wait (t=10.2), cwnd caps at 30 KB.
            await window.wait(30_000)
            self.assertAlmostEqual(now, 10.2, places=6)
            self.assertEqual(window.cwnd_bytes, 30_000)

            # Next 30 KB triggers 3rd RTT wait (t=10.3), cwnd stays capped at 30 KB.
            await window.wait(30_000)
            self.assertAlmostEqual(now, 10.3, places=6)
            self.assertEqual(window.cwnd_bytes, 30_000)


if __name__ == "__main__":
    unittest.main()
