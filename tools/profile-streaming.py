#!/usr/bin/env python3
"""Measure real playback through an already-running private Spool instance.

Use the normal credential restoration path; this driver never handles media URLs,
headers or account configuration. Playback updates normal server watch progress.
The report contains only numeric source/transport data and account ID prefixes.

--request-counts compares fixed request counts (forced) with automatic selection
on the same items; touched streaming settings are restored afterwards. --host
drives a remote instance (for example a TV) through spoolet's SSH transport.
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
    parser.add_argument("--host", help="USER@HOST of a remote instance, passed to spoolet --host")
    parser.add_argument("--request-counts", default="auto",
                        help="comma list of fixed counts 1-16 and/or 'auto', e.g. 1,4,auto")
    parser.add_argument("--from-start", action="store_true", help="play every run from the beginning")
    parser.add_argument("--seek-seconds", type=float, help="seek to this position once loaded")
    args = parser.parse_args()
    if args.repeats < 1 or any(not math.isfinite(value) or value <= 0 for value in (args.seconds, args.timeout)):
        parser.error("counts and durations must be positive and finite")
    if args.seek_seconds is not None and (not math.isfinite(args.seek_seconds) or args.seek_seconds < 0):
        parser.error("--seek-seconds must be a finite position")
    counts = [part.strip() for part in args.request_counts.split(",") if part.strip()]
    if not counts or any(part != "auto" and not (part.isdigit() and 1 <= int(part) <= 16) for part in counts):
        parser.error("--request-counts takes 1-16 and/or auto")
    cli = args.spoolet.resolve()
    if not cli.is_file() or not os.access(cli, os.X_OK):
        parser.error("--spoolet must be an executable")
    os.umask(0o077)
    args.output.mkdir(parents=True, exist_ok=False, mode=0o700)
    report = {"schemaVersion": 3, "complete": False, "scope": "real-server-playback", "requestCounts": counts,
              "samples": []}

    def save():
        destination = args.output / "streaming.json"
        temporary = destination.with_suffix(".tmp")
        temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        temporary.replace(destination)

    def call(command, *values):
        remote = ["--host", args.host] if args.host else []
        result = subprocess.run([str(cli), *remote, "--instance", args.instance, command, *map(str, values)],
                                capture_output=True, text=True, timeout=30 if args.host else 15)
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
                "peak-received-bytes-per-second", "elapsed-seconds", "selected-requests", "ttfb-ms",
                "capacity-bytes-per-second", "capacity-samples", "active-requests", "peak-active-requests",
                "failed-requests", "retry-attempts", "stream-id", "range-bytes", "ring-bytes",
                "consumer-wait-seconds", "ring-wait-seconds", "head-of-line-seconds", "peak-reorder-bytes",
                "physical-connections", "streams-per-connection", "curl-thread-cpu-seconds", "curl-thread-wall-seconds",
                "tcp-rtt-min-ms", "tcp-rtt-max-ms", "max-socket-receive-buffer-bytes",
                "recent-scheduling-lateness-ms", "peak-scheduling-lateness-ms",
                "playback-buffer-seconds", "playback-demand-bits-per-second", "ordered-available-bytes",
                "consumer-delivered-bytes", "readable-bytes", "retained-bytes", "reorder-bytes", "copy-in-bytes",
                "copy-out-bytes", "reorder-copy-bytes", "body-callbacks", "worker-iterations", "coalesced-wakeups",
                "head-of-line-events", "reorder-wait-seconds", "oldest-gap-seconds",
                "perform-wall-seconds", "dispatch-wall-seconds", "poll-wall-seconds")
        def numeric(fields, row):
            return {field: row[field] for field in fields
                    if isinstance(row.get(field), (int, float)) and not isinstance(row[field], bool)
                    and math.isfinite(row[field])}
        result = numeric(keys, raw)
        samples = {
            "lane-samples": ("lane", "received-bytes", "active-seconds", "average-bytes-per-second",
                             "current-bytes-per-second", "peak-bytes-per-second", "capacity-bytes-per-second", "samples"),
            "concurrency-samples": ("requests", "capacity-bytes-per-second", "ttfb-ms", "samples"),
            "streams": ("stream-id", "readable-bytes", "retained-bytes", "reorder-bytes",
                        "prefix-publication-available"),
            "connections": ("connection-id", "socket-generation", "http-version", "transport", "active-requests",
                             "socket-receive-buffer-bytes", "receive-window-bytes", "tcp-rtt-ms"),
            "protocol-samples": ("http-version", "ordered-available-bytes", "cpu-ns-per-ordered-byte"),
        }
        for key, fields in samples.items():
            rows = raw.get(key, [])
            if isinstance(rows, list):
                result[key] = []
                for row in rows[:64]:
                    if not isinstance(row, dict):
                        continue
                    selected = numeric(fields, row)
                    if key == "streams" and isinstance(row.get("prefix-publication-available"), bool):
                        selected["prefix-publication-available"] = row["prefix-publication-available"]
                    result[key].append(selected)
        return result

    touched = ("playback/threadedStreaming", "playback/maxStreamingThreads", "playback/forceStreamingThreads")
    saved = {}

    def configure(count):
        # Fixed counts force the request count; auto uses the saved ceiling.
        call("settings", "set", "playback/threadedStreaming", "true")
        if count == "auto":
            call("settings", "set", "playback/forceStreamingThreads", "false")
            call("settings", "set", "playback/maxStreamingThreads",
                 json.dumps(saved.get("playback/maxStreamingThreads") or 4))
        else:
            call("settings", "set", "playback/forceStreamingThreads", "true")
            call("settings", "set", "playback/maxStreamingThreads", count)

    save()
    active = False
    try:
        initial = call("status")
        if initial.get("playback", {}).get("active"):
            raise RuntimeError("instance is already playing; refusing to interrupt it")
        values = call("settings", "get").get("values", {})
        saved = {key: values[key] for key in touched if key in values}
        runs = [(item, repeat) for item in args.item for repeat in range(args.repeats)]
        for item, repeat in runs:
            account = item.split(":", 1)[0]
            for count in counts:
                configure(count)
                # Let deferred teardown finish: repeats include true core setup,
                # but intentionally retain learned per-server transport history.
                time.sleep(1.5)
                start = time.monotonic()
                call("play", item, *(["--from-start"] if args.from_start else []))
                active = True
                sample = {"account": account, "requests": count, "repeat": repeat + 1, "loadedMs": None,
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
                        if args.seek_seconds is not None:
                            call("seek", args.seek_seconds)
                            initial_position = args.seek_seconds
                    if loaded is not None and sample["advancingMs"] is None:
                        if playback.get("position", 0) > initial_position + 0.1 and not playback.get("paused"):
                            sample["advancingMs"] = (now - start) * 1000
                    if now >= next_snapshot:
                        sample["network"].append({"atSeconds": now - start,
                            "eligible": bool(playback.get("loaded")) and not playback.get("paused")
                                        and not playback.get("seeking"),
                            **transport(state)})
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
                rows = sample["network"]
                sample["peakPhysicalConnections"] = max((row.get("physical-connections", 0) for row in rows), default=0)
                sample["peakActiveRequests"] = max((row.get("peak-active-requests", 0) for row in rows), default=0)
                # A fixed count the transport could not reach is not that count.
                sample["countReached"] = count == "auto" or sample["peakActiveRequests"] >= int(count)
                eligible = [(before, after) for before, after in zip(rows, rows[1:])
                            if before["eligible"] and after["eligible"]
                            and after["atSeconds"] > before["atSeconds"]
                            and before.get("stream-id") == after.get("stream-id")]
                span = sum(after["atSeconds"] - before["atSeconds"] for before, after in eligible)
                sample["eligibleSeconds"] = span
                for field, output, scale in (
                    ("ordered-available-bytes", "orderedMbps", 8e-6),
                    ("received-bytes", "receivedMbps", 8e-6),
                    ("consumer-wait-seconds", "consumerWaitFraction", 1),
                    ("head-of-line-seconds", "headOfLineFraction", 1),
                ):
                    intervals = [(before, after) for before, after in eligible
                                 if field in before and field in after and after[field] >= before[field]]
                    duration = sum(after["atSeconds"] - before["atSeconds"] for before, after in intervals)
                    if duration:
                        sample[output] = scale * sum(after[field] - before[field]
                                                     for before, after in intervals) / duration
                sample["effectiveHttpVersions"] = sorted({connection["http-version"]
                    for row in rows for connection in row.get("connections", [])
                    if connection.get("http-version") in (10, 11, 20, 30)})
                save()
        report["complete"] = True
        lines = ["# Real-server automatic streaming profile", "",
                 "Observed on this client/network, not a TV or a guarantee of globally fastest startup.",
                 "Startup includes provider negotiation, player setup, container probing and rendering.",
                 "Repeats retain learned server history; playback updates normal watch progress.", "",
                 "Ordered/received rates exclude paused/seeking intervals but include full-cache idle time; "
                 "they are not isolated capacity estimates. Compare the same item and offset.", "",
                 "| Account | Requests | Runs | Median loaded | Median advancing | Buffering total | "
                 "Median ordered | Median received | Median consumer wait | Median head-of-line | Peak connections | Count reached |",
                 "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]

        def median(rows, key, scale=1.0):
            values = [row[key] * scale for row in rows if isinstance(row.get(key), (int, float))]
            return statistics.median(values) if values else float("nan")

        for account, count in dict.fromkeys((row["account"], row["requests"]) for row in report["samples"]):
            rows = [row for row in report["samples"] if row["account"] == account and row["requests"] == count]
            lines.append(f"| {account} | {count} | {len(rows)} | {median(rows, 'loadedMs'):.0f} ms | "
                         f"{median(rows, 'advancingMs'):.0f} ms | "
                         f"{sum(row['bufferingSeconds'] for row in rows):.2f} s | "
                         f"{median(rows, 'orderedMbps'):.1f} Mbps | "
                         f"{median(rows, 'receivedMbps'):.1f} Mbps | {median(rows, 'consumerWaitFraction', 100):.0f}% | "
                         f"{median(rows, 'headOfLineFraction', 100):.0f}% | "
                         f"{max(row['peakPhysicalConnections'] for row in rows)} | "
                         f"{'yes' if all(row['countReached'] for row in rows) else 'NO'} |")
        text = "\n".join(lines) + "\n"
        (args.output / "streaming.md").write_text(text)
        print(text, end="")
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        category = ("control-timeout" if isinstance(error, subprocess.TimeoutExpired)
                    else "local-io" if isinstance(error, OSError)
                    else "invalid-control-data" if isinstance(error, ValueError)
                    else "playback-or-control-rejected")
        report["failure"] = category
        print("streaming profile: " + category)
        return 1
    finally:
        if active:
            try:
                call("stop")
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                pass
        for key, value in saved.items():
            try:
                call("settings", "set", key, json.dumps(value))
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                report.setdefault("restoreFailed", []).append(key)
        save()


if __name__ == "__main__":
    raise SystemExit(main())
