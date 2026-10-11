#!/usr/bin/env python3
"""Capture and score page loads on every provider of a running Spool.

Drives an instance through spoolet the way a person moves through it --
library, movie, series, season, episode, back, Home rows -- capturing every
visibly changed frame of each load with `spoolet capture`, then ranks the
loads with flash-report.py. Only the flagged frames need looking at; each
capture directory holds them as flagged-*.png next to its report.json.

Start an instance first, offscreen so it does not take over the desktop:
  nix run .#run -- -platform offscreen --instance flashprobe
then:
  nix-shell -p "python3.withPackages (p: [ p.numpy p.pillow ])" \\
      --run "python3 tools/flash-sweep.py --instance flashprobe --out /tmp/flash"

Output names carry account prefixes and positions, never titles or URLs.
"""

import argparse
import importlib.util
import json
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Spoolet:
    def __init__(self, binary, instance):
        self.binary, self.instance = binary, instance

    def __call__(self, *args, check=True):
        result = subprocess.run([self.binary, "--instance", self.instance, *map(str, args)],
                                capture_output=True, text=True, timeout=120)
        try:
            reply = json.loads(result.stdout)
        except json.JSONDecodeError:
            raise SystemExit(f"spoolet {args[0]}: no JSON reply")
        if check and not reply.get("ok"):
            raise RuntimeError(f"spoolet {' '.join(map(str, args))}: {reply.get('error')}")
        return reply.get("result", {})


def capture(spoolet, out, name, action, settle_ms, max_ms):
    directory = out / name
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    spoolet("capture", "start", directory, settle_ms, max_ms)
    try:
        action()
    except RuntimeError as error:
        spoolet("capture", "stop")
        print(f"  skip {name}: {error}")
        shutil.rmtree(directory)
        return None
    while spoolet("capture", "status")["running"]:
        time.sleep(0.2)
    status = spoolet("capture", "status")
    print(f"  {name}: {status['saved']} frames, {status['stopReason']}")
    return directory


def first_of(spoolet, kind, prefix, types):
    for row in spoolet("items", kind, 0, 200)["items"]:
        if row["id"].startswith(prefix + ":") and row["type"] in types:
            return row
    return None


def sweep(spoolet, out, prefixes, settle_ms, max_ms, pick):
    captured = []

    def run(name, action):
        directory = capture(spoolet, out, name, action, settle_ms, max_ms)
        if directory:
            captured.append(directory)
        return directory

    libraries = spoolet("items", "libraries", 0, 200)["items"]
    accounts = sorted({row["id"].split(":", 1)[0] for row in libraries})
    for account in accounts:
        if prefixes and account not in prefixes:
            continue
        print(f"account {account}")
        for kind in ("movies", "tvshows"):
            library = next((row for row in libraries if row["id"].startswith(account + ":")
                            and row["type"] == kind), None)
            if not library:
                continue
            spoolet("home")
            time.sleep(1)
            if not run(f"{account}-{kind}-library", lambda: spoolet("library", library["id"])):
                continue
            if spoolet("items", "browse", 0, 1)["count"] <= pick:
                continue
            if not run(f"{account}-{kind}-details", lambda: spoolet("open", "browse", pick)):
                continue
            if kind == "tvshows":
                if run(f"{account}-season", lambda: spoolet("open", "context", 0)):
                    run(f"{account}-episode", lambda: spoolet("open", "context", pick))
                    run(f"{account}-episode-back", lambda: spoolet("back"))
                    run(f"{account}-season-back", lambda: spoolet("back"))
            run(f"{account}-{kind}-details-back", lambda: spoolet("back"))

    spoolet("home")
    time.sleep(1)
    for kind in ("resume", "next-up"):
        rows = spoolet("items", kind, 0, 200)["items"]
        for account in accounts:
            if prefixes and account not in prefixes:
                continue
            index = next((row["index"] for row in rows if row["id"].startswith(account + ":")), None)
            if index is None:
                continue
            spoolet("home")
            time.sleep(1)
            run(f"{account}-{kind}-open", lambda: spoolet("open", kind, index))
    return captured


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--instance", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--spoolet", default=str(ROOT / "build/linux-release/install/bin/spoolet"))
    parser.add_argument("--account", action="append", default=[], help="limit to an account prefix")
    parser.add_argument("--settle-ms", type=int, default=1500)
    parser.add_argument("--max-ms", type=int, default=12000)
    parser.add_argument("--pick", type=int, default=0,
                        help="open the Nth library item and episode; a fresh N avoids the details cache")
    args = parser.parse_args()
    spoolet = Spoolet(args.spoolet, args.instance)
    if not spoolet("status").get("initialized"):
        raise SystemExit("instance is not initialized yet")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    captured = sweep(spoolet, args.out, set(args.account), args.settle_ms, args.max_ms, args.pick)

    spec = importlib.util.spec_from_file_location("flash_report", ROOT / "tools/flash-report.py")
    report = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(report)
    sys.argv = ["flash-report.py", *map(str, captured)]
    print("\nscore  load")
    report.main()


if __name__ == "__main__":
    main()
