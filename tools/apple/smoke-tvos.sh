#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="${1:?Pass the built tvOS Spool.app}"
[[ "$(uname -s)" == Darwin ]] || { echo 'error: Apple TV simulator requires macOS/Xcode' >&2; exit 1; }
result="$ROOT/build/tvos/smoke"
mkdir -p "$result"
runtime="$(xcrun simctl list runtimes --json | python3 -c 'import json,sys; r=[x for x in json.load(sys.stdin)["runtimes"] if x["isAvailable"] and x["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.tvOS")]; print(sorted(r,key=lambda x:tuple(map(int,x["version"].split("."))))[-1]["identifier"])')"
device_type="$(xcrun simctl list devicetypes --json | python3 -c 'import json,sys; print(next(x["identifier"] for x in json.load(sys.stdin)["devicetypes"] if x["name"].startswith("Apple TV 4K")))')"
device="$(xcrun simctl create Spool-tvOS-smoke "$device_type" "$runtime")"
cleanup() { xcrun simctl shutdown "$device" >/dev/null 2>&1 || true; xcrun simctl delete "$device"; }
trap cleanup EXIT
xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b
# simctl's console mode returns after process exit. Parse only the exact
# credential-free consumer-test result, never dump full app/provider logs.
python3 - "$device" "$app" "$result" <<'PY'
import json, plistlib, re, subprocess, sys, tempfile, uuid
from pathlib import Path

device, app, output = sys.argv[1:]
output = Path(output)
app = Path(app)
smoke = app.parent / "smoke" / "spool-tvos-playback-smoke.app"
bundle_ids = []
with tempfile.TemporaryDirectory(prefix="spool-tvos-sign-") as signing:
    for bundle in (app, smoke):
        with (bundle / "Info.plist").open("rb") as metadata:
            identifier = plistlib.load(metadata)["CFBundleIdentifier"]
        bundle_ids.append(identifier)
        # Application/Keychain entitlements are embedded by the simulator
        # linker. The host macOS signature must contain only its local debug
        # entitlement; signing restricted iOS entitlements makes AMFI kill it.
        entitlements = Path(signing) / (identifier + ".plist")
        with entitlements.open("wb") as file:
            plistlib.dump({"com.apple.security.get-task-allow": True}, file)
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none",
                        "--entitlements", str(entitlements), "--generate-entitlement-der", str(bundle)], check=True)
        subprocess.run(["xcrun", "simctl", "install", device, str(bundle)], check=True)
app_id, smoke_id = bundle_ids
checks = [
    (app_id, ["--launch-test"], "launch test: application UI rendered"),
    (smoke_id, ["mpv-video-item"],
     "mpv video smoke: upright frames and OSD rendered across detach and resize"),
    (smoke_id, ["tvos-audio"],
     "tvOS audio smoke: AudioUnit output advanced with exclusive playback session"),
    (smoke_id, ["tvos-credentials"], None),
]
results = {}
for bundle, arguments, expected in checks:
    receipt = None
    launch_arguments = arguments
    if arguments == ["tvos-credentials"]:
        container = subprocess.check_output(
            ["xcrun", "simctl", "get_app_container", device, bundle, "data"], text=True).strip()
        receipt = Path(container) / "tmp" / "tvos-credentials-result.json"
        receipt.unlink(missing_ok=True)
        (output / "credentials-result.json").unlink(missing_ok=True)
        nonce = uuid.uuid4().hex
        launch_arguments = [*arguments, nonce]
    # Selectors are separate native executions, not arguments to a process
    # UIKit may still be retiring after the previous consumer returned.
    process = subprocess.run(["xcrun", "simctl", "launch", "--console", "--terminate-running-process",
                              device, bundle, *launch_arguments],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
    if receipt is None:
        passed = process.returncode == 0 and expected in process.stdout
    else:
        try:
            report = json.loads(receipt.read_text())
        except (OSError, ValueError):
            report = None
        expected_report = {"case": "tvos-credentials", "nonce": nonce,
                           "saved": True, "loaded": True, "removed": True, "writable": True}
        valid = (isinstance(report, dict) and report.keys() == expected_report.keys()
                 and report["case"] == expected_report["case"] and report["nonce"] == nonce
                 and all(type(report[key]) is bool for key in ["saved", "loaded", "removed", "writable"]))
        passed = process.returncode == 0 and valid and report == expected_report
        if valid:
            (output / "credentials-result.json").write_text(json.dumps(report, indent=2) + "\n")
            if passed and "Keychain roundtrip and sandbox file persistence passed" not in process.stdout:
                print("tvOS credentials smoke: console marker absent; fresh native receipt passed")
        if not passed:
            print("tvOS credentials smoke: native receipt " + (
                json.dumps({key: report[key] for key in ["saved", "loaded", "removed", "writable"]})
                if valid else "missing or invalid"))
    results[arguments[0]] = passed
    print(f"{arguments[0]}: {'passed' if passed else 'FAILED'}")
    if not passed:
        print(f"native consumer exit: {process.returncode}")
        # Report only controlled native smoke diagnostics; URLs/auth are absent
        # from these test result lines and provider logs are deliberately omitted.
        lines = process.stdout.splitlines()
        # simctl rejects a process before any app/provider code runs. Preserve
        # its nested native launch error, not unrelated application output.
        native_error = next((index for index, line in enumerate(lines)
                             if line.startswith("An error was encountered processing the command")), None)
        if native_error is not None:
            for line in lines[native_error:]:
                print(re.sub(r"https?://\S+", "[redacted-url]", line))
        for line in lines:
            if any(marker in line for marker in ["launch test:", "video result:", "orientation:", "viewport:",
                                                "first video frame", "render context handoff did not complete",
                                                "render context was not ready", "failed to initialize mpv",
                                                "tvOS audio smoke:", "tvOS credentials smoke:",
                                                "startup:", "[qml]", "font registration failed:",
                                                "database initialization failed:", "dyld[",
                                                "An error was encountered processing the command"]):
                print(re.sub(r"https?://\S+", "[redacted-url]", line))
        if arguments == ["mpv-video-item"]:
            container = subprocess.check_output(
                ["xcrun", "simctl", "get_app_container", device, bundle, "data"], text=True).strip()
            frame = Path(container) / "tmp" / "mpv-video-item-failure.png"
            if frame.exists():
                (output / "video-failure.png").write_bytes(frame.read_bytes())
        (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
        raise SystemExit(1)
(output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
PY
xcrun simctl io "$device" screenshot "$result/simulator.png"
