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
xcrun simctl install "$device" "$app"
# simctl's console mode returns after process exit. Parse only the exact
# credential-free consumer-test result, never dump full app/provider logs.
python3 - "$device" "$app" "$result" <<'PY'
import json, re, subprocess, sys
from pathlib import Path

device, app, output = sys.argv[1:]
output = Path(output)
checks = [
    ("com.sachk.spool", ["--launch-test"], "launch test: application UI rendered"),
    ("com.sachk.spool.playback-smoke", ["mpv-video-item"],
     "mpv video smoke: upright frames and OSD rendered across detach and resize"),
    ("com.sachk.spool.playback-smoke", ["tvos-audio"],
     "tvOS audio smoke: AudioUnit output advanced with exclusive playback session"),
    ("com.sachk.spool.playback-smoke", ["tvos-credentials"],
     "tvOS credentials smoke: Keychain roundtrip and sandbox file persistence passed"),
]
smoke = Path(app).parent / "smoke" / "spool-tvos-playback-smoke.app"
subprocess.run(["xcrun", "simctl", "install", device, str(smoke)], check=True)
results = {}
for bundle, arguments, expected in checks:
    process = subprocess.run(["xcrun", "simctl", "launch", "--console", device, bundle, *arguments],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
    passed = process.returncode == 0 and expected in process.stdout
    results[arguments[0]] = passed
    print(f"{arguments[0]}: {'passed' if passed else 'FAILED'}")
    if not passed:
        # Report only controlled native smoke diagnostics; URLs/auth are absent
        # from these test result lines and provider logs are deliberately omitted.
        for line in process.stdout.splitlines():
            if any(marker in line for marker in ["launch test:", "video result:", "orientation:",
                                                "render context was not ready", "failed to initialize mpv",
                                                "tvOS audio smoke:", "tvOS credentials smoke:",
                                                "startup:", "[qml]", "font registration failed:",
                                                "database initialization failed:", "dyld[",
                                                "An error was encountered processing the command"]):
                print(re.sub(r"https?://\S+", "[redacted-url]", line))
        (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
        raise SystemExit(1)
(output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
PY
xcrun simctl io "$device" screenshot "$result/simulator.png"
