#!/usr/bin/env python3
"""Report real PlayerController setup; gate only egregious median costs.

Absolute ceilings deliberately do not compare different hosted runner machines.
A single scheduling outlier remains visible but cannot fail the performance gate.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import statistics
import sys

# Milliseconds, not interactive frame budgets: these catch seconds-long blocking
# or a catastrophically slow local setup without policing normal CI variance.
CEILINGS = {
    "constructCallMs": 2000,
    "playCallMs": 2000,
    "fileLoadedMs": 15000,
    "firstFrameMs": 20000,
    "playHeartbeatMaxGapMs": 2000,
    "setupWorkerMs": 15000,
    "preparationHeartbeatMaxGapMs": 2000,
}
METRICS = {
    "constructCallMs": "construct call",
    "playCallMs": "play call",
    "fileLoadedMs": "file loaded",
    "firstFrameMs": "visible frame",
    "playHeartbeatMaxGapMs": "play heartbeat gap",
    "preparationReadyMs": "preparation ready (includes heuristic delay)",
    "setupWorkerMs": "initialization worker",
    "preparationHeartbeatMaxGapMs": "preparation heartbeat gap",
}


def describe(report: dict) -> tuple[str, bool]:
    if not isinstance(report, dict) or report.get("schemaVersion") != 1 or report.get("complete") is not True:
        raise ValueError("missing or incomplete player setup measurement")
    samples = report.get("samples")
    if not isinstance(samples, list) or len(samples) < 6:
        raise ValueError("at least three real samples per mode are required")
    groups = {mode: [] for mode in ("cold", "prepared")}
    for sample in samples:
        if not isinstance(sample, dict) or sample.get("mode") not in groups:
            raise ValueError("invalid sample mode")
        for metric in METRICS:
            value = sample.get(metric)
            if (isinstance(value, bool) or not isinstance(value, (int, float))
                    or not math.isfinite(value) or value < 0):
                raise ValueError(f"missing or invalid {metric}")
        if sample["fileLoadedMs"] <= 0 or sample["firstFrameMs"] < sample["fileLoadedMs"]:
            raise ValueError("sample did not measure media loading and a subsequent frame")
        if not isinstance(sample.get("preparedAdopted"), bool):
            raise ValueError("sample does not attribute prepared core adoption")
        if sample["mode"] == "prepared":
            if not sample["preparedAdopted"] or sample.get("preparationHeartbeats", 0) < 1:
                raise ValueError("prepared sample did not observe background preparation and adoption")
        elif sample["preparedAdopted"]:
            raise ValueError("cold sample unexpectedly adopted a prepared core")
        groups[sample["mode"]].append(sample)
    if any(len(group) < 3 for group in groups.values()):
        raise ValueError("at least three samples of each mode are required")

    lines = ["### Player setup benchmark", "",
             "Real PlayerController + libmpv, local demux/decode, embedded OpenGL video; the fixture has no audio stream.",
             "Cold means a fresh controller without a prepared core; only the first cold sample is process-cold. "
             "OS/library caches are not flushed. Prepared readiness includes the launch-safe heuristic delay.",
             "Heartbeat gaps are GUI timer delivery gaps, not display presentation or dropped-frame counts. "
             "Visible-frame timing includes framebuffer capture; actual panel presentation is not measured.", "",
             f"Qt {report.get('qtVersion', '?')} · {report.get('cpuArchitecture', '?')} · "
             f"{report.get('qpaPlatform', '?')} · {len(samples)} samples", "",
             "| mode | metric | median ms | worst ms | fail ceiling (median ms) |",
             "|---|---|---:|---:|---:|"]
    failures = []
    for mode, group in groups.items():
        for metric, label in METRICS.items():
            if mode == "cold" and metric.startswith("preparation"):
                continue
            values = [sample[metric] for sample in group]
            median = statistics.median(values)
            ceiling = CEILINGS.get(metric)
            if ceiling is not None and median > ceiling:
                failures.append(f"{mode} {label}: {median:.1f} ms > {ceiling} ms")
            lines.append(f"| {mode} | {label} | {median:.1f} | {max(values):.1f} | "
                         f"{ceiling if ceiling is not None else 'diagnostic only'} |")
    lines += ["", "Only egregious absolute median costs fail on shared runners; individual outliers and "
              "cold/prepared ratios are diagnostic, not gates."]
    if failures:
        lines += ["", "**Egregious player setup slowdown:**", *[f"- {failure}" for failure in failures]]
    return "\n".join(lines) + "\n", bool(failures)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("current", type=Path)
    parser.add_argument("--markdown", type=Path)
    args = parser.parse_args()
    try:
        text, failed = describe(json.loads(args.current.read_text(encoding="utf-8")))
    except (OSError, ValueError, TypeError) as error:
        text = f"### Player setup benchmark\n\n**Measurement failed:** {error}\n"
        failed = True
    print(text, end="")
    if args.markdown:
        args.markdown.write_text(text, encoding="utf-8")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
