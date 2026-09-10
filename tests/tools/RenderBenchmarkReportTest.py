#!/usr/bin/env python3
"""Benchmark comparisons must tolerate redistributed work and runner outliers."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def report(walls: dict[str, list[float]], *, cpu: float = 2.0, gap: float = 0.0,
           cold: bool = False, backend: str = "software") -> dict:
    return {
        "iterations": 4,
        "cold": cold,
        "quickBackend": backend,
        "samples": [
            {"routeTo": route, "wallMs": wall, "guiCpuMs": cpu,
             "instanceMs": cpu, "maxGapMs": gap, "actualSwaps": 1,
             "frameBudgetMs": 16.666667}
            for route, values in walls.items() for wall in values
        ],
    }


def run(script: Path, current: dict, *, baseline: dict | None = None,
        warn_only: bool = False, expected: int = 0) -> str:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        current_path = root / "current.json"
        current_path.write_text(json.dumps(current), encoding="utf-8")
        args = [sys.executable, str(script), str(current_path)]
        if baseline is not None:
            baseline_path = root / "baseline.json"
            baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
            args += ["--baseline", str(baseline_path)]
        if warn_only:
            args.append("--warn-only")
        result = subprocess.run(args, text=True, capture_output=True, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"report returned {result.returncode}, expected {expected}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def main() -> int:
    script = Path(sys.argv[1])
    baseline = report({"home": [20.0] * 4, "search": [20.0] * 4})

    # More CPU/construction/frame gaps or a different distribution of route work
    # must not fail a walk whose overall time has not regressed.
    run(script, report({"home": [35.0] * 4, "search": [5.0] * 4}, cpu=100.0, gap=200.0),
        baseline=baseline)

    # One scheduling outlier cannot dominate an otherwise unchanged walk.
    run(script, report({"home": [20.0, 2000.0, 20.0, 20.0], "search": [20.0] * 4}),
        baseline=baseline)

    # Both the relative and absolute margin matter.
    run(script, report({"home": [220.0] * 4, "search": [220.0] * 4}),
        baseline=report({"home": [200.0] * 4, "search": [200.0] * 4}))
    run(script, report({"home": [7.0] * 4}), baseline=report({"home": [2.0] * 4}))

    # A large overall slowdown fails strict mode, but is an annotation-only
    # warning in CI. This applies to cold and warm comparisons alike.
    for cold in (False, True):
        reference = report({"home": [20.0] * 4, "search": [20.0] * 4}, cold=cold)
        slower = report({"home": [30.0] * 4, "search": [30.0] * 4}, cold=cold)
        run(script, slower, baseline=reference, expected=1)
        output = run(script, slower, baseline=reference, warn_only=True)
        assert "::warning " in output, output

    # Different walks/render paths must not be compared as equivalent work.
    run(script, report({"home": [200.0] * 4}), baseline=baseline)
    run(script, report({"home": [200.0] * 4, "search": [200.0] * 4}, cold=True), baseline=baseline)
    run(script, report({"home": [200.0] * 4, "search": [200.0] * 4}, backend="rhi"), baseline=baseline)

    # Warning-only must not hide a broken measurement with no samples.
    run(script, report({}), warn_only=True, expected=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
