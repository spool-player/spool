#!/usr/bin/env python3
"""The Actions report gates sustained disasters, not noisy hosted-runner samples."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def report():
    return {"schemaVersion": 1, "complete": True, "samples": [
        {"mode": mode, "constructCallMs": 2, "playCallMs": 5,
         "fileLoadedMs": 80, "firstFrameMs": 100,
         "playHeartbeatMaxGapMs": 20, "preparationReadyMs": 3100 if mode == "prepared" else 0,
         "setupWorkerMs": 100,
         "preparationHeartbeatMaxGapMs": 20 if mode == "prepared" else 0,
         "preparationHeartbeats": 300 if mode == "prepared" else 0,
         "preparedAdopted": mode == "prepared"}
        for mode in ("cold", "prepared") for _ in range(3)]}


def run(script, value, expected):
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "setup.json"
        markdown = Path(directory) / "summary.md"
        source.write_text(json.dumps(value), encoding="utf-8")
        process = subprocess.run([sys.executable, str(script), str(source), "--markdown", str(markdown)],
                                 text=True, capture_output=True, check=False)
        assert process.returncode == expected, (process.returncode, process.stdout, process.stderr)
        assert markdown.read_text(encoding="utf-8") == process.stdout
        return process.stdout


def main():
    script = Path(sys.argv[1])
    healthy = report()
    assert "| prepared | file loaded | 80.0 |" in run(script, healthy, 0)
    # One descheduled sample stays visible, but medians remain healthy.
    outlier = report()
    outlier["samples"][0].update(fileLoadedMs=24000, firstFrameMs=25000, playHeartbeatMaxGapMs=12000)
    assert "25000.0" in run(script, outlier, 0)
    for mode in ("cold", "prepared"):
        for metric in ("playCallMs", "constructCallMs", "playHeartbeatMaxGapMs",
                       "fileLoadedMs", "firstFrameMs"):
            slow = report()
            for sample in slow["samples"]:
                if sample["mode"] == mode:
                    sample[metric] = 24000
                    if metric == "fileLoadedMs":
                        sample["firstFrameMs"] = 25000
            assert "Egregious" in run(script, slow, 1)
    for metric, value in (("preparationHeartbeatMaxGapMs", 3000), ("setupWorkerMs", 16000)):
        slow = report()
        for sample in slow["samples"][3:]:
            sample[metric] = value
        run(script, slow, 1)
    # Unmeasured or nonfinite values must never silently become a fast run.
    for bad in (None, -1, True, "1", float("nan"), float("inf")):
        invalid = report()
        invalid["samples"][0]["playCallMs"] = bad
        run(script, invalid, 1)
    for change in (lambda value: value.update(complete=False),
                   lambda value: value.update(samples=value["samples"][:3]),
                   lambda value: value["samples"][3].update(preparedAdopted=False),
                   lambda value: value["samples"][0].update(preparedAdopted=True),
                   lambda value: value["samples"][3].update(preparationHeartbeats=0),
                   lambda value: value["samples"][0].pop("firstFrameMs")):
        invalid = copy.deepcopy(healthy)
        change(invalid)
        assert "Measurement failed" in run(script, invalid, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
