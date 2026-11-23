#!/usr/bin/env python3
"""Find visual flashes and layout jumps in a `spoolet capture` frame sequence.

A page should fill in: things appear once in a held place and stay. This
looks for the three ways loading looks wrong instead, and writes a
side-by-side image only for the frames that show one, so nobody has to step
through every frame:

  flash   content leaves a place and then returns to it (A, then B, then A
          again), such as a title that blanks while details reload
  jump    everything below some line moves up or down between two frames,
          such as an overview arriving and pushing the buttons down
  lost    content that was on screen is gone when the page settles, and
          was not merely replaced by other content

Tiles that change on most frames or cycle through many looks (a spinner, a
caret) are animation, not loading, and are ignored. The first frame is the
page the action started from, so it only serves as the "before" of frame 1.

Usage: flash-report.py CAPTURE_DIR [CAPTURE_DIR ...] [--json]
Needs numpy and Pillow, for example:
  nix-shell -p "python3.withPackages (p: [ p.numpy p.pillow ])" --run "python3 tools/flash-report.py DIR"
"""

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageDraw

SCALE = 4  # analyse at a quarter of the window size
TILE = 8  # analysis pixels per tile side (32 window pixels)
CHANGED = 7.0  # mean absolute difference that counts as a visible change
SAME = 4.0  # ... and below which two states are the same
BLANK_STD = 2.5  # a tile this flat holds no content
ANIMATED = 0.45  # share of transitions after which a tile counts as animation
CYCLING = 4  # distinct looks after which a tile counts as animation
DETAIL = 12.0  # gradient that marks an edge or text, as opposed to a flat fill
MIN_JUMP = 2  # analysis pixels (8 window pixels)
BAND = 8  # analysis rows per band when looking for moved content (32 window pixels)
MAX_JUMP = 50  # 200 window pixels; larger matches were coincidences


def load(directory):
    index = json.loads((directory / "index.json").read_text())
    frames = []
    for entry in index["frames"]:
        image = Image.open(directory / entry["file"]).convert("L")
        image = image.resize((image.width // SCALE, image.height // SCALE), Image.BOX)
        frames.append(np.asarray(image, dtype=np.float32))
    return index, frames


def tiles(frame):
    rows, cols = frame.shape[0] // TILE, frame.shape[1] // TILE
    return frame[: rows * TILE, : cols * TILE].reshape(rows, TILE, cols, TILE).swapaxes(1, 2)


def difference(a, b):
    return np.abs(tiles(a) - tiles(b)).mean(axis=(2, 3))


def detail(frame):
    edges = np.zeros_like(frame, dtype=bool)
    edges[:-1, :-1] = (np.abs(np.diff(frame, axis=0))[:, :-1] + np.abs(np.diff(frame, axis=1))[:-1]) > DETAIL
    return edges


def jumps(a, b):
    """Bands of existing content that moved vertically from `a` to `b`.

    The frame is cut into horizontal bands, and each band that held detail
    in `a` (edges, text) looks for itself in `b`. Bands find their own
    offsets, so a page that pushes one row 40px and the next 120px is still
    seen. Only detail is compared, so artwork filling an empty placeholder
    is not a move: a row of identical empty cards would otherwise match
    itself one row down. Returns [(top, bottom, dy)] in analysis pixels.
    """
    height = a.shape[0]
    edges, after = detail(a), detail(b)
    moved = []
    for top in range(0, height - BAND + 1, BAND):
        bottom = top + BAND
        mask = edges[top:bottom]
        if mask.mean() < 0.02:
            continue
        # Content that moves leaves its place. Where most of the band's edges
        # are still there, something filled in around them (a poster in its
        # card) and any offset that matches is a coincidence.
        if (mask & after[top:bottom]).sum() > 0.5 * mask.sum():
            continue
        band = a[top:bottom]

        def cost(dy):
            if top + dy < 0 or bottom + dy > height:
                return np.inf
            return np.abs(band - b[top + dy: bottom + dy])[mask].mean()

        still = cost(0)
        if still <= CHANGED:
            continue
        best, best_cost = 0, still
        for dy in range(-MAX_JUMP, MAX_JUMP + 1):
            if abs(dy) >= MIN_JUMP:
                candidate = cost(dy)
                if candidate < best_cost:
                    best, best_cost = dy, candidate
        if best and best_cost < min(still * 0.5, SAME * 2):
            moved.append((top, bottom, best))
    return moved


def analyse(directory):
    index, frames = load(directory)
    report = {"capture": directory.name, "frames": len(frames), "stopReason": index.get("stopReason"),
              "events": [], "score": 0}
    # Frame 0 shows the page before the action; changes into frame 1 are the
    # navigation itself. Indices below stay those of the captured frames.
    first = 1
    if len(frames) - first < 2:
        return report
    stack = np.stack([tiles(f) for f in frames])  # frame, row, col, y, x
    blank = stack.std(axis=(3, 4)) < BLANK_STD
    changes = np.stack([difference(frames[i - 1], frames[i]) > CHANGED for i in range(first + 1, len(frames))])
    animated = changes.mean(axis=0) > ANIMATED if len(changes) >= 4 else np.zeros(changes.shape[1:], bool)
    rows, cols = blank.shape[1:]

    flashes = {}
    for r in range(rows):
        for c in range(cols):
            if animated[r, c]:
                continue
            seen = []  # distinct states of this tile, as (frame, pixels)
            found = []
            for i in range(first, len(frames)):
                pixels = stack[i, r, c]
                if seen and np.abs(pixels - seen[-1][1]).mean() <= SAME:
                    continue
                for frame, state in seen[:-1]:
                    if not blank[frame, r, c] and np.abs(pixels - state).mean() <= SAME:
                        found.append((i, seen[-1][0]))
                        break
                seen.append((i, pixels))
            if len(seen) > CYCLING:
                animated[r, c] = True
                continue
            for frame, origin in found:
                flashes.setdefault(frame, []).append((r, c, origin))
    for frame, cells in sorted(flashes.items()):
        report["events"].append({"kind": "flash", "frame": frame, "from": min(cell[2] for cell in cells),
                                 "tiles": [[r, c] for r, c, _ in cells]})

    for i in range(first + 1, len(frames)):
        moved = jumps(frames[i - 1], frames[i])
        if moved:
            side = frames[i].shape[1] // TILE
            report["events"].append({"kind": "jump", "frame": i, "from": i - 1,
                                     "dy": max((dy * SCALE for _, _, dy in moved), key=abs),
                                     "bands": [[top * SCALE, bottom * SCALE, dy * SCALE] for top, bottom, dy in moved],
                                     "tiles": [[r, c] for top, bottom, _ in moved
                                               for r in range(top // TILE, (bottom + TILE - 1) // TILE)
                                               for c in range(side)]})

    final = len(frames) - 1
    lost = [[r, c] for r in range(rows) for c in range(cols)
            if not animated[r, c] and blank[final, r, c]
            and any(not blank[i, r, c] for i in range(first, final))]
    if lost:
        report["events"].append({"kind": "lost", "frame": final, "from": first, "tiles": lost})

    # A jump moves the whole page under the eye, so it outweighs a few
    # flashing tiles; content lost at the end counts least.
    report["score"] = sum(len(e["tiles"]) if e["kind"] == "flash" else 25 * len(e["bands"]) if e["kind"] == "jump"
                          else len(e["tiles"]) // 4 for e in report["events"])
    return report


def annotate(directory, report):
    for number, event in enumerate(report["events"]):
        names = [f"frame-{event['from']:04d}.png", f"frame-{event['frame']:04d}.png"]
        before, after = (Image.open(directory / name).convert("RGB") for name in names)
        canvas = Image.new("RGB", (before.width * 2 + 8, before.height), "white")
        canvas.paste(before, (0, 0))
        canvas.paste(after, (before.width + 8, 0))
        draw = ImageDraw.Draw(canvas)
        side = TILE * SCALE
        for r, c in event.get("tiles", []):
            for offset in (0, before.width + 8):
                draw.rectangle([offset + c * side, r * side, offset + (c + 1) * side - 1, (r + 1) * side - 1],
                               outline="red", width=2)
        label = f"{event['kind']}  frame {event['from']} -> {event['frame']}"
        if event["kind"] == "jump":
            label += f"  dy {event['dy']}px"
        draw.rectangle([0, 0, len(label) * 8 + 12, 22], fill="black")
        draw.text((6, 5), label, fill="yellow")
        path = directory / f"flagged-{number:02d}-{event['kind']}.png"
        canvas.save(path)
        event["image"] = str(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("captures", nargs="+", type=pathlib.Path)
    parser.add_argument("--json", action="store_true", help="print the full report as JSON")
    args = parser.parse_args()
    reports = []
    for directory in args.captures:
        for old in directory.glob("flagged-*.png"):
            old.unlink()
        report = analyse(directory)
        annotate(directory, report)
        (directory / "report.json").write_text(json.dumps(report, indent=2))
        reports.append(report)
    if args.json:
        json.dump(reports, sys.stdout, indent=2)
        return
    for report in sorted(reports, key=lambda r: -r.get("score", 0)):
        kinds = {}
        for event in report["events"]:
            kinds[event["kind"]] = kinds.get(event["kind"], 0) + 1
        summary = ", ".join(f"{count} {kind}" for kind, count in sorted(kinds.items())) or "clean"
        print(f"{report.get('score', 0):5d}  {report['capture']}: {report['frames']} frames, {summary}")


if __name__ == "__main__":
    main()
