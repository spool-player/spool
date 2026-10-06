#!/usr/bin/env python3
"""Render the existing Spool icon into Xcode's layered Apple TV catalogue."""
import json
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
catalogue = Path(sys.argv[1]).resolve()
brand = catalogue / "App Icon & Top Shelf Image.brandassets"
info = {"version": 1, "author": "xcode"}


def descriptor(directory, **fields):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "Contents.json").write_text(json.dumps({**fields, "info": info}, indent=2) + "\n")


def render(path, width, height, foreground):
    command = ["magick", "-size", f"{width}x{height}", "xc:none" if foreground else "xc:#17151b"]
    if foreground:
        icon_size = round(height * 0.72)
        command += ["(", "-background", "none", str(root / "app/icons/spool.svg"),
                    "-resize", f"{icon_size}x{icon_size}", ")", "-gravity", "center", "-composite"]
    subprocess.run([*command, str(path)], check=True)


descriptor(catalogue)
assets = []
for name, width, height in [("Large", 1280, 768), ("Small", 400, 240)]:
    stack = brand / f"App Icon - {name}.imagestack"
    descriptor(stack, layers=[{"filename": "Front.imagestacklayer"}, {"filename": "Back.imagestacklayer"}])
    for layer in ["Front", "Back"]:
        layer_path = stack / f"{layer}.imagestacklayer"
        descriptor(layer_path)
        images = []
        for scale in ([1] if name == "Large" else [1, 2]):
            filename = f"{layer.lower()}-{scale}x.png"
            images.append({"idiom": "tv", "scale": f"{scale}x", "filename": filename})
            (layer_path / "Content.imageset").mkdir(parents=True, exist_ok=True)
            render(layer_path / "Content.imageset" / filename, width * scale, height * scale, layer == "Front")
        descriptor(layer_path / "Content.imageset", images=images)
    assets.append({"size": f"{width}x{height}", "idiom": "tv", "filename": stack.name, "role": "primary-app-icon"})
for name, width, role in [("Top Shelf Image", 1920, "top-shelf-image"),
                          ("Top Shelf Image Wide", 2320, "top-shelf-image-wide")]:
    directory = brand / f"{name}.imageset"
    directory.mkdir(parents=True, exist_ok=True)
    images = []
    for scale in [1, 2]:
        filename = f"shelf-{scale}x.png"
        render(directory / filename, width * scale, 720 * scale, False)
        # Composite the same foreground icon, retaining an opaque back image.
        foreground = directory / f"foreground-{scale}x.png"
        render(foreground, width * scale, 720 * scale, True)
        subprocess.run(["magick", str(directory / filename), str(foreground), "-composite", str(directory / filename)], check=True)
        foreground.unlink()
        images.append({"idiom": "tv", "scale": f"{scale}x", "filename": filename})
    descriptor(directory, images=images)
    assets.append({"size": f"{width}x720", "idiom": "tv", "filename": directory.name, "role": role})
descriptor(brand, assets=assets)
