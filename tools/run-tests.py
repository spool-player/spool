#!/usr/bin/env python3
"""Run every traditional result before starting real GPU GUI e2e.

Native binaries isolate their selectors and retain JSON result journals. CTest
also owns external Python/shell consumer checks; neither phase short-circuits
when the other fails. --resume retains native failures and skips known crashes.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--retry-failed", action="store_true")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--config", help="CTest configuration for multi-configuration builds (for example Release)")
    args = parser.parse_args()
    if not 1 <= args.workers <= 32:
        parser.error("--workers must be between 1 and 32")
    if args.retry_failed and not args.resume:
        parser.error("--retry-failed requires --resume")
    build = args.build_dir.resolve()
    if not (build / "CTestTestfile.cmake").is_file():
        parser.error("build directory has no configured CTest suite")
    ctest = shutil.which(args.ctest)
    if ctest is None:
        parser.error("ctest executable is unavailable")
    environment = os.environ.copy()
    environment["SPOOL_TEST_RESUME"] = "1" if args.resume else "0"
    environment["SPOOL_TEST_RETRY_FAILED"] = "1" if args.retry_failed else "0"
    # External tools may run alongside the single native supervisor, but never
    # multiply native worker counts across directory-specific executables.
    environment["SPOOL_TEST_WORKERS"] = str(args.workers)
    state = {"format": 1, "buildDir": str(build), "phases": []}
    failed = False
    for phase in ("traditional", "e2e"):
        phase_environment = environment.copy()
        if phase == "e2e":
            # Native graphics lifecycles share the display/driver. Their actual
            # selectors remain isolated, but GPU work is deliberately serial.
            phase_environment["SPOOL_TEST_WORKERS"] = "1"
            phase_environment.pop("QT_QUICK_BACKEND", None)
        command = [ctest, "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
                   "--parallel", str(args.workers if phase == "traditional" else 1), "-L", f"^{phase}$"]
        if args.config:
            command.extend(["-C", args.config])
        if phase == "e2e" and sys.platform in ("darwin", "win32") and environment.get("GITHUB_ACTIONS") == "true":
            # A dedicated hosted CI display, never the developer's desktop.
            phase_environment["SPOOL_E2E_ISOLATED_DISPLAY"] = "1"
        # Only host Linux GUI tests use the private Weston/Xvfb session. Android
        # device CTest and tvOS simulator adapters own their native displays.
        is_device = (build / "spool-device-tests.json").is_file()
        if phase == "e2e" and sys.platform.startswith("linux") and not is_device:
            launcher = Path(__file__).resolve().with_name("test-gpu-session.sh")
            command = ["bash", str(launcher), *command]
        started = time.time()
        try:
            code = subprocess.run(command, env=phase_environment, check=False).returncode
        except OSError as error:
            print(f"{phase}: cannot start test command: {error}", file=sys.stderr)
            code = 2
        state["phases"].append({"phase": phase, "started": started, "finished": time.time(), "exitCode": code})
        failed |= code != 0
        path = build / "test-phases.json"
        temporary = path.with_suffix(".json.tmp")
        try:
            temporary.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
            temporary.replace(path)
        except OSError as error:
            failed = True
            print(f"{phase}: cannot retain phase journal: {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
