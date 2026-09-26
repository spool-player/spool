#!/usr/bin/env python3
"""Aggregate + symbolize a bigalloc dump (live >threshold allocations w/ stacks).

Usage: bigalloc-report.py <dump-file> [--top N]

The dump (one line per live allocation): "<size> <module>+0x<off> ...".
We group identical stacks, sum their bytes, and addr2line the frames against
local unstripped binaries (build-id/basename matched). Answers: which call
stacks hold the big memory.
"""
import collections
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.path.join(os.path.dirname(ROOT), "build/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot")
ADDR2LINE = os.path.join(SDK, "bin/arm-webos-linux-gnueabi-addr2line")

# Where to find local copies of the TV modules (best symbols first).
SEARCH_DIRS = [
    os.path.join(ROOT, "build"),                 # spool.unstripped
    os.path.join(ROOT, "build/webos-mpv-build"), # libmpv (if present, unstripped)
    os.path.join(ROOT, "app/lib"),               # deployed libs (build-id match)
    os.path.join(os.path.dirname(ROOT), "build/third_party"),
]


def find_local(modpath):
    base = os.path.basename(modpath)
    if base.startswith("spool"):
        cand = os.path.join(ROOT, "build/spool.unstripped")
        if os.path.exists(cand):
            return cand
    # Match soname (libmpv.so.2) to the versioned file (libmpv.so.2.5.0).
    stem = base.split(".so")[0] + ".so" if ".so" in base else base
    for d in SEARCH_DIRS:
        for root, _, files in os.walk(d):
            if base in files:
                return os.path.join(root, base)
            for fn in files:
                if fn.startswith(stem):
                    return os.path.join(root, fn)
    return None


_a2l_cache = {}


def addr2line(localfile, off):
    if not localfile or not os.path.exists(ADDR2LINE):
        return None
    key = (localfile, off)
    if key in _a2l_cache:
        return _a2l_cache[key]
    try:
        out = subprocess.run([ADDR2LINE, "-f", "-C", "-e", localfile, off],
                             capture_output=True, text=True, timeout=10).stdout.strip().splitlines()
    except Exception:
        out = []
    res = None
    if out and out[0] not in ("??", ""):
        res = out[0] + (" (" + out[1] + ")" if len(out) > 1 and out[1] not in ("??:0", "??:?") else "")
    _a2l_cache[key] = res
    return res


def sym(frame):
    # New format from dladdr: "module(symbol+0xoff)" -> symbol already resolved.
    if "(" in frame and frame.endswith(")"):
        mod, rest = frame.split("(", 1)
        return f"{rest.rstrip(')')}  [{os.path.basename(mod)}]"
    if "+0x" not in frame:
        return frame
    mod, off = frame.rsplit("+", 1)
    local = find_local(mod)
    fn = addr2line(local, off) if local else None
    base = os.path.basename(mod)
    return f"{fn}  [{base}]" if fn else f"{base}+{off}"


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: bigalloc-report.py <dump> [--top N]")
    dump = sys.argv[1]
    top = 25
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])

    stacks = collections.defaultdict(lambda: [0, 0])  # stack -> [bytes, count]
    footer = ""
    with open(dump) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                footer = line
                continue
            parts = line.split(" ")
            if len(parts) < 2:
                continue
            try:
                size = int(parts[0])
            except ValueError:
                continue
            # Drop the tracer's own frames (track/malloc/realloc in bigalloc.so).
            frames = tuple(p for p in parts[1:] if "bigalloc.so" not in p)
            stacks[frames][0] += size
            stacks[frames][1] += 1

    ranked = sorted(stacks.items(), key=lambda kv: kv[1][0], reverse=True)
    grand = sum(v[0] for v in stacks.values())
    print(f"=== bigalloc: {len(stacks)} distinct stacks, {grand/1048576:.1f} MB live in >=threshold allocations ===")
    if footer:
        print(footer)
    print()
    for frames, (nbytes, count) in ranked[:top]:
        print(f"{nbytes/1048576:7.1f} MB  x{count:<5d}")
        for fr in frames:
            print(f"      {sym(fr)}")
        print()


if __name__ == "__main__":
    main()
