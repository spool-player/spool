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
        warn_only: bool = False, expected: int = 0, require_comparable: bool = False,
        baseline_missing: bool = False) -> str:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        current_path = root / "current.json"
        current_path.write_text(json.dumps(current), encoding="utf-8")
        args = [sys.executable, str(script), str(current_path)]
        if baseline is not None:
            baseline_path = root / "baseline.json"
            baseline_path.write_text(json.dumps(baseline), encoding="utf-8")
            args += ["--baseline", str(baseline_path)]
        elif baseline_missing:
            args += ["--baseline", str(root / "missing.json")]
        if require_comparable:
            args.append("--require-comparable")
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

    # New timing includes construction: old and new origins are not an A/B pair.
    for key, value in (("schemaVersion", 2), ("qpaPlatform", "wayland"),
                       ("framePumpMs", 0), ("windowWidth", 3840), ("library", "Movies")):
        changed = report({"home": [200.0] * 4, "search": [200.0] * 4})
        changed[key] = value
        output = run(script, changed, baseline=baseline)
        assert "no comparison made" in output, output
        assert "+900%" not in output, "incompatible reports must not print per-route deltas"

    incomplete = report({"home": [1.0] * 4})
    incomplete["complete"] = False
    incomplete["failures"] = [{"reason": "route_timeout"}]
    run(script, incomplete, warn_only=True, expected=1)
    run(script, baseline, baseline=incomplete, warn_only=True, expected=1)

    # Warning-only must not hide a broken measurement with no samples.
    run(script, report({}), warn_only=True, expected=1)

    # Present-but-unmeasured values must not silently become zero, NaN must
    # not disable regression comparisons, and a zero swap count is no frame.
    for key in ("wallMs", "guiCpuMs", "instanceMs", "maxGapMs", "actualSwaps", "frameBudgetMs"):
        for value in (None, -1, float("nan"), float("inf"), True, "1"):
            invalid = report({"home": [20.0] * 4})
            invalid["samples"][0][key] = value
            run(script, invalid, warn_only=True, expected=1)
        missing = report({"home": [20.0] * 4})
        del missing["samples"][0][key]
        run(script, baseline, baseline=missing, warn_only=True, expected=1)
    for key in ("wallMs", "actualSwaps", "frameBudgetMs"):
        unmeasured = report({"home": [20.0] * 4})
        unmeasured["samples"][0][key] = 0
        run(script, unmeasured, expected=1)
    run(script, baseline, baseline=report({}), warn_only=True, expected=1)

    # Local A/B runs can demand proof of comparable work; warning-only must
    # not turn absent/incompatible baselines into a successful strict run.
    run(script, baseline, baseline=baseline, require_comparable=True)
    run(script, baseline, baseline_missing=True)
    run(script, baseline, baseline_missing=True, require_comparable=True, warn_only=True, expected=1)
    run(script, baseline, require_comparable=True, expected=1)
    for key, value in (("qtVersion", "different"), ("providerRuntime", "different"),
                       ("settleMs", 500), ("iterations", 1), ("warmupPasses", 2)):
        incompatible = report({"home": [20.0] * 4, "search": [20.0] * 4})
        incompatible[key] = value
        run(script, incompatible, baseline=baseline, require_comparable=True,
            warn_only=True, expected=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
