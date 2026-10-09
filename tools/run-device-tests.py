#!/usr/bin/env python3
"""Run the registered native selectors on an already booted isolated emulator.

CTest owns phase ordering. Every selector launches the same installed phase
bundle/APK; mobile OSes do not permit the desktop QProcess supervisor model.
A fresh, nonce-correlated native receipt is mandatory, never a console fallback.
"""
import argparse
from datetime import datetime
import hmac
import json
import os
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import signal
import socket
import socketserver
import sys
import time
import threading
import uuid


class SelectorCrash(RuntimeError):
    """The launched consumer did not publish a final native result."""


class SelectorStartFailure(RuntimeError):
    """The platform rejected starting the native consumer."""

def failure_result(error):
    status = ("crashed" if isinstance(error, SelectorCrash) else
              "start-failed" if isinstance(error, SelectorStartFailure) else
              "timed-out" if isinstance(error, (TimeoutError, subprocess.TimeoutExpired)) else "failed")
    reason = str(error) if isinstance(error, (RuntimeError, TimeoutError)) else type(error).__name__
    result = {"status": status, "exitCode": getattr(error, "exit_code", 1), "reason": reason}
    if hasattr(error, "signal"):
        result["signal"] = error.signal
    if hasattr(error, "cleanup"):
        result["cleanup"] = error.cleanup
    return result



def command(arguments, *, check=True, timeout=30, input=None, env=None):
    return subprocess.run(arguments, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True, check=check, timeout=timeout, input=input, env=env)


def diagnostic(text):
    # Only controlled native test diagnostics. Never print provider/app payloads.
    markers = ("video result:", "orientation:", "viewport:", "first video frame", "native presentation:",
               "render context handoff did not complete", "render context was not ready",
               "failed to initialize mpv", "tvOS audio smoke:", "tvOS credentials smoke:",
               "An error was encountered processing the command", "dyld[")
    for line in text.splitlines():
        if any(marker in line for marker in markers):
            print(re.sub(r"https?://\S+", "[redacted-url]", line))


def mpv_log_requested(arguments):
    return (os.environ.get("SPOOL_TEST_MPV_LOG") == "1" and len(arguments) >= 2 and
            arguments[0] == "--child" and
            re.fullmatch(r"mpv-video-item(?:-[a-zA-Z0-9_-]+)?", arguments[1]) is not None)


class Android:
    def __init__(self, phase):
        self.bundle = "com.sachk.spool.tests" if phase == "traditional" else "com.sachk.spool.e2e_tests"
        self.adb = str(Path(os.environ["ANDROID_HOME"]) / "platform-tools" / "adb")
        if not os.environ.get("ANDROID_SERIAL", "").startswith("emulator-"):
            raise RuntimeError("ANDROID_SERIAL must identify the isolated emulator")
        self.call("shell", "run-as", self.bundle, "mkdir", "-p", "files")
        self.receipt = f"/data/user/0/{self.bundle}/files/spool-test-receipt.json"

    def call(self, *arguments, **options):
        return command([self.adb, "-s", os.environ["ANDROID_SERIAL"], *arguments], **options)

    def unlink(self, path):
        self.call("shell", "run-as", self.bundle, "rm", "-f", path)

    def read(self, path):
        # Shell-v2 preserves the remote cat exit status; raw exec-out does not.
        process = self.call("shell", "-T", "run-as", self.bundle, "cat", path, check=False)
        if process.returncode:
            raise FileNotFoundError(path)
        return process.stdout
    def stop(self):
        self.call("shell", "am", "force-stop", self.bundle, check=False)


    def execute(self, arguments, timeout):
        self.call("shell", "am", "force-stop", self.bundle)
        # adb joins its shell argv; quote the complete extra so the remote shell
        # does not consume selector options as am options. Qt splits on spaces.
        extra = shlex.quote(" ".join(arguments))
        # Qt's debuggable-Activity-only extra, for this native renderer fixture
        # alone. No generic host environment forwarding or production flag.
        debug_extra = (["--es", "extraenvvars_SPOOL_TEST_MPV_LOG", "1"]
                       if mpv_log_requested(arguments) else [])
        result = self.call("shell", "am", "start", "-W", "-n",
                           f"{self.bundle}/org.qtproject.qt.android.bindings.QtActivity",
                           "--es", "applicationArguments", extra, *debug_extra, check=False)
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
        # Retain the actual native render buffer, not only the OS home screen
        # reached after the failed phase Activity has stopped.
        native_log = output / "native-output-private.log"
        if selector.startswith("mpv-video-item") and native_log.exists():
            for line in native_log.read_text().splitlines():
                if not line.startswith("native failure frame: "):
                    continue
                path = line.removeprefix("native failure frame: ")
                prefixes = (f"/data/user/0/{self.bundle}/", f"/data/data/{self.bundle}/")
                if (not path.startswith(prefixes) or "/../" in path or
                        not path.endswith("/mpv-video-item-failure.png")):
                    continue
                frame = subprocess.run(
                    [self.adb, "-s", os.environ["ANDROID_SERIAL"], "shell", "-T",
                     "run-as", self.bundle, "cat", path],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15)
                if frame.returncode == 0 and frame.stdout.startswith(b"\x89PNG\r\n\x1a\n"):
                    (output / f"{selector}-native-failure.png").write_bytes(frame.stdout)
                break
        # Screenshots contain only the isolated emulator test applications.
        shot = subprocess.run([self.adb, "-s", os.environ["ANDROID_SERIAL"], "exec-out", "screencap", "-p"],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15)
        if shot.returncode == 0:
            (output / f"{selector}-failure.png").write_bytes(shot.stdout)


NATIVE_DIAGNOSTIC_NAME = "native-crash-diagnostic.json"
NATIVE_REPORT_LIMIT = 1024 * 1024
NATIVE_FRAME_LIMIT = 64


def simulator_launch_pid(text, bundle):
    # simctl's launch line is separate from the consumer's redirected file
    # descriptors. Never search the app log or infer a PID from receipt data.
    matches = set(re.findall(r"^" + re.escape(bundle) + r":\s*([1-9][0-9]*)\s*$",
                             text, re.MULTILINE))
    return int(matches.pop()) if len(matches) == 1 else None


def ips_timestamp(value):
    if not isinstance(value, str):
        return None
    try:
        stamp = datetime.fromisoformat(value)
        return stamp.timestamp() if stamp.tzinfo is not None else None
    except ValueError:
        try:
            return datetime.strptime(value, "%Y-%m-%d %H:%M:%S.%f %z").timestamp()
        except ValueError:
            return None


def matching_simulator_report(path, launch):
    # Apple's .ips is metadata JSON followed by payload JSON, not one JSON
    # document. Raw strings are used only for private identity checks.
    if path.is_symlink() or path.stat().st_size > NATIVE_REPORT_LIMIT:
        return None
    if path.stat().st_mtime < launch["started"]:
        return None
    with path.open("rb") as stream:
        raw = stream.read(NATIVE_REPORT_LIMIT + 1)
    if len(raw) > NATIVE_REPORT_LIMIT:
        return None
    text = raw.decode("utf-8")
    decoder = json.JSONDecoder()
    metadata, boundary = decoder.raw_decode(text.lstrip())
    payload = json.loads(text.lstrip()[boundary:])
    if not isinstance(metadata, dict) or not isinstance(payload, dict):
        return None
    if type(payload.get("pid")) is not int or payload["pid"] != launch["pid"]:
        return None
    bundle_info = payload.get("bundleInfo")
    if not isinstance(bundle_info, dict) or bundle_info.get("CFBundleIdentifier") != launch["bundle"]:
        return None
    if metadata.get("bundleID", launch["bundle"]) != launch["bundle"]:
        return None
    proc_path = payload.get("procPath")
    if not isinstance(proc_path, str):
        return None
    app = Path(launch["appPath"])
    executable = launch.get("executable")
    if proc_path.startswith(launch["appPath"] + "/"):
        if launch["device"].lower() not in proc_path.lower().split("/"):
            return None
    else:
        # macOS replaces private procPath components with placeholders in IPS
        # reports. Bind that path to the installed Mach-O, not a basename alone.
        # https://developer.apple.com/documentation/xcode/interpreting-the-json-format-of-a-crash-report
        if (not any(part in ("USER", "*", "...", "<private>") for part in proc_path.split("/"))
                or payload.get("coalitionName") != "com.apple.CoreSimulator.SimDevice." + launch["device"]
                or not isinstance(executable, str)
                or not proc_path.endswith("/" + app.name + "/" + executable)):
            return None
        images = payload.get("usedImages")
        identities = launch.get("binaryIdentities", [])
        if (not isinstance(images, list) or not identities
                or not any(isinstance(image, dict) and image.get("source") == "P"
                           and image.get("name") == executable
                           and [str(image.get("uuid", "")).lower(), image.get("arch")] in identities
                           for image in images)):
            return None
    captured = ips_timestamp(payload.get("captureTime"))
    if captured is None or not launch["started"] <= captured <= launch["finished"]:
        return None
    return payload


def safe_native_diagnostic(value):
    # Explicit export schema: no arbitrary symbol names, paths, report labels,
    # termination descriptions, registers, raw stacks or application data.
    result = {"format": 1}
    if not isinstance(value, dict):
        raise ValueError("native crash diagnostic schema is not an object")
    for key in ("pid", "faultingThread"):
        if type(value.get(key)) is int and value[key] >= 0:
            result[key] = value[key]
    for key in ("launchStarted", "launchFinished"):
        if type(value.get(key)) in (int, float):
            result[key] = value[key]
    if value.get("collection") in ("matched", "unavailable", "launch-unidentified"):
        result["collection"] = value["collection"]
    stages = []
    source_stages = value.get("stages")
    for stage in (source_stages if isinstance(source_stages, list) else [])[:16]:
        if not isinstance(stage, dict):
            continue
        if stage.get("stage") not in ("selector-entry", "selector-return", "status-assigned",
                                      "receipt-handler-entry", "receipt-handler-return"):
            continue
        clean = {"stage": stage["stage"]}
        if stage["stage"] == "selector-return" and type(stage.get("exitCode")) is int:
            clean["exitCode"] = stage["exitCode"]
        if (stage["stage"] == "receipt-handler-return" and type(stage.get("saved")) is int
                and stage["saved"] in (0, 1)):
            clean["saved"] = int(stage["saved"])
        stages.append(clean)
    result["stages"] = stages
    for field, keys in (("exception", ("type", "signal", "codes")),
                        ("termination", ("code", "flags"))):
        source = value.get(field)
        if not isinstance(source, dict):
            continue
        clean = {key: source[key] for key in keys if key != "codes" and type(source.get(key)) is int}
        if field == "exception" and isinstance(source.get("codes"), list):
            clean["codes"] = [code for code in source["codes"][:8] if type(code) is int]
        result[field] = clean
    for field in ("frames", "lastExceptionFrames"):
        frames = []
        source_frames = value.get(field)
        for frame in (source_frames if isinstance(source_frames, list) else [])[:NATIVE_FRAME_LIMIT]:
            if (isinstance(frame, dict) and type(frame.get("imageIndex")) is int and
                    type(frame.get("imageOffset")) is int and
                    frame["imageIndex"] >= 0 and frame["imageOffset"] >= 0):
                frames.append({"imageIndex": frame["imageIndex"], "imageOffset": frame["imageOffset"]})
        result[field] = frames
    images = []
    source_images = value.get("images")
    for image in (source_images if isinstance(source_images, list) else [])[:NATIVE_FRAME_LIMIT * 2]:
        if (not isinstance(image, dict) or type(image.get("imageIndex")) is not int
                or image["imageIndex"] < 0):
            continue
        clean = {"imageIndex": image["imageIndex"]}
        identifier = image.get("uuid")
        if isinstance(identifier, str) and re.fullmatch(
                r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}", identifier):
            clean["uuid"] = str(uuid.UUID(identifier))
        if image.get("arch") in ("arm64", "arm64e", "arm64_32", "x86_64", "x86_64h", "arm", "i386"):
            clean["arch"] = image["arch"]
        images.append(clean)
    result["images"] = images
    return result


def native_report_fields(payload):
    # Darwin exception numbers and POSIX signal numbers, not free-form Apple
    # descriptions. Image offsets can be symbolicated against UUID-matched
    # build binaries privately; unreviewed function/symbol strings stay private.
    exception = payload.get("exception", {})
    exception = exception if isinstance(exception, dict) else {}
    types = {"EXC_BAD_ACCESS": 1, "EXC_BAD_INSTRUCTION": 2, "EXC_ARITHMETIC": 3,
             "EXC_EMULATION": 4, "EXC_SOFTWARE": 5, "EXC_BREAKPOINT": 6,
             "EXC_SYSCALL": 7, "EXC_MACH_SYSCALL": 8, "EXC_RPC_ALERT": 9,
             "EXC_CRASH": 10, "EXC_RESOURCE": 11, "EXC_GUARD": 12, "EXC_CORPSE_NOTIFY": 13}
    codes = exception.get("rawCodes", [])
    signal_name = exception.get("signal")
    number = getattr(signal, signal_name, None) if isinstance(signal_name, str) else None
    result = {"exception": {"codes": codes if isinstance(codes, list) else []},
              "termination": payload.get("termination", {})}
    if isinstance(exception.get("type"), str) and exception["type"] in types:
        result["exception"]["type"] = types[exception["type"]]
    if isinstance(number, signal.Signals):
        result["exception"]["signal"] = int(number)
    threads = payload.get("threads", [])
    faulting = payload.get("faultingThread")
    if isinstance(threads, list) and type(faulting) is int and 0 <= faulting < len(threads):
        result["faultingThread"] = faulting
        thread = threads[faulting]
        if isinstance(thread, dict) and isinstance(thread.get("frames"), list):
            result["frames"] = thread["frames"][:NATIVE_FRAME_LIMIT]
    backtrace = payload.get("lastExceptionBacktrace")
    if isinstance(backtrace, list):
        result["lastExceptionFrames"] = backtrace[:NATIVE_FRAME_LIMIT]
    indexes = {frame.get("imageIndex") for field in ("frames", "lastExceptionFrames")
               for frame in result.get(field, []) if isinstance(frame, dict)
               and type(frame.get("imageIndex")) is int}
    images = payload.get("usedImages", [])
    result["images"] = [dict(images[index], imageIndex=index) for index in sorted(indexes)
                        if isinstance(images, list) and 0 <= index < len(images)
                        and isinstance(images[index], dict)]
    return safe_native_diagnostic(result)


def native_stages(path):
    stages = []
    if not path.is_file() or path.is_symlink():
        return stages
    pattern = re.compile(r"^native stage: (selector-entry|selector-return|status-assigned|"
                         r"receipt-handler-entry|receipt-handler-return)"
                         r"(?: (exitCode=-?[0-9]+|saved=[01]))?\s*$")
    # Only the final bounded private log window is needed for return/atexit
    # breadcrumbs; renderer debug output must not make collection unbounded.
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size - NATIVE_REPORT_LIMIT))
        text = stream.read(NATIVE_REPORT_LIMIT).decode("utf-8", errors="replace")
        for line in text.splitlines():
            match = pattern.fullmatch(line)
            if match:
                stage = {"stage": match[1]}
                if match[2]:
                    key, number = match[2].split("=")
                    stage[key] = int(number)
                stages.append(stage)
                stages = stages[-16:]
    return stages


class TvOS:
    def __init__(self, phase):
        self.bundle = "com.sachk.spool.tests" if phase == "traditional" else "com.sachk.spool.e2e-tests"
        self.device = os.environ["SPOOL_TVOS_DEVICE"]
        self.container = Path(command(["xcrun", "simctl", "get_app_container", self.device,
                                       self.bundle, "data"]).stdout.strip())
        self.receipt = str(self.container / "tmp" / "spool-test-receipt.json")
        self.credentials = self.container / "tmp" / "tvos-credentials-result.json"
        self.app_path = command(["xcrun", "simctl", "get_app_container", self.device,
                                 self.bundle, "app"]).stdout.strip()
        with (Path(self.app_path) / "Info.plist").open("rb") as stream:
            metadata = plistlib.load(stream)
        self.executable = metadata["CFBundleExecutable"]
        if (metadata.get("CFBundleIdentifier") != self.bundle or
                not isinstance(self.executable, str) or
                not re.fullmatch(r"[a-zA-Z0-9_-]+", self.executable)):
            raise RuntimeError("installed simulator selector bundle identity is invalid")
        binary = command(["xcrun", "dwarfdump", "--uuid", str(Path(self.app_path) / self.executable)]).stdout
        self.binary_identities = [
            [str(uuid.UUID(identifier)), arch]
            for identifier, arch in re.findall(
                r"^UUID: ([0-9a-fA-F-]{36}) \((arm64|arm64e|x86_64|x86_64h)\) ", binary, re.MULTILINE)
        ]
        if not self.binary_identities:
            raise RuntimeError("installed simulator selector binary identity is unavailable")
        self.launch = None
        self.console = ""

    def unlink(self, path):
        Path(path).unlink(missing_ok=True)

    def read(self, path):
        return Path(path).read_text()

    def execute(self, arguments, timeout):
        self.launch = {"bundle": self.bundle, "device": self.device, "appPath": self.app_path,
                       "executable": self.executable, "binaryIdentities": self.binary_identities,
                       "started": time.time(), "pid": None,
                       "selector": arguments[1] if len(arguments) >= 2 and arguments[0] == "--child" else None,
                       "nonce": arguments[-1]}
        self.console = ""
        launch_env = dict(os.environ)
        launch_env.pop("SIMCTL_CHILD_SPOOL_TEST_MPV_LOG", None)
        if mpv_log_requested(arguments):
            launch_env["SIMCTL_CHILD_SPOOL_TEST_MPV_LOG"] = "1"
        try:
            process = command(["xcrun", "simctl", "launch", "--console", "--terminate-running-process",
                               self.device, self.bundle, *arguments], check=False, timeout=timeout, env=launch_env)
            self.console = process.stdout
        except subprocess.TimeoutExpired as error:
            captured = error.stdout or ""
            self.console = captured.decode("utf-8", errors="replace") if isinstance(captured, bytes) else captured
            raise
        finally:
            self.launch["finished"] = time.time()
            self.launch["pid"] = simulator_launch_pid(self.console, self.bundle)
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

    def collect_native_diagnostic(self, output, selector):
        if self.launch is None or self.launch["selector"] != selector:
            return
        write_journal(output / "native-launch-private.json", self.launch)
        descriptor = os.open(output / "simctl-console-private.log",
                             os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(self.console)
        value = {"launchStarted": self.launch["started"], "launchFinished": self.launch["finished"],
                 "stages": native_stages(Path(self.receipt + ".log")),
                 "collection": "unavailable"}
        if self.launch["pid"] is None:
            value["collection"] = "launch-unidentified"
        else:
            value["pid"] = self.launch["pid"]
            # Delayed OS report publication is bounded independently of the
            # selector timeout. No retries/relaunches, and no cleanup-generated
            # crash can match the pre-cleanup captureTime window.
            reports = (Path.home() / "Library/Logs/DiagnosticReports", Path("/Library/Logs/DiagnosticReports"))
            deadline = time.monotonic() + 5
            examined = set()
            parsed = 0
            while time.monotonic() < deadline and parsed < 128:
                for path in (path for root in reports for path in root.glob(self.executable + "*.ips")):
                    if time.monotonic() >= deadline or parsed >= 128:
                        break
                    try:
                        stat = path.stat()
                        identity = (path, stat.st_mtime_ns, stat.st_size)
                        if identity in examined or stat.st_mtime < self.launch["started"]:
                            continue
                        examined.add(identity)
                        parsed += 1
                        payload = matching_simulator_report(path, self.launch)
                    except (OSError, ValueError, UnicodeError):
                        continue
                    if payload is not None:
                        value.update(native_report_fields(payload))
                        value.update(collection="matched",
                                     stages=native_stages(Path(self.receipt + ".log")))
                        write_journal(output / NATIVE_DIAGNOSTIC_NAME, safe_native_diagnostic(value))
                        return
                time.sleep(min(0.25, max(0, deadline - time.monotonic())))
        write_journal(output / NATIVE_DIAGNOSTIC_NAME, safe_native_diagnostic(value))


def receipt(device, arguments, timeout, selector=None, output=None):
    nonce = uuid.uuid4().hex
    if isinstance(device, TvOS):
        # Failures before execute must never reuse the previous selector/list PID.
        device.launch = None
        device.console = ""
    device.unlink(device.receipt)
    log_path = device.receipt + ".log"
    device.unlink(log_path)
    if selector == "tvos-credentials":
        device.unlink(device.credentials)
        # Existing native credentials consumer requires a positional nonce.
        arguments = [*arguments, nonce]
    arguments = [*arguments, "--log", log_path, "--receipt", device.receipt, "--nonce", nonce]
    launcher_code, text = device.execute(arguments, timeout)
    try:
        text = device.read(log_path)
    except FileNotFoundError:
        if output is not None:
            raise RuntimeError("native selector did not retain its requested private diagnostic log")
    if output is not None:
        descriptor = os.open(output / "native-output-private.log", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as log:
            log.write(text)
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


class JourneyOwner:
    """Adapter-owned resources; the disposable host controller never owns cleanup."""

    def __init__(self, platform, directory, nonce, regression=None):
        if os.environ.get("SPOOL_E2E_ISOLATED_DEVICE") != "1":
            raise RuntimeError("mobile journey requires a driver-owned isolated emulator")
        self.platform = platform
        self.directory = directory
        self.nonce = nonce
        self.regression = regression
        self.ready = threading.Event()
        self.bundle = (os.environ.get("SPOOL_E2E_TVOS_BUNDLE", "com.sachk.spool")
                       if platform == "tvos" else "com.sachk.spool")
        self.device = os.environ["ANDROID_SERIAL" if platform == "android" else "SPOOL_TVOS_DEVICE"]
        if platform == "android" and not self.device.startswith("emulator-"):
            raise RuntimeError("Android journey requires an isolated emulator serial")
        self.tool = (os.environ.get("SPOOL_E2E_ADB") or
                     str(Path(os.environ["ANDROID_HOME"]) / "platform-tools" / "adb")
                     if platform == "android" else os.environ.get("SPOOL_E2E_XCRUN", "xcrun"))
        self.process = None
        self.pending_launch = None
        self.mappings = []
        self.app_path = None
        self.artifact_failure = False
        if platform == "tvos":
            self.app_path = self.call("get_app_container", self.device, self.bundle, "app").stdout.strip()
            if not Path(self.app_path).is_dir():
                raise RuntimeError("installed production simulator application is unavailable")
        self.persist()

    def call(self, *arguments, **options):
        prefix = [self.tool, "-s", self.device] if self.platform == "android" else [self.tool, "simctl"]
        return command([*prefix, *arguments], **options)

    def persist(self):
        # Intent is durable BEFORE each mutating OS command, including its exact
        # endpoint. No capability, provider payload or product log belongs here.
        write_journal(self.directory / "ownership-private.json",
                      {"format": 1, "nonce": self.nonce, "platform": self.platform,
                       "device": self.device, "bundle": self.bundle, "process": self.process,
                       "pendingLaunch": self.pending_launch, "mappings": self.mappings})

    def running_pid(self):
        if self.platform == "android":
            result = self.call("shell", "pidof", "-s", self.bundle, check=False)
            text = result.stdout.strip()
            if result.returncode and self.call("get-state").stdout.strip() != "device":
                raise RuntimeError("Android process observation requires a connected emulator")
            return int(text) if text.isdecimal() else None
        listing = self.call("spawn", self.device, "launchctl", "list").stdout
        for line in listing.splitlines():
            fields = line.split()
            if (len(fields) == 3 and fields[0].isdecimal() and
                    fields[2].startswith(f"UIKitApplication:{self.bundle}[")):
                return int(fields[0])
        return None

    def identity(self, pid):
        if self.platform == "android":
            result = self.call("shell", "-T", "run-as", self.bundle, "cat", f"/proc/{pid}/stat", check=False)
            if result.returncode:
                if self.call("get-state").stdout.strip() != "device":
                    raise RuntimeError("Android process observation requires a connected emulator")
                exists = self.call("shell", "run-as", self.bundle, "test", "-d", f"/proc/{pid}", check=False)
                if exists.returncode != 1:
                    raise RuntimeError("owned Android process birth identity could not be read")
                return None
            fields = result.stdout.rsplit(") ", 1)
            if len(fields) != 2 or len(fields[1].split()) < 20:
                raise RuntimeError("Android process birth identity is unavailable")
            return fields[1].split()[19]  # Linux stat field 22, starttime.
        result = command(["ps", "-p", str(pid), "-o", "lstart=", "-o", "command="],
                         check=False, env=dict(os.environ, LC_ALL="C"))
        if not result.stdout.strip():
            return None
        # Simulator PIDs are host PIDs. Never signal an unrelated/recycled PID.
        text = result.stdout.strip()
        if self.app_path + "/" not in text:
            return None  # The original app is gone; a recycled PID is not ours.
        return text[:24]

    def mappings_now(self, kind):
        rows = self.call(kind, "--list").stdout.splitlines()
        return [tuple(row.split()) for row in rows if len(row.split()) == 3]

    def map(self, kind, local, remote):
        if any(row[1] == local for row in self.mappings_now(kind)):
            raise RuntimeError("journey mapping endpoint is already owned by another consumer")
        mapping = {"kind": kind, "local": local, "remote": remote, "nonce": self.nonce}
        self.mappings.append(mapping)
        self.persist()
        # --no-rebind closes the observation/creation race without replacing
        # another consumer's endpoint. Retain uncertain intents for cleanup.
        result = self.call(kind, "--no-rebind", local, remote, check=False)
        if result.returncode:
            # A definitive rejection created nothing. In particular, do not
            # remove a racing consumer's endpoint even if its tuple matches.
            self.mappings.remove(mapping)
            self.persist()
            raise RuntimeError("OS rejected creation of the owned journey mapping")

    def recover_launch(self):
        if self.pending_launch is not None and self.process is None:
            pid = self.running_pid()
            if pid:
                birth = self.identity(pid)
                if birth is None:
                    return
                if self.platform == "android" and int(birth) < self.pending_launch["minimumBirth"]:
                    raise RuntimeError("refusing to own an Android process predating the launch")
                if (self.platform == "tvos" and
                        time.mktime(time.strptime(birth, "%a %b %d %H:%M:%S %Y")) <
                        self.pending_launch["started"] - 1):
                    raise RuntimeError("refusing to own a simulator process predating the launch")
                self.process = {"pid": pid, "birth": birth, "nonce": self.nonce}
                self.persist()

    def stop_product(self):
        self.recover_launch()
        if self.process is not None:
            pid = self.process["pid"]
            if self.identity(pid) == self.process["birth"]:
                if self.platform == "android":
                    self.call("shell", "run-as", self.bundle, "kill", "-9", str(pid), check=False)
                else:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                deadline = time.monotonic() + 10
                while self.identity(pid) == self.process["birth"] and time.monotonic() < deadline:
                    time.sleep(0.1)
                if self.identity(pid) == self.process["birth"]:
                    raise RuntimeError("owned production app survived cleanup")
        self.process = None
        self.pending_launch = None
        self.persist()

    def cleanup(self):
        errors = ["artifact-retain"] if self.artifact_failure else []
        pid = self.process["pid"] if self.process else None
        try:
            self.recover_launch()
            pid = self.process["pid"] if self.process else None
            self.stop_product()
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
            errors.append("product-stop")
        for mapping in list(self.mappings):
            try:
                rows = self.mappings_now(mapping["kind"])
                matching = [row for row in rows if row[1] == mapping["local"]]
                if matching:
                    if (len(matching) != 1 or matching[0][2] != mapping["remote"] or
                            (mapping["kind"] == "forward" and matching[0][0] != self.device)):
                        raise RuntimeError("mapping ownership changed; refusing unrelated removal")
                    self.call(mapping["kind"], "--remove", mapping["local"])
                if any(row[1] == mapping["local"] for row in self.mappings_now(mapping["kind"])):
                    raise RuntimeError("owned mapping survived cleanup")
                self.mappings.remove(mapping)
                self.persist()
            except (OSError, RuntimeError, subprocess.SubprocessError):
                errors.append("mapping-remove")
        result = {"format": 1, "nonce": self.nonce, "platform": self.platform,
                  "pid": pid, "productGone": self.process is None and self.pending_launch is None,
                  "ownedMappingsGone": not self.mappings, "errors": errors}
        try:
            write_journal(self.directory / "cleanup-result.json", result)
        except OSError:
            errors.append("journal-write")
        return result

    def dispatch(self, operation, value):
        if operation == "prepare":
            if self.running_pid():
                raise RuntimeError("production bundle is already running; refusing unrelated app cleanup")
            if self.platform == "android":
                if "Success" not in self.call("shell", "pm", "clear", self.bundle).stdout:
                    raise RuntimeError("isolated production application sandbox could not be reset")
                port = value["reversePort"]
                if type(port) is not int or not 0 < port < 65536:
                    raise RuntimeError("invalid journey fixture endpoint")
                self.map("reverse", f"tcp:{port}", f"tcp:{port}")
            return {}
        if operation == "launch":
            if self.process is not None or self.running_pid():
                raise RuntimeError("production bundle is already running")
            arguments = value["arguments"]
            if (not isinstance(arguments, list) or not arguments or
                    any(not isinstance(arg, str) for arg in arguments)):
                raise RuntimeError("invalid production launch arguments")
            epoch = 0
            if self.platform == "android":
                epoch = int(self.call("shell", "date", "+%s").stdout.strip())
                uptime = self.call("shell", "cat", "/proc/uptime").stdout.split()[0]
                ticks = self.call("shell", "getconf", "CLK_TCK").stdout.strip()
                self.pending_launch = {"minimumBirth": int(float(uptime) * int(ticks))}
            else:
                self.pending_launch = {"started": time.time()}
            self.persist()
            if self.platform == "android":
                launched = self.call("shell", "am", "start", "-W", "-n",
                                     f"{self.bundle}/com.sachk.spool.SpoolActivity", "--es",
                                     "applicationArguments", shlex.quote(" ".join(arguments)))
                if "Error:" in launched.stdout:
                    raise SelectorStartFailure("Android rejected the production activity launch")
            else:
                launched = self.call("launch", self.device, self.bundle, *arguments)
                match = re.search(r":\s*([0-9]+)\s*$", launched.stdout)
                if match:
                    pid = int(match[1])
                    birth = self.identity(pid)
                    if birth is not None:
                        self.process = {"pid": pid, "birth": birth, "nonce": self.nonce}
                        self.persist()
            self.recover_launch()
            if self.process is None:
                raise RuntimeError("production app launch did not expose its OS process identity")
            return {"pid": self.process["pid"], "epoch": epoch}
        if operation == "forward":
            if self.platform != "android" or self.process is None:
                raise RuntimeError("forwarding requires an owned Android production process")
            port = value["port"]
            if type(port) is not int or not 0 < port < 65536:
                raise RuntimeError("invalid production automation endpoint")
            # Choose a known endpoint before adb runs; --no-rebind is the final
            # arbiter if another consumer binds it after this reservation closes.
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                local = reservation.getsockname()[1]
            self.map("forward", f"tcp:{local}", f"tcp:{port}")
            return {"port": local}
        if operation == "stop":
            self.stop_product()
            return {}
        if operation == "cleanup":
            try:
                source = Path(value["artifacts"]).resolve()
                if (source.parent != self.directory.resolve() or
                        not source.name.startswith("app-journey-")):
                    raise RuntimeError("journey image source escaped its private attempt")
                retained = self.directory / "journey-images" / source.name
                retained.mkdir(parents=True, exist_ok=True, mode=0o700)
                for name in ("frame.png", "library-text.png", "mobile-default-startup.png", "download-failure.png"):
                    image = source / name
                    if image.is_file() and not image.is_symlink():
                        (retained / name).write_bytes(image.read_bytes())
                if (self.platform == "android" and (source / "download-failure.png").is_file()
                        and self.process is not None
                        and self.identity(self.process["pid"]) == self.process["birth"]):
                    ledger = self.call("shell", "-T", "run-as", self.bundle, "sh", "-c",
                                       "'test $(wc -c < files/downloads.json) -le 16777216 && cat files/downloads.json'",
                                       check=False)
                    if ledger.returncode == 0:
                        descriptor = os.open(self.directory / "downloads-private.json",
                                             os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                            stream.write(ledger.stdout)
            except (KeyError, OSError, RuntimeError):
                self.artifact_failure = True
            result = self.cleanup()
            if result["errors"]:
                raise RuntimeError("adapter could not retire every journey resource")
            return result
        if operation == "ready":
            if self.regression:
                control = os.environ.get("SPOOL_E2E_SPOOLET")
                if not control:
                    raise RuntimeError("cleanup regression requires SPOOL_E2E_SPOOLET")
                descriptor = Path(value["descriptor"])
                if not descriptor.resolve().is_relative_to(self.directory.resolve()):
                    raise RuntimeError("controller descriptor escaped its private attempt directory")
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    response = command([control, "--descriptor", str(descriptor),
                                        "--timeout", "5000", "state"], check=False, timeout=10)
                    envelope = json.loads(response.stdout)
                    if envelope.get("ok") and envelope.get("result", {}).get("initialized"):
                        self.ready.set()
                        return {}
                    time.sleep(0.1)
                raise RuntimeError("real mobile public controller did not initialize for cleanup regression")
            return {}
        raise RuntimeError("unknown adapter ownership operation")


class JourneyService(socketserver.TCPServer):
    def __init__(self, owner):
        self.owner = owner
        self.token = uuid.uuid4().hex + uuid.uuid4().hex
        self.regression_peer = None
        self.incomplete_frame = threading.Event()
        super().__init__(("127.0.0.1", 0), JourneyRequest)
    def handle_error(self, request, client_address):
        # No traceback/request payload may disclose the bearer capability.
        pass



class JourneyRequest(socketserver.StreamRequestHandler):
    def handle(self):
        deadline = time.monotonic() + 5
        frame = bytearray()
        try:
            while len(frame) < 65536:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                self.connection.settimeout(remaining)
                data = self.connection.recv(min(4096, 65536 - len(frame)))
                if not data or time.monotonic() >= deadline:
                    return
                frame.extend(data)
                separator = frame.find(b"\n")
                if separator >= 0:
                    if separator != len(frame) - 1:
                        return
                    request = json.loads(frame[:-1])
                    break
                if self.client_address == self.server.regression_peer:
                    self.server.incomplete_frame.set()
            else:
                return
        except (OSError, ValueError):
            return
        self.connection.settimeout(5)
        if (not isinstance(request, dict) or request.get("nonce") != self.server.owner.nonce or
                not isinstance(request.get("token"), str) or
                not hmac.compare_digest(request["token"], self.server.token)):
            return
        try:
            result = self.server.owner.dispatch(request["operation"], request.get("value", {}))
            response = {"ok": True, "result": result}
        except (OSError, KeyError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
            # Exception text/argv and OS output may contain private values.
            response = {"ok": False, "error": type(error).__name__}
        try:
            self.wfile.write(json.dumps(response).encode() + b"\n")
        except OSError:
            # Host death cannot cancel the already-recorded OS operation.
            pass

class SlowRequest:
    """A real unauthenticated drip peer, bounded even against the old reader."""

    def __init__(self, service):
        self.closed = False
        self.elapsed_ms = None
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        service.regression_peer = self.socket.getsockname()
        service.incomplete_frame.clear()
        self.socket.settimeout(5)
        self.socket.connect(service.server_address)
        self.started = time.monotonic()
        self.socket.sendall(b"{")
        self.thread = threading.Thread(target=self.observe, daemon=True)
        self.thread.start()
        if not service.incomplete_frame.wait(5):
            self.thread.join(9)
            raise RuntimeError("slow request was not admitted at the real adapter socket boundary")

    def observe(self):
        next_byte = self.started + 1
        try:
            while time.monotonic() - self.started < 8:
                self.socket.settimeout(0.1)
                try:
                    data = self.socket.recv(1)
                    if not data:
                        self.closed = True
                        break
                except socket.timeout:
                    pass
                if time.monotonic() >= next_byte:
                    self.socket.sendall(b" ")
                    next_byte += 1
        except OSError:
            self.closed = True
        finally:
            self.elapsed_ms = int((time.monotonic() - self.started) * 1000)
            self.socket.close()

    def result(self):
        self.thread.join(9)
        return {"slowClientRejected": self.closed and self.elapsed_ms is not None and self.elapsed_ms <= 7000,
                "slowClientCloseElapsedMs": self.elapsed_ms}



def app_journey(platform, output, timeout, regression=None):
    # The SAME host e2e controller drives the real mobile product. Only the
    # adapter owns the product/mappings, so killing that controller is safe.
    path_string = os.environ.get("SPOOL_DEVICE_HOST_E2E", "")
    if not path_string:
        raise RuntimeError("SPOOL_DEVICE_HOST_E2E must name the built host e2e controller")
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    nonce = uuid.uuid4().hex
    path = output / "app-journey-receipt.json"
    owner = JourneyOwner(platform, output, nonce, regression)
    service = JourneyService(owner)
    capability = output / "adapter-capability-private.json"
    write_journal(capability, {"format": 1, "platform": platform, "nonce": nonce,
                              "port": service.server_address[1], "token": service.token})
    environment = dict(os.environ, SPOOL_E2E_DEVICE_JOURNEY_OWNER=str(capability),
                       SPOOL_E2E_ARTIFACT_DIR=str(output))
    thread = threading.Thread(target=service.serve_forever, daemon=True)
    thread.start()
    process = None
    cleanup = None
    log = None
    started = time.time()
    injected = False
    slow_request = None
    try:
        descriptor = os.open(output / "host-controller-private.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        log = os.fdopen(descriptor, "wb")
        process = subprocess.Popen([str(Path(path_string).resolve()), "--child", "app-journey",
                                    "--device", platform, "--receipt", str(path), "--nonce", nonce],
                                   stdout=log, stderr=subprocess.STDOUT, env=environment)
        deadline = time.monotonic() + max(timeout, 600)
        while process.poll() is None:
            if regression and owner.ready.is_set() and not injected:
                # Stop only this controller after genuine public readiness, so
                # the slow peer cannot prevent the journey reaching its boundary.
                os.kill(process.pid, signal.SIGSTOP)
                slow_request = SlowRequest(service)
                injected = True
                if regression == "timeout":
                    # Exercise the actual timeout/kill path after observing a
                    # live production app through authenticated public spoolet.
                    deadline = time.monotonic() + 1
                else:
                    process.kill()
            if time.monotonic() >= deadline:
                process.kill()
                process.wait()
                error = TimeoutError("host mobile journey exceeded its deadline")
                error.exit_code = process.returncode
                if process.returncode < 0:
                    error.signal = -process.returncode
                raise error
            time.sleep(0.05)
        if process.returncode < 0:
            error = SelectorCrash("host mobile journey terminated by an OS signal")
            error.exit_code = process.returncode
            error.signal = -process.returncode
            raise error
        if regression and not injected:
            raise RuntimeError("controller exited before the real cleanup regression boundary")
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
    finally:
        active_error = sys.exc_info()[1]
        if process is not None and process.poll() is None:
            process.kill()
            process.wait()
        # Wait for any in-flight OS mutation before retiring its recorded intent.
        service.shutdown()
        thread.join()
        service.server_close()
        cleanup = owner.cleanup()
        if active_error is not None:
            active_error.cleanup = cleanup
        if log is not None:
            log.close()
        try:
            capability.unlink(missing_ok=True)
        except OSError:
            cleanup["errors"].append("capability-unlink")
        outcome = failure_result(active_error) if active_error is not None else dict(report)
        outcome.update(format=1, nonce=nonce, selector="app-journey", platform=platform,
                       started=started, finished=time.time(), cleanup=cleanup,
                       publicControllerReady=owner.ready.is_set(), faultInjected=injected)
        if slow_request is not None:
            outcome.update(slow_request.result())
        if process is not None:
            outcome.update(controllerPid=process.pid, controllerExitCode=process.returncode)
        try:
            write_journal(output / "controller-result.json", outcome)
            write_journal(output / "cleanup-result.json", cleanup)
        except OSError:
            cleanup["errors"].append("journal-write")
    if cleanup["errors"]:
        error = RuntimeError("adapter could not retire every journey resource")
        error.cleanup = cleanup
        raise error
    report["cleanup"] = cleanup
    return report


class MappingConsumer:
    """Independent real Android reverse consumer, kept alive across host failure."""

    def __init__(self, device, directory):
        self.device = device
        self.token = uuid.uuid4().hex
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(20)
        self.port = self.listener.getsockname()[1]
        self.mapping = f"tcp:{self.port}"
        private = directory / "independent-consumer-private"
        private.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.owner = JourneyOwner("android", private, uuid.uuid4().hex)

    def __enter__(self):
        try:
            self.owner.map("reverse", self.mapping, self.mapping)
            return self
        except (OSError, RuntimeError, subprocess.SubprocessError):
            self.owner.cleanup()
            self.listener.close()
            raise

    def probe(self):
        failures = []

        def respond():
            try:
                connection, _ = self.listener.accept()
                with connection, connection.makefile("rb") as stream:
                    connection.settimeout(5)
                    if stream.readline(129).strip() != self.token.encode():
                        raise RuntimeError("independent mapping request nonce mismatch")
                    connection.sendall(self.token.encode() + b"\n")
            except (OSError, RuntimeError) as error:
                failures.append(type(error).__name__)

        worker = threading.Thread(target=respond, daemon=True)
        worker.start()
        process = None
        try:
            # nc half-closes on stdin EOF, which adb forwarding translates to
            # closing the whole peer. Keep stdin open until the real server has
            # returned its nonce and closed; communicate(input=...) cannot do so.
            process = subprocess.Popen(
                [self.device.adb, "-s", os.environ["ANDROID_SERIAL"], "shell", "-T",
                 "toybox", "nc", "-w", "3", "127.0.0.1", str(self.port)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            process.stdin.write((self.token + "\n").encode())
            process.stdin.flush()
            process.wait(timeout=10)
            response = process.stdout.read(129)
        finally:
            if process is not None:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
                process.stdout.close()
            worker.join(20)
        if (worker.is_alive() or failures or process.returncode != 0 or
                response != (self.token + "\n").encode()):
            raise RuntimeError("independent real emulator mapping consumer stopped working")

    def __exit__(self, *_):
        self.listener.close()
        if self.owner.cleanup()["errors"]:
            raise RuntimeError("independent consumer mapping cleanup failed")


def cleanup_regression(platform, device, output, timeout, mode):
    expected = "timed-out" if mode == "timeout" else "crashed"

    def exercise():
        try:
            app_journey(platform, output, timeout, regression=mode)
        except (SelectorCrash, TimeoutError) as error:
            observed = "crashed" if isinstance(error, SelectorCrash) else "timed-out"
            cleanup = error.cleanup
            failure = json.loads((output / "controller-result.json").read_text())
            if (observed != expected or failure.get("status") != expected or
                    failure.get("nonce") != cleanup["nonce"] or not failure.get("faultInjected") or
                    not failure.get("publicControllerReady")):
                raise RuntimeError("cleanup regression did not reach its required real-controller fault boundary")
            if not failure.get("slowClientRejected"):
                raise RuntimeError("slow unauthenticated peer exceeded the bounded request-frame retirement deadline")
        else:
            raise RuntimeError("cleanup regression did not observe its required host failure")
        write_journal(output / "cleanup-result.json", cleanup)
        if (not cleanup["productGone"] or not cleanup["ownedMappingsGone"] or cleanup["errors"] or
                type(cleanup["pid"]) is not int or cleanup["pid"] <= 0):
            raise RuntimeError("host failure left owned production resources alive")
        # An independently installed native consumer must still launch and
        # publish its actual nonce-correlated public result after cleanup.
        receipt(device, ["--list"], timeout)
        return {"format": 1, "nonce": cleanup["nonce"], "status": "passed", "exitCode": 0,
                "failure": failure, "cleanup": cleanup, "otherConsumerWorks": True}

    if platform == "android":
        with MappingConsumer(device, output) as consumer:
            consumer.probe()
            result = exercise()
            consumer.probe()
        return result
    return exercise()


def safe_result(value):
    # Schema construction, never recursive copying of arbitrary native JSON.
    result = {}
    for key in ("format", "exitCode", "signal", "pid", "controllerPid", "controllerExitCode",
                "slowClientCloseElapsedMs"):
        if type(value.get(key)) is int:
            result[key] = value[key]
    for key in ("started", "finished"):
        if type(value.get(key)) in (int, float):
            result[key] = value[key]
    for key in ("productGone", "ownedMappingsGone", "otherConsumerWorks",
                "safeExportVerified", "diagnosticFailure", "resumeSkipped",
                "publicControllerReady", "faultInjected", "slowClientRejected"):
        if type(value.get(key)) is bool:
            result[key] = value[key]
    if isinstance(value.get("nonce"), str) and re.fullmatch("[a-f0-9]{32}", value["nonce"]):
        result["nonce"] = value["nonce"]
    if value.get("platform") in ("android", "tvos"):
        result["platform"] = value["platform"]
    if isinstance(value.get("selector"), str) and re.fullmatch("[a-zA-Z0-9_-]+", value["selector"]):
        result["selector"] = value["selector"]
    if value.get("status") in ("pending", "running", "passed", "failed", "skipped", "crashed",
                               "timed-out", "start-failed", "interrupted"):
        result["status"] = value["status"]
    if isinstance(value.get("errors"), list):
        result["errors"] = [error for error in value["errors"]
                            if error in ("product-stop", "mapping-remove", "journal-write",
                                         "capability-unlink", "artifact-retain")]
    for key in ("cleanup", "failure"):
        if isinstance(value.get(key), dict):
            result[key] = safe_result(value[key])
    if isinstance(value.get("attempts"), list):
        result["attempts"] = [safe_result(attempt) for attempt in value["attempts"] if isinstance(attempt, dict)]
    return result


def export_attempt(private, destination, result):
    destination.mkdir(parents=True, exist_ok=True)
    write_journal(destination / "native-result.json", safe_result(result))
    controller = private / "controller-result.json"
    if controller.is_file() and not controller.is_symlink():
        write_journal(destination / "controller-result.json", safe_result(json.loads(controller.read_text())))
    diagnostic_receipt = private / NATIVE_DIAGNOSTIC_NAME
    if diagnostic_receipt.is_file() and not diagnostic_receipt.is_symlink():
        write_journal(destination / NATIVE_DIAGNOSTIC_NAME,
                      safe_native_diagnostic(json.loads(diagnostic_receipt.read_text())))
    # Only named screenshots at the journey's top level. Never traverse app
    # data roots, registry descriptors, OCR text or raw product/controller logs.
    images = list(private.glob("*-failure.png"))
    journeys = [*private.glob("app-journey-*"), *(private / "journey-images").glob("app-journey-*")]
    for journey in journeys:
        if journey.is_dir() and not journey.is_symlink():
            images.extend(journey / name for name in
                          ("frame.png", "library-text.png", "mobile-default-startup.png", "download-failure.png"))
    for image in images:
        if image.is_file() and not image.is_symlink():
            data = image.read_bytes()
            if data.startswith(b"\x89PNG\r\n\x1a\n"):
                relative = (Path(image.parent.name) / image.name if image.parent.name.startswith("app-journey-")
                            else image.relative_to(private))
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
    # Generated controlled attempt log; never copy child stdout/stderr.
    safe = safe_result(result)
    lines = [f"status={safe.get('status', 'invalid')}", f"exitCode={safe.get('exitCode', 'unknown')}"]
    log = private / "host-controller-private.log"
    if log.is_file() and not log.is_symlink():
        pattern = re.compile(
            r"^app-journey: production spoolet rejected ([a-z_-]{1,32}) "
            r"\(exit=(-?[0-9]+), code=([a-z_]{1,64}), elapsed_ms=([0-9]+), "
            r"timeout_origin=(unknown|server|client_write|client_response)\)")
        with log.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = pattern.match(line)
                if match:
                    operation, code, error, elapsed, origin = match.groups()
                    controlled = (f"spoolet command={operation} exitCode={int(code)} code={error} "
                                  f"elapsed_ms={int(elapsed)} timeoutOrigin={origin}")
                    lines.append(controlled)
                    print("app-journey: " + controlled, flush=True)
    (destination / "attempt.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
    allowed = {"native-result.json", "controller-result.json", "attempt.log", "frame.png", "library-text.png",
               "mobile-default-startup.png", NATIVE_DIAGNOSTIC_NAME}
    files = [path for path in destination.rglob("*") if path.is_file()]
    if any(path.is_symlink() or (path.name not in allowed and not path.name.endswith("-failure.png"))
           for path in files):
        raise RuntimeError("safe artifact export contains a non-allowlisted file")
    # Capability files and their bearer token cannot enter the exported schema.
    serialized = json.dumps(safe_result(result))
    for capability in private.glob("app-journey-*/device.json"):
        token = json.loads(capability.read_text()).get("token")
        if isinstance(token, str) and token and token in serialized:
            raise RuntimeError("safe artifact export contains a private capability")


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
    parser.add_argument("--cleanup-regression", choices=("timeout", "crash"),
                        help="run only a real-emulator host-failure cleanup regression")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.cleanup_regression and args.phase != "e2e":
        parser.error("--cleanup-regression requires --phase e2e")
    output = args.artifact_dir.resolve()
    if args.platform == "android":
        override = os.environ.get("ANDROID_LAUNCH_TEST_DIR")
        output = Path(override).resolve() if override else output / os.environ.get("SPOOL_ANDROID_FORM_FACTOR", "phone")
    output.mkdir(parents=True, exist_ok=True)
    suffix = f"-cleanup-{args.cleanup_regression}" if args.cleanup_regression else ""
    state_path = output / f"{args.phase}{suffix}-result.json"
    device = Android(args.phase) if args.platform == "android" else TvOS(args.phase)
    if args.cleanup_regression:
        names = [f"app-journey-cleanup-{args.cleanup_regression}"]
    else:
        names = receipt(device, ["--list"], args.timeout)
        if args.phase == "e2e":
            # Complete native GPU selectors even when other selectors fail.
            names.append("app-journey")
            if os.environ.get("SPOOL_TEST_DEVICE_CLEANUP_REGRESSIONS") == "1":
                names.extend(("app-journey-cleanup-timeout", "app-journey-cleanup-crash"))
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
        safe = output / "safe-export"
        safe.mkdir(parents=True, exist_ok=True)
        write_journal(safe / state_path.name,
                      {"platform": args.platform, "selectors": names,
                       "results": {name: safe_result(result) for name, result in results.items()}})

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
        attempt_directory = output / name / f"attempt-{len(attempts) + 1}-{uuid.uuid4().hex[:8]}"
        attempt_directory.mkdir(parents=True, exist_ok=False, mode=0o700)
        try:
            if name == "tvos-credentials":
                (output / "credentials-result.json").unlink(missing_ok=True)
            if name == "app-journey":
                result = app_journey(args.platform, attempt_directory, args.timeout)
            elif name.startswith("app-journey-cleanup-"):
                result = cleanup_regression(args.platform, device, attempt_directory,
                                            args.timeout, name.removeprefix("app-journey-cleanup-"))
            else:
                result = receipt(device, ["--child", name], args.timeout, name, attempt_directory)
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, TimeoutError) as error:
            # Subprocess exception strings can contain full argv. Only controlled
            # errors are retained, never native stdout or command payloads.
            result = failure_result(error)
            # JourneyOwner cleaned the production bundle. Do not confuse it
            # with the separate native selector bundle (especially on tvOS).
            if not name.startswith("app-journey"):
                try:
                    device.stop()
                except (OSError, subprocess.SubprocessError):
                    result["diagnosticFailure"] = True
        cleanup_path = attempt_directory / "cleanup-result.json"
        if "cleanup" not in result and cleanup_path.exists():
            result["cleanup"] = json.loads(cleanup_path.read_text())
        if result.get("cleanup", {}).get("errors"):
            result["diagnosticFailure"] = True
        result.update(started=started, finished=time.time())
        result["attempts"] = attempts + [dict(result)]
        results[name] = result
        # Save the terminal result before optional diagnostic work can fail or
        # be interrupted. Each retry retains its earlier receipts and captures.
        persist()
        print(f"{args.phase}/{name}: {result['status']}", flush=True)
        if result["status"] not in ("passed", "skipped"):
            if isinstance(device, TvOS) and not name.startswith("app-journey"):
                try:
                    device.collect_native_diagnostic(attempt_directory, name)
                except (OSError, ValueError, RuntimeError):
                    print(f"{name}: native crash diagnostic unavailable", flush=True)
            try:
                device.capture(attempt_directory, name)
            except (OSError, subprocess.SubprocessError):
                print(f"{name}: failure capture unavailable", flush=True)
        if "credentials" in result:
            try:
                write_journal(output / "credentials-result.json", result["credentials"])
            except OSError:
                result["diagnosticFailure"] = True
                persist()
                print(f"{name}: credential receipt artifact unavailable", flush=True)
        try:
            export_attempt(attempt_directory,
                           output / "safe-export" / args.phase / name / attempt_directory.name, result)
            if name.startswith("app-journey-cleanup-"):
                result["safeExportVerified"] = True
                result["attempts"][-1]["safeExportVerified"] = True
                export_attempt(attempt_directory,
                               output / "safe-export" / args.phase / name / attempt_directory.name, result)
        except (OSError, ValueError, RuntimeError):
            result["diagnosticFailure"] = True
            result["attempts"][-1]["diagnosticFailure"] = True
            print(f"{name}: safe artifact export unavailable", flush=True)
        persist()
    return int(any(result.get("status") not in ("passed", "skipped") or result.get("diagnosticFailure")
                   for result in results.values()))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"device test infrastructure failed: {type(error).__name__}", flush=True)
        raise SystemExit(1)
