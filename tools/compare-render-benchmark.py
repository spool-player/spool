#!/usr/bin/env python3
"""Report route timings; flag only large overall transition-time regressions.

Overall time is the sum of per-route median wall times: one complete measured
walk, without idle settle delays or a single scheduling outlier. Individual
routes, CPU shares, construction costs and frame gaps are diagnostic only, so
redistributing work between routes does not penalise a new implementation.

GitHub Actions uses --warn-only because hosted runners are not comparable
performance machines. Move gating to dedicated hardware before enforcing it.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

METRICS = [
    ("maxGapMs", "timer lateness"),
    ("wallMs", "wall"),
    ("guiCpuMs", "gui cpu"),
    ("instanceMs", "instance ready"),
    ("actualSwaps", "swaps"),
]

COMPARISON_KEYS = (
    "schemaVersion", "timingOrigin", "script", "qpaPlatform", "graphicsApi",
    "framePumpMs", "windowWidth", "windowHeight", "devicePixelRatio",
    "library", "listMode", "frameBudgetMs", "measurementScope", "cold",
    "quickBackend", "qtVersion", "cpuArchitecture", "buildAbi", "providerId",
    "providerRuntime", "warmupPasses", "settleMs", "stepTimeoutMs", "cacheMode",
    "interpreterRequested", "deviceLabel", "iterations",
)


def validate_report(report: dict) -> None:
    """Reject missing/invalid measurements rather than turning them into zero."""
    if not isinstance(report, dict):
        raise ValueError("report must be a JSON object")
    if report.get("complete") is False or report.get("failures"):
        raise ValueError("incomplete run; inspect failures before comparing")
    samples = report.get("samples")
    if not isinstance(samples, list) or not samples:
        raise ValueError("no route samples recorded")
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict) or not isinstance(sample.get("routeTo"), str) or not sample["routeTo"]:
            raise ValueError(f"sample {index} has no route")
        for key in [key for key, _ in METRICS] + ["frameBudgetMs"]:
            value = sample.get(key)
            if (isinstance(value, bool) or not isinstance(value, (float, int))
                    or not math.isfinite(value) or value < 0):
                raise ValueError(f"sample {index} has missing or invalid {key}")
        if sample["wallMs"] <= 0 or sample["actualSwaps"] < 1 or sample["frameBudgetMs"] <= 0:
            raise ValueError(f"sample {index} has no measured transition/frame")


def comparable(current: dict, baseline: dict) -> bool:
    return (summarise(current).keys() == summarise(baseline).keys()
            and all(current.get(key) == baseline.get(key) for key in COMPARISON_KEYS))


def load(path: Path) -> dict:
    with path.open() as handle:
        return json.load(handle)


def summarise(report: dict) -> dict:
    by_route: dict[str, list[dict]] = defaultdict(list)
    for sample in report.get("samples", []):
        by_route[sample["routeTo"]].append(sample)
    summary = {}
    for route, samples in by_route.items():
        entry = {
            key: statistics.median(sample[key] for sample in samples)
            for key, _ in METRICS
        }
        entry["__worstGap__"] = max(sample["maxGapMs"] for sample in samples)
        entry["count"] = len(samples)
        summary[route] = entry
    return summary


def format_delta(current: float, baseline: float | None) -> str:
    if baseline is None or baseline <= 0:
        return ""
    change = current / baseline - 1
    return " (=)" if abs(change) < 0.005 else f" ({change:+.0%})"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("current", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--tolerance", type=float, default=0.25,
                        help="minimum fractional increase in overall transition time")
    parser.add_argument("--warn-only", action="store_true",
                        help="report performance regressions as GitHub warnings, without failing")
    parser.add_argument("--markdown", type=Path)
    parser.add_argument("--require-comparable", action="store_true",
                        help="fail if the requested baseline is absent or uses a different workload/environment")
    args = parser.parse_args()

    if not math.isfinite(args.tolerance) or args.tolerance < 0:
        parser.error("--tolerance must be finite and nonnegative")
    try:
        report = load(args.current)
        validate_report(report)
        baseline_report = load(args.baseline) if args.baseline and args.baseline.exists() else None
        if baseline_report is not None:
            validate_report(baseline_report)
    except (OSError, ValueError, TypeError) as error:
        print(f"render benchmark: {error}", file=sys.stderr)
        return 1
    samples = report["samples"]
    current = summarise(report)
    baseline = summarise(baseline_report) if baseline_report is not None else None
    budget = float(samples[0]["frameBudgetMs"])
    cold = report.get("cold", False)
    can_compare = baseline_report is not None and comparable(report, baseline_report)
    if args.require_comparable and not can_compare:
        print("render benchmark: comparable baseline required but absent or incompatible", file=sys.stderr)
        return 1
    overall = sum(entry["wallMs"] for entry in current.values())
    was_overall = sum(entry["wallMs"] for entry in baseline.values()) if can_compare else None

    lines = [
        f"### Render benchmark ({'cold' if cold else 'warm'})",
        "",
        f"Frame budget {budget:.2f} ms · {len(samples)} switches · "
        f"{report.get('iterations', '?')} iterations",
        "",
        f"Overall transition time: **{overall:.1f} ms**{format_delta(overall, was_overall)} "
        "(sum of per-route medians; idle settle delays excluded).",
        "",
        "Route timings and CPU shares are diagnostic. Timer lateness is not a dropped-frame count.",
        "Swap callbacks are not measured panel presentation; provider response processing is not isolated here.",
    ]
    if report.get("schemaVersion", 1) < 2:
        lines += ["", "Legacy timing: synchronous page construction may be excluded and cold loads labelled warm."]
    if args.warn_only:
        lines += ["", "Warning-only on shared CI runners. Run on dedicated hardware before enabling performance gates."]
    if args.baseline and baseline_report is None:
        lines += ["", "Baseline file is absent; reporting measurements without a comparison."]
    if baseline is not None and not can_compare:
        lines += ["", "Baseline workload, timing origin or rendering environment differs; no comparison made."]
    lines += ["", "| route | " + " | ".join(label for _, label in METRICS) + " | max lateness |",
              "|" + "---|" * (len(METRICS) + 2)]
    for route in sorted(current):
        entry = current[route]
        cells = [route]
        for key, _ in METRICS:
            value = entry[key]
            was = baseline.get(route, {}).get(key) if can_compare else None
            unit = "" if key == "actualSwaps" else " ms"
            precision = 0 if key == "actualSwaps" else 1
            cells.append(f"{value:.{precision}f}{unit}{format_delta(value, was)}")
        cells.append(f"{entry['__worstGap__']:.1f} ms")
        lines.append("| " + " | ".join(cells) + " |")

    regression = (
        was_overall is not None and was_overall > 0
        and overall >= was_overall * (1 + args.tolerance)
        and overall - was_overall >= budget
    )
    if regression:
        message = (f"Overall transition time increased {overall / was_overall - 1:+.0%}: "
                   f"{was_overall:.1f} ms to {overall:.1f} ms "
                   f"(+{overall - was_overall:.1f} ms).")
        lines += ["", "**Overall regression**", "", message]
        if args.warn_only:
            print(f"::warning title=Render benchmark regression::{message}")
    text = "\n".join(lines)
    print(text)
    if args.markdown:
        args.markdown.write_text(text + "\n")
    return 1 if regression and not args.warn_only else 0


if __name__ == "__main__":
    sys.exit(main())
