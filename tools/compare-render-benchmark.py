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
import statistics
import sys
from collections import defaultdict
from pathlib import Path

METRICS = [
    ("maxGapMs", "worst frame gap"),
    ("wallMs", "wall"),
    ("guiCpuMs", "gui cpu"),
    ("instanceMs", "construct"),
    ("actualSwaps", "swaps"),
]


def load(path: Path) -> dict:
    with path.open() as handle:
        return json.load(handle)


def summarise(report: dict) -> dict:
    by_route: dict[str, list[dict]] = defaultdict(list)
    for sample in report.get("samples", []):
        by_route[sample.get("routeTo", "?")].append(sample)
    summary = {}
    for route, samples in by_route.items():
        entry = {
            key: statistics.median(float(sample.get(key, 0)) for sample in samples)
            for key, _ in METRICS
        }
        entry["__worstGap__"] = max(float(sample.get("maxGapMs", 0)) for sample in samples)
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
    args = parser.parse_args()

    report = load(args.current)
    samples = report.get("samples", [])
    if not samples:
        print("render benchmark: no samples recorded", file=sys.stderr)
        return 1
    current = summarise(report)
    baseline_report = load(args.baseline) if args.baseline and args.baseline.exists() else None
    baseline = summarise(baseline_report) if baseline_report else None
    budget = float(samples[0].get("frameBudgetMs", 16.7))
    cold = report.get("cold", False)
    comparable = bool(baseline) and (
        current.keys() == baseline.keys()
        and cold == baseline_report.get("cold", False)
        and report.get("quickBackend", "") == baseline_report.get("quickBackend", "")
    )
    overall = sum(entry["wallMs"] for entry in current.values())
    was_overall = sum(entry["wallMs"] for entry in baseline.values()) if comparable else None

    lines = [
        f"### Render benchmark ({'cold' if cold else 'warm'})",
        "",
        f"Frame budget {budget:.2f} ms · {len(samples)} switches · "
        f"{report.get('iterations', '?')} iterations",
        "",
        f"Overall transition time: **{overall:.1f} ms**{format_delta(overall, was_overall)} "
        "(sum of per-route medians; idle settle delays excluded).",
        "",
        "Route timings, CPU shares, construction costs and frame gaps are diagnostic only.",
    ]
    if args.warn_only:
        lines += ["", "Warning-only on shared CI runners. Run on dedicated hardware before enabling performance gates."]
    if baseline and not comparable:
        lines += ["", "Baseline route set, warm/cold mode or rendering backend differs; no overall comparison made."]
    lines += ["", "| route | " + " | ".join(label for _, label in METRICS) + " | worst gap |",
              "|" + "---|" * (len(METRICS) + 2)]
    for route in sorted(current):
        entry = current[route]
        cells = [route]
        for key, _ in METRICS:
            value = entry[key]
            was = baseline.get(route, {}).get(key) if baseline else None
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
