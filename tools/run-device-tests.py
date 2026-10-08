#!/usr/bin/env python3
"""Run the registered native selectors on an already booted isolated emulator.

CTest owns phase ordering. Every selector launches the same installed phase
bundle/APK; mobile OSes do not permit the desktop QProcess supervisor model.
A fresh, nonce-correlated native receipt is mandatory, never a console fallback.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid


class SelectorCrash(RuntimeError):
    """The launched consumer did not publish a final native result."""


class SelectorStartFailure(RuntimeError):
    """The platform rejected starting the native consumer."""


def command(arguments, *, check=True, timeout=30):
    return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, check=check, timeout=timeout)


def diagnostic(text):
    # Only controlled native test diagnostics. Never print provider/app payloads.
    markers = ("video result:", "orientation:", "viewport:", "first video frame",
               "render context handoff did not complete", "render context was not ready",
               "failed to initialize mpv", "tvOS audio smoke:", "tvOS credentials smoke:",
               "An error was encountered processing the command", "dyld[")
    for line in text.splitlines():
        if any(marker in line for marker in markers):
            print(re.sub(r"https?://\S+", "[redacted-url]", line))


class Android:
    def __init__(self, phase):
        self.bundle = "com.sachk.spool.tests" if phase == "traditional" else "com.sachk.spool.e2e_tests"
        self.adb = str(Path(os.environ["ANDROID_HOME"]) / "platform-tools" / "adb")
        if not os.environ.get("ANDROID_SERIAL", "").startswith("emulator-"):
            raise RuntimeError("ANDROID_SERIAL must identify the isolated emulator")
        self.call("shell", "run-as", self.bundle, "mkdir", "-p", "files")
        self.receipt = f"/data/user/0/{self.bundle}/files/spool-test-receipt.json"

    def call(self, *arguments, **options):
        return command([self.adb, *arguments], **options)

    def unlink(self, path):
        self.call("shell", "run-as", self.bundle, "rm", "-f", path)

    def read(self, path):
        process = self.call("exec-out", "run-as", self.bundle, "cat", path, check=False)
        if process.returncode:
            raise FileNotFoundError(path)
        return process.stdout

    def execute(self, arguments, timeout):
        self.call("shell", "am", "force-stop", self.bundle)
        # adb joins its shell argv; quote the complete extra so the remote shell
        # does not consume selector options as am options. Qt splits on spaces.
        extra = shlex.quote(" ".join(arguments))
        result = self.call("shell", "am", "start", "-W", "-n",
                           f"{self.bundle}/org.qtproject.qt.android.bindings.QtActivity",
                           "--es", "applicationArguments", extra, check=False)
        if result.returncode or "Error:" in result.stdout:
            raise SelectorStartFailure("Android rejected the native test activity launch")
        deadline = time.monotonic() + timeout
        # am start -W has already waited for the activity launch. A process
        # that dies before the first pid/receipt observation is still a crash,
        # not a reason to wait for the entire selector deadline.
        seen_process = True
        try:
            while time.monotonic() < deadline:
                try:
                    report = json.loads(self.read(self.receipt))
                    if (not isinstance(report, dict) or report.get("format") != 1 or
                            report.get("nonce") != arguments[-1]):
                        raise RuntimeError("Android native receipt has invalid nonce/schema")
                    if report.get("status") != "running":
                        return None, ""
                    seen_process = True
                except FileNotFoundError:
                    pass
                running = bool(self.call("shell", "pidof", self.bundle, check=False).stdout.strip())
                seen_process |= running
                if seen_process and not running:
                    # The process may have published its final receipt between
                    # our read and pidof. Re-read once; never relaunch the test.
                    try:
                        final = json.loads(self.read(self.receipt))
                    except FileNotFoundError as error:
                        raise SelectorCrash("native Android selector exited without a final receipt") from error
                    if (isinstance(final, dict) and final.get("nonce") == arguments[-1]
                            and final.get("status") != "running"):
                        return None, ""
                    raise SelectorCrash("native Android selector crashed without a final receipt")
                time.sleep(0.2)
            raise TimeoutError("native Android selector exceeded its deadline")
        finally:
            self.call("shell", "am", "force-stop", self.bundle, check=False)

    def capture(self, output, selector):
        # Screenshots contain only the isolated emulator test applications.
        shot = subprocess.run([self.adb, "exec-out", "screencap", "-p"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15)
        if shot.returncode == 0:
            (output / f"{selector}-failure.png").write_bytes(shot.stdout)


class TvOS:
    def __init__(self, phase):
        self.bundle = "com.sachk.spool.tests" if phase == "traditional" else "com.sachk.spool.e2e-tests"
        self.device = os.environ["SPOOL_TVOS_DEVICE"]
        self.container = Path(command(["xcrun", "simctl", "get_app_container", self.device,
                                       self.bundle, "data"]).stdout.strip())
        self.receipt = str(self.container / "tmp" / "spool-test-receipt.json")
        self.credentials = self.container / "tmp" / "tvos-credentials-result.json"

    def unlink(self, path):
        Path(path).unlink(missing_ok=True)

    def read(self, path):
        return Path(path).read_text()

    def execute(self, arguments, timeout):
        process = command(["xcrun", "simctl", "launch", "--console", "--terminate-running-process",
                           self.device, self.bundle, *arguments], check=False, timeout=timeout)
        if (process.returncode and
                "An error was encountered processing the command" in process.stdout and
                not Path(self.receipt).exists()):
            raise SelectorStartFailure("Apple simulator rejected the native consumer launch")
        return process.returncode, process.stdout

    def stop(self):
        command(["xcrun", "simctl", "terminate", self.device, self.bundle], check=False)

    def capture(self, output, selector):
        frame = self.container / "tmp" / "mpv-video-item-failure.png"
        if selector.startswith("mpv-video-item") and frame.exists():
            (output / f"{selector}-failure.png").write_bytes(frame.read_bytes())


def receipt(device, arguments, timeout, selector=None):
    nonce = uuid.uuid4().hex
    device.unlink(device.receipt)
    if selector == "tvos-credentials":
        device.unlink(device.credentials)
        # Existing native credentials consumer requires a positional nonce.
        arguments = [*arguments, nonce]
    arguments = [*arguments, "--receipt", device.receipt, "--nonce", nonce]
    launcher_code, text = device.execute(arguments, timeout)
    diagnostic(text)
    try:
        report = json.loads(device.read(device.receipt))
    except FileNotFoundError as error:
        raise SelectorCrash("native selector exited without a final receipt") from error
    if not isinstance(report, dict) or report.get("format") != 1 or report.get("nonce") != nonce:
        raise RuntimeError("missing, stale, or invalid native test receipt")
    if report.get("status") == "running":
        raise SelectorCrash("native selector crashed before its final receipt")
    if selector is None:
        names = report.get("selectors")
        if launcher_code not in (None, 0):
            raise RuntimeError("native selector enumeration launcher failed")
        if report.get("status") != "passed" or type(report.get("exitCode")) is not int or report["exitCode"] != 0:
            raise RuntimeError("native selector enumeration failed")
        if (not isinstance(names, list) or not names or
                any(not isinstance(name, str) or not re.fullmatch(r"[a-zA-Z0-9_-]+", name) for name in names) or
                len(set(names)) != len(names)):
            raise RuntimeError("native selector registry is empty or invalid")
        return names
    status = report.get("status")
    code = report.get("exitCode")
    if (report.get("selector") != selector or type(code) is not int or
            status not in ("passed", "failed", "skipped") or
            (status == "passed" and code != 0) or (status == "skipped" and code != 77) or
            (status == "failed" and code in (0, 77))):
        raise RuntimeError("native selector result is invalid")
    if launcher_code not in (None, 0, code):
        raise RuntimeError("native launcher exit disagrees with the consumer receipt")
    if selector == "tvos-credentials":
        nested = json.loads(device.read(device.credentials))
        expected = {"case": selector, "nonce": nonce, "saved": True, "loaded": True,
                    "removed": True, "writable": True}
        valid = (isinstance(nested, dict) and nested.keys() == expected.keys() and
                 nested.get("case") == selector and nested.get("nonce") == nonce and
                 all(type(nested.get(key)) is bool for key in ("saved", "loaded", "removed", "writable")))
        if not valid:
            raise RuntimeError("fresh native Keychain/sandbox receipt is missing or invalid")
        # A valid schema may report failure; preserve it, but never invent success.
        report["credentials"] = nested
        if nested != expected:
            report.update(status="failed", exitCode=1)
    return report


def app_journey(platform, output, timeout):
    # The controller is the SAME host e2e binary. It observes the actual mobile
    # application through its public automation transport and device screenshot,
    # not a host-rendered surrogate.
    path_string = os.environ.get("SPOOL_DEVICE_HOST_E2E", "")
    if not path_string:
        raise RuntimeError("SPOOL_DEVICE_HOST_E2E must name the built host e2e controller")
    executable = Path(path_string).resolve()
    nonce = uuid.uuid4().hex
    path = output / "app-journey-receipt.json"
    path.unlink(missing_ok=True)
    process = command([str(executable), "--child", "app-journey", "--device", platform,
                       "--receipt", str(path), "--nonce", nonce], check=False, timeout=max(timeout, 600))
    for line in process.stdout.splitlines():
        if line.startswith("app-journey:"):
            print(re.sub(r"https?://\S+", "[redacted-url]", line))
    try:
        report = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise SelectorCrash("host mobile journey exited without a final receipt") from error
    if isinstance(report, dict) and report.get("status") == "running":
        raise SelectorCrash("host mobile journey crashed before its final receipt")
    if (not isinstance(report, dict) or report.get("format") != 1 or report.get("nonce") != nonce or
            report.get("selector") != "app-journey" or report.get("status") not in ("passed", "failed") or
            type(report.get("exitCode")) is not int or report["exitCode"] != process.returncode or
            (report["status"] == "passed") != (process.returncode == 0)):
        raise RuntimeError("host mobile journey did not produce a fresh final native result")
    return report


def write_journal(path, value):
    temporary = path.with_name(path.name + ".tmp")
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
        stream.write(json.dumps(value, indent=2) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("android", "tvos"), required=True)
    parser.add_argument("--phase", choices=("traditional", "e2e"), required=True)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    output = args.artifact_dir.resolve()
    if args.platform == "android":
        output /= os.environ.get("SPOOL_ANDROID_FORM_FACTOR", "phone")
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / f"{args.phase}-result.json"
    device = Android(args.phase) if args.platform == "android" else TvOS(args.phase)
    names = receipt(device, ["--list"], args.timeout)
    if args.phase == "e2e":
        # Append after native GPU selectors; even their failures must not prevent
        # the real production application's account/playback journey from running.
        names.append("app-journey")
    resume = os.environ.get("SPOOL_TEST_RESUME") == "1"
    retry = os.environ.get("SPOOL_TEST_RETRY_FAILED") == "1"
    if retry and not resume:
        raise RuntimeError("retry-failed requires resume; previous results must remain intact")
    results = {}
    if resume and state_path.exists():
        previous = json.loads(state_path.read_text())
        if (not isinstance(previous, dict) or previous.get("platform") != args.platform or
                previous.get("selectors") != names or not isinstance(previous.get("results"), dict)):
            raise RuntimeError("resume journal belongs to another platform or selector registry")
        results = previous["results"]
        for name, result in results.items():
            if result.get("status") == "running":
                interrupted = {key: value for key, value in result.items() if key != "attempts"}
                interrupted.update(status="interrupted",
                                   reason="supervisor interrupted; native exit was not observed")
                interrupted.pop("exitCode", None)
                result.update(interrupted)
                result["attempts"] = result.get("attempts", []) + [interrupted]
            if result.get("status") == "crashed":
                result["resumeSkipped"] = True

    def persist():
        write_journal(state_path, {"platform": args.platform, "selectors": names, "results": results})

    persist()
    for name in names:
        # Supervisor interruption is unknown, not a product crash. Resume it
        # while retaining its attempt; only observed crashes stay resume-skipped.
        previous = results.get(name, {})
        resumable = previous.get("status") in ("pending", "interrupted")
        retriable = retry and previous.get("status") in ("failed", "timed-out", "start-failed")
        if previous and not (resumable or retriable):
            continue
        attempts = previous.get("attempts", [])
        started = time.time()
        results[name] = {"status": "running", "started": started, "attempts": attempts}
        persist()
        try:
            if name == "tvos-credentials":
                (output / "credentials-result.json").unlink(missing_ok=True)
            result = (app_journey(args.platform, output, args.timeout) if name == "app-journey"
                      else receipt(device, ["--child", name], args.timeout, name))
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, TimeoutError) as error:
            # Subprocess exception strings can contain full argv. Only controlled
            # errors are retained, never native stdout or command payloads.
            reason = str(error) if isinstance(error, (RuntimeError, TimeoutError)) else type(error).__name__
            status = ("crashed" if isinstance(error, SelectorCrash) else
                      "start-failed" if isinstance(error, SelectorStartFailure) else
                      "timed-out" if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)) else "failed")
            result = {"status": status, "exitCode": 1, "reason": reason}
            if isinstance(device, TvOS):
                try:
                    device.stop()
                except (OSError, subprocess.SubprocessError):
                    pass
        result.update(started=started, finished=time.time())
        result["attempts"] = attempts + [dict(result)]
        results[name] = result
        # Save the terminal result before optional diagnostic work can fail or
        # be interrupted. Each retry retains its earlier receipts and captures.
        persist()
        print(f"{args.phase}/{name}: {result['status']}", flush=True)
        if result["status"] not in ("passed", "skipped"):
            try:
                capture_directory = output / name / f"attempt-{len(result['attempts'])}"
                capture_directory.mkdir(parents=True, exist_ok=True)
                device.capture(capture_directory, name)
            except (OSError, subprocess.SubprocessError):
                print(f"{name}: failure capture unavailable", flush=True)
        if "credentials" in result:
            try:
                write_journal(output / "credentials-result.json", result["credentials"])
            except OSError:
                result["diagnosticFailure"] = True
                persist()
                print(f"{name}: credential receipt artifact unavailable", flush=True)
    return int(any(result.get("status") not in ("passed", "skipped") or result.get("diagnosticFailure")
                   for result in results.values()))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"device test infrastructure failed: {type(error).__name__}", flush=True)
        raise SystemExit(1)
