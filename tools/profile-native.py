#!/usr/bin/env python3
"""Measure real route transitions on Linux's private native graphics display.

Run through nix develop .#native. Never attaches to a running app, copies an
account profile, or uses the host compositor. No raw application logs are saved.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
GRAPHICS_ENV = ("SPOOL_TEST_DRI_DIR", "SPOOL_TEST_VULKAN_ICD",
                "SPOOL_TEST_DRIVER_LIB_DIR", "SPOOL_TEST_EGL_VENDOR")
SPEC = importlib.util.spec_from_file_location("render_report", ROOT / "tools/compare-render-benchmark.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class Cancelled(Exception):
    pass


def cancel(signum, _frame):
    raise Cancelled(signum)


def write_json(path: Path, value: dict) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def stop_group(process: subprocess.Popen) -> None:
    # Kill the entire launcher/compositor/app group, even if the group leader
    # exited first. Only descendants of this invocation can be targeted.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait()


def measure(app: Path, output: Path, cold: bool, args) -> dict:
    environment = os.environ.copy()
    for key in list(environment):
        if (key.startswith("SPOOL_") and key not in GRAPHICS_ENV) or key in (
            "QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "QT_QPA_PLATFORMTHEME",
            "QV4_FORCE_INTERPRETER", "QSG_RHI_BACKEND",
        ):
            environment.pop(key)
    environment.update({
        "SPOOL_BENCH": "routes",
        "SPOOL_BENCH_OUT": str(output),
        "SPOOL_BENCH_COLD": "1" if cold else "0",
        "SPOOL_BENCH_ITERATIONS": str(args.iterations),
        "SPOOL_BENCH_WARMUP": str(args.warmup),
        "SPOOL_BENCH_TIMEOUT_MS": str(args.step_timeout_ms),
        "SPOOL_BENCH_SETTLE_MS": str(args.settle_ms),
        "SPOOL_BENCH_PUMP_MS": "8",
        "SPOOL_BENCH_DEVICE_LABEL": "native-private",
        "SPOOL_WINDOW_SIZE": "1920x1080",
        "SPOOL_TEST_DISPLAY_BACKEND": "weston",
        "QSG_RHI_BACKEND": "opengl",
        "QT_LOGGING_RULES": "*.debug=false",
    })
    # Both app and compositor keep all caches/state/diagnostics under this run's
    # temporary root, including on timeout. Do not retain potentially private
    # logs (even startup failures), environment dumps or account databases.
    with tempfile.TemporaryDirectory(prefix="spool-profile-") as directory:
        environment["TMPDIR"] = directory
        process = subprocess.Popen(
            ["bash", str(ROOT / "tools/launch-native-app.sh"), str(app)],
            env=environment, cwd=ROOT, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True,
        )
        try:
            code = process.wait(timeout=args.timeout)
            if code != 0:
                raise ValueError(f"benchmark/isolated graphics launcher exited {code}; no valid run")
        except subprocess.TimeoutExpired:
            raise ValueError(f"benchmark exceeded {args.timeout:g}s deadline") from None
        finally:
            # A second Ctrl-C must not interrupt teardown and orphan the app.
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                stop_group(process)
            finally:
                signal.signal(signal.SIGINT, cancel)
                signal.signal(signal.SIGTERM, cancel)
    if not output.is_file():
        raise ValueError("app exited without writing its benchmark report")
    report = REPORT.load(output)
    REPORT.validate_report(report)
    if (report.get("schemaVersion") != 2 or report.get("complete") is not True
            or report.get("iterations") != args.iterations
            or report.get("warmupPasses") != args.warmup
            or report.get("cold") != cold or report.get("qpaPlatform") != "wayland"
            or report.get("measurementScope") != "route-transition"):
        raise ValueError("app report does not describe the requested native route measurement")
    script = report.get("script")
    if not isinstance(script, list) or not script or any(not isinstance(route, str) for route in script):
        raise ValueError("app report has no benchmark script")
    # The warmup ends on the final route, so repeated identical routes are
    # deliberately skipped by the real benchmark. Require every other step in
    # every measured pass; a partial walk must never look like a faster walk.
    previous = script[-1]
    steps = []
    for step, route in enumerate(script):
        if route != previous:
            steps.append(step)
        previous = route
    expected = {(run, step) for run in range(args.iterations) for step in steps}
    observed = [(sample.get("pass"), sample.get("step")) for sample in report["samples"]]
    if len(observed) != len(expected) or set(observed) != expected:
        raise ValueError("benchmark did not measure every requested route step/pass exactly once")
    if any(sample["routeTo"] != script[sample["step"]] for sample in report["samples"]):
        raise ValueError("benchmark samples do not match their script steps")
    idle = report.get("idleGapsMs")
    if (not isinstance(idle, list) or len(idle) != len(observed)
            or any(isinstance(value, bool) or not isinstance(value, (int, float))
                   or not math.isfinite(value) or value < 0 for value in idle)):
        raise ValueError("benchmark did not record a valid idle noise floor beside every route sample")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build/linux-release/install/bin/spool")
    parser.add_argument("--output", type=Path, required=True, help="new artifact directory; must not already exist")
    parser.add_argument("--mode", choices=("warm", "eviction", "both"), default="both")
    parser.add_argument("--repeats", type=int, default=3, help="fresh app/display runs per mode")
    parser.add_argument("--iterations", type=int, default=4, help="measured walks per app process")
    parser.add_argument("--warmup", type=int, default=1, help="unmeasured walks (at least one)")
    parser.add_argument("--timeout", type=float, default=120, help="total seconds per run including display startup")
    parser.add_argument("--step-timeout-ms", type=int, default=8000)
    parser.add_argument("--settle-ms", type=int, default=120)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this driver requires Linux's private graphics session; it never uses the host desktop")
    if (min(args.repeats, args.iterations, args.warmup, args.step_timeout_ms, args.settle_ms) < 1
            or not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("counts/timings must be positive and finite")
    app = args.app.resolve()
    if not app.is_file() or not os.access(app, os.X_OK):
        parser.error("--app must name an existing executable; build the native release first")
    if any(not os.environ.get(key) for key in GRAPHICS_ENV):
        parser.error("missing pinned graphics environment; run through nix develop .#native -c python3")
    output = args.output.resolve()
    os.umask(0o077)
    try:
        output.mkdir(parents=True, exist_ok=False, mode=0o700)
    except OSError:
        parser.error("--output must be a writable new directory (existing artifacts are never overwritten)")
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    summary = {
        "schemaVersion": 1, "complete": False,
        "scope": "isolated-empty-profile-route-transitions",
        "rendering": {
            "display": "private-weston-x11", "apiSelection": "opengl",
            "driverSelection": "Mesa llvmpipe", "softwareRendering": True,
        },
        "appSha256": None,
        "configuration": {key: getattr(args, key) for key in (
            "mode", "repeats", "iterations", "warmup", "timeout", "step_timeout_ms", "settle_ms")},
        "runs": [], "modes": {},
    }
    write_json(output / "summary.json", summary)
    try:
        with app.open("rb") as handle:
            summary["appSha256"] = hashlib.file_digest(handle, "sha256").hexdigest()
        modes = ("warm", "eviction") if args.mode == "both" else (args.mode,)
        for mode in modes:
            reference = None
            for repeat in range(1, args.repeats + 1):
                name = f"{mode}-{repeat:02d}.json"
                report = measure(app, output / name, mode == "eviction", args)
                if reference is not None and not REPORT.comparable(report, reference):
                    raise ValueError("rendering environment changed between repeats; refusing to aggregate")
                reference = report
                routes = REPORT.summarise(report)
                summary["runs"].append({
                    "report": name, "mode": mode,
                    "overallMs": sum(route["wallMs"] for route in routes.values()),
                    "routes": routes,
                    "idleMedianMs": statistics.median(report["idleGapsMs"]),
                    "idleMaxMs": max(report["idleGapsMs"]),
                    "environment": {key: report.get(key) for key in REPORT.COMPARISON_KEYS},
                })
                write_json(output / "summary.json", summary)
            runs = [run for run in summary["runs"] if run["mode"] == mode]
            totals = [run["overallMs"] for run in runs]
            summary["modes"][mode] = {
                "medianOverallMs": statistics.median(totals),
                "minOverallMs": min(totals), "maxOverallMs": max(totals),
            }
        summary["complete"] = True
        lines = ["# Native route profile", "",
                 "Fresh empty profiles on private Weston/Mesa llvmpipe, not hardware/display pacing.",
                 "Overall = sum of per-route median wall times within each app run.",
                 "Eviction requests app memory-pressure eviction, not a cold OS/disk cache.", "",
                 "| mode | runs | median overall | min | max |", "|---|---:|---:|---:|---:|"]
        for mode, totals in summary["modes"].items():
            lines.append(f"| {mode} | {args.repeats} | {totals['medianOverallMs']:.2f} ms | "
                         f"{totals['minOverallMs']:.2f} ms | {totals['maxOverallMs']:.2f} ms |")
        lines += ["", "Per-route timings, swap callbacks and idle timer noise are in summary.json.",
                  "No accounts, playback/provider pipeline, panel presentation, RSS or allocations are measured."]
        (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("\n".join(lines))
        return 0
    except Cancelled as error:
        summary["complete"] = False
        summary["failure"] = "cancelled"
        print("native profile: cancelled; owned child processes stopped", file=sys.stderr)
        return 128 + error.args[0]
    except (OSError, ValueError, TypeError) as error:
        summary["complete"] = False
        summary["failure"] = str(error)
        print(f"native profile: {error}", file=sys.stderr)
        return 1
    finally:
        write_json(output / "summary.json", summary)


if __name__ == "__main__":
    sys.exit(main())
