#!/usr/bin/env python3
"""Measure real playback through an already-running private Spool instance.

Use the normal credential restoration path; this driver never handles media URLs,
headers or account configuration. Playback updates normal server watch progress.
The report contains only numeric source/transport data and account ID prefixes.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spoolet", type=Path, required=True)
    parser.add_argument("--instance", required=True)
    parser.add_argument("--item", action="append", required=True, help="qualified playable item ID; repeat for servers")
    parser.add_argument("--output", type=Path, required=True, help="new private report directory")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=20, help="observed playback seconds after loading")
    parser.add_argument("--timeout", type=float, default=90, help="per-play startup deadline")
    args = parser.parse_args()
    if args.repeats < 1 or any(not math.isfinite(value) or value <= 0 for value in (args.seconds, args.timeout)):
        parser.error("counts and durations must be positive and finite")
    cli = args.spoolet.resolve()
    if not cli.is_file() or not os.access(cli, os.X_OK):
        parser.error("--spoolet must be an executable")
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = {"schemaVersion": 1, "complete": False, "scope": "real-server-auto-playback", "samples": []}

    def save():
        destination = args.output / "streaming.json"
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        temporary.replace(destination)

    def call(command, *values):
        result = subprocess.run([str(cli), "--instance", args.instance, command, *map(str, values)],
                                capture_output=True, text=True, timeout=15)
        try:
            reply = json.loads(result.stdout)
        except (ValueError, TypeError):
            raise RuntimeError("control command returned no valid response") from None
        if result.returncode != 0 or not reply.get("ok"):
            # Do not copy provider/server errors into the report.
            raise RuntimeError("control command rejected: " + command)
        return reply.get("result", {})

    def transport(state):
        raw = state.get("network", {})
        # The production snapshot is already credential-free. Keep this second
        # numeric allowlist so future diagnostics cannot expand report contents.
        keys = ("received-bytes", "current-received-bytes-per-second", "average-received-bytes-per-second",
                "peak-received-bytes-per-second", "elapsed-seconds", "selected-requests", "latency-ms",
                "capacity-bytes-per-second", "capacity-samples", "active-requests", "peak-active-requests",
                "failed-requests", "retry-attempts", "stream-id", "range-bytes", "ring-bytes")
        result = {key: raw[key] for key in keys if isinstance(raw.get(key), (int, float)) and not isinstance(raw[key], bool)
                  and math.isfinite(raw[key])}
        for key in ("lane-samples", "concurrency-samples"):
            result[key] = [{field: value for field, value in row.items()
                            if isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)}
                           for row in raw.get(key, []) if isinstance(row, dict)]
        return result

    save()
    active = False
    try:
        initial = call("status")
        if initial.get("playback", {}).get("active"):
            raise RuntimeError("instance is already playing; refusing to interrupt it")
        for item in args.item:
            account = item.split(":", 1)[0]
            for repeat in range(args.repeats):
                # Let deferred teardown finish: repeats include true core setup,
                # but intentionally retain learned per-server transport history.
                time.sleep(1.5)
                start = time.monotonic()
                call("play", item)
                active = True
                sample = {"account": account, "repeat": repeat + 1, "loadedMs": None,
                          "advancingMs": None, "bufferingSeconds": 0, "network": []}
                report["samples"].append(sample)
                last_tick = start
                previous_buffering = False
                loaded = None
                initial_position = None
                next_snapshot = start
                while True:
                    state = call("status")
                    now = time.monotonic()
                    playback = state.get("playback", {})
                    if playback.get("failed") or state.get("appError"):
                        raise RuntimeError("playback failed")
                    if previous_buffering:
                        sample["bufferingSeconds"] += now - last_tick
                    last_tick = now
                    previous_buffering = bool(playback.get("buffering"))
                    if loaded is None and playback.get("loaded"):
                        loaded = now
                        sample["loadedMs"] = (now - start) * 1000
                        initial_position = playback.get("position", 0)
                        source = state.get("source", {})
                        sample["source"] = {key: source[key] for key in ("bitrate", "width", "height", "qualityHeight")
                                            if isinstance(source.get(key), (int, float))}
                    if loaded is not None and sample["advancingMs"] is None:
                        if playback.get("position", 0) > initial_position + 0.1 and not playback.get("paused"):
                            sample["advancingMs"] = (now - start) * 1000
                    if now >= next_snapshot:
                        sample["network"].append({"atSeconds": now - start, **transport(state)})
                        next_snapshot = now + 1
                        save()
                    if loaded is None and now - start > args.timeout:
                        raise RuntimeError("startup deadline exceeded")
                    if loaded is not None and now - loaded >= args.seconds:
                        if sample["advancingMs"] is None:
                            raise RuntimeError("loaded media did not advance")
                        if not any("received-bytes" in row for row in sample["network"]):
                            raise RuntimeError("no real curl transport metrics observed")
                        break
                    time.sleep(0.05)
                call("stop")
                active = False
                save()
        report["complete"] = True
        lines = ["# Real-server automatic streaming profile", "",
                 "Observed on this client/network, not a TV or a guarantee of globally fastest startup.",
                 "Startup includes provider negotiation, player setup, container probing and rendering.",
                 "Repeats retain learned server history; playback updates normal watch progress.", "",
                 "| Account | Runs | Median loaded | Median advancing | Buffering total |",
                 "|---|---:|---:|---:|---:|"]
        for account in dict.fromkeys(row["account"] for row in report["samples"]):
            rows = [row for row in report["samples"] if row["account"] == account]
            lines.append(f"| {account} | {len(rows)} | {statistics.median(row['loadedMs'] for row in rows):.0f} ms | "
                         f"{statistics.median(row['advancingMs'] for row in rows):.0f} ms | "
                         f"{sum(row['bufferingSeconds'] for row in rows):.2f} s |")
        text = "\n".join(lines) + "\n"
        (args.output / "streaming.md").write_text(text)
        print(text, end="")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["failure"] = str(error)
        print("streaming profile: " + str(error))
        return 1
    finally:
        if active:
            try:
                call("stop")
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                pass
        save()


if __name__ == "__main__":
    raise SystemExit(main())
