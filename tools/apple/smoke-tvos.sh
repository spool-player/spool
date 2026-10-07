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
import json, os, plistlib, re, subprocess, sys, tempfile
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
        # Simulator Keychain still enforces signed application/access-group
        # entitlements. A local ad-hoc identity needs no developer certificate
        # and is not a signing identity for device/App Store distribution.
        application = "SPOOLSMOKE." + identifier
        entitlements = Path(signing) / (identifier + ".plist")
        with entitlements.open("wb") as file:
            plistlib.dump({
                "application-identifier": application,
                "com.apple.developer.team-identifier": "SPOOLSMOKE",
                "keychain-access-groups": [application],
            }, file)
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
    (smoke_id, ["tvos-credentials"],
     "tvOS credentials smoke: Keychain roundtrip and sandbox file persistence passed"),
]
results = {}
for bundle, arguments, expected in checks:
    environment = os.environ.copy()
    if arguments == ["mpv-video-item"]:
        environment["SIMCTL_CHILD_SPOOL_TEST_MPV_LOG"] = "1"
    process = subprocess.run(["xcrun", "simctl", "launch", "--console", device, bundle, *arguments],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90,
                             env=environment)
    passed = process.returncode == 0 and expected in process.stdout
    results[arguments[0]] = passed
    print(f"{arguments[0]}: {'passed' if passed else 'FAILED'}")
    if not passed:
        # Report only controlled native smoke diagnostics; URLs/auth are absent
        # from these test result lines and provider logs are deliberately omitted.
        for line in process.stdout.splitlines():
            if arguments == ["mpv-video-item"] and any(
                    marker in line for marker in ["[vd]", "[vo/libmpv]", "[libmpv_render", "[ffmpeg/video]",
                                                 "player: render backend"]):
                print(re.sub(r"https?://\S+", "[redacted-url]", line))
            if any(marker in line for marker in ["launch test:", "video result:", "orientation:", "viewport:", "decoder:",
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
