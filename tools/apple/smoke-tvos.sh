#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
app="${1:?Pass the built tvOS Spool.app}"
[[ "$(uname -s)" == Darwin ]] || { echo 'error: Apple TV simulator requires macOS/Xcode' >&2; exit 1; }
host_build="${SPOOL_DEVICE_HOST_BUILD_DIR:-$ROOT/build/macos/app}"
if [[ -z "${SPOOL_DEVICE_HOST_E2E:-}" ]]; then
  [[ -f "$host_build/native-spool-e2e-path.txt" ]] || {
    echo 'error: build the native host journey controller with tools/build-macos.sh first' >&2
    exit 1
  }
  read -r SPOOL_DEVICE_HOST_E2E <"$host_build/native-spool-e2e-path.txt"
fi
if [[ -z "${SPOOL_E2E_SPOOLET:-}" ]]; then
  [[ -f "$host_build/native-spoolet-path.txt" ]] || {
    echo 'error: build the matching native spoolet with tools/build-macos.sh first' >&2
    exit 1
  }
  read -r SPOOL_E2E_SPOOLET <"$host_build/native-spoolet-path.txt"
fi
export SPOOL_DEVICE_HOST_E2E SPOOL_E2E_SPOOLET
export SPOOL_E2E_ISOLATED_DEVICE=1
result="$ROOT/build/tvos/smoke"
mkdir -p "$result"
runtime="$(xcrun simctl list runtimes --json | python3 -c 'import json,sys; r=[x for x in json.load(sys.stdin)["runtimes"] if x["isAvailable"] and x["identifier"].startswith("com.apple.CoreSimulator.SimRuntime.tvOS")]; print(sorted(r,key=lambda x:tuple(map(int,x["version"].split("."))))[-1]["identifier"])')"
device_type="$(xcrun simctl list devicetypes --json | python3 -c 'import json,sys; print(next(x["identifier"] for x in json.load(sys.stdin)["devicetypes"] if x["name"].startswith("Apple TV 4K")))')"
device="$(xcrun simctl create Spool-tvOS-tests "$device_type" "$runtime")"
cleanup() { xcrun simctl shutdown "$device" >/dev/null 2>&1 || true; xcrun simctl delete "$device"; }
trap cleanup EXIT
xcrun simctl boot "$device"
xcrun simctl bootstatus "$device" -b
export SPOOL_TVOS_DEVICE="$device"
# Install the real product and both unified native selector bundles. Application
# and Keychain entitlements stay embedded by the simulator linker; the local
# macOS signature must not duplicate restricted iOS entitlements (AMFI rejects it).
python3 - "$device" "$app" <<'PY'
import plistlib
import subprocess
import sys
import tempfile
from pathlib import Path

device, app = sys.argv[1:]
app = Path(app).resolve()
bundles = (app, app.parent / "tests/spool-tests.app", app.parent / "tests/spool-e2e-tests.app")
with tempfile.TemporaryDirectory(prefix="spool-tvos-sign-") as signing:
    for bundle in bundles:
        with (bundle / "Info.plist").open("rb") as metadata:
            identifier = plistlib.load(metadata)["CFBundleIdentifier"]
        entitlements = Path(signing) / (identifier + ".plist")
        with entitlements.open("wb") as file:
            plistlib.dump({"com.apple.security.get-task-allow": True}, file)
        subprocess.run(["codesign", "--force", "--sign", "-", "--timestamp=none",
                        "--entitlements", str(entitlements), "--generate-entitlement-der", str(bundle)], check=True)
        subprocess.run(["xcrun", "simctl", "install", device, str(bundle)], check=True)
PY
build_dir="${SPOOL_TVOS_TEST_BUILD_DIR:-$(dirname "$app")/../app}"
status=0
# CTest traditional and e2e phases use run-device-tests.py, requiring fresh native
# nonce receipts and preserving the extra Keychain/sandbox consumer receipt.
# Both phases finish even when a selector fails, crashes, or exceeds its deadline.
nix develop "$ROOT#native" -c python "$ROOT/tools/run-tests.py" \
  --build-dir "$build_dir" --config Release --workers 1 || status=1
# Keep the product bundle's UI launch contract in addition to native GPU/QML
# selectors. This is not substituted for a provider/playback app journey.
python3 - "$device" "$app" "$result" <<'PY' || status=1
import json
import plistlib
import subprocess
import sys
from pathlib import Path

device, app, output = sys.argv[1:]
with (Path(app) / "Info.plist").open("rb") as file:
    bundle = plistlib.load(file)["CFBundleIdentifier"]
try:
    process = subprocess.run(["xcrun", "simctl", "launch", "--console", "--terminate-running-process",
                              device, bundle, "--launch-test"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, timeout=90)
    passed = process.returncode == 0 and "launch test: application UI rendered" in process.stdout
except subprocess.TimeoutExpired:
    subprocess.run(["xcrun", "simctl", "terminate", device, bundle],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    passed = False
(Path(output) / "launch-result.json").write_text(json.dumps({"application-ui-rendered": passed}, indent=2) + "\n")
print("application UI launch: " + ("passed" if passed else "FAILED"))
raise SystemExit(0 if passed else 1)
PY
xcrun simctl io "$device" screenshot "$result/simulator.png" || status=1
exit "$status"
