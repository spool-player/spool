#!/usr/bin/env python3
"""Build, check and describe Spool provider packages (.tar.zst, manifest format 2).

  spool-provider.py build DIR [--output FILE]     deterministic package from a provider checkout
  spool-provider.py validate FILE                 the same checks Spool runs before installing
  spool-provider.py feed FILE --url URL           the store/feed entry for a published package

A package holds manifest.json, LICENSE/NOTICE, the icon, and the logic/, ui/ and
assets/ directories. Nothing in it is executed by these checks.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
from typing import NoReturn

API = "0.2"
MAX_ARCHIVE = 16 * 1024 * 1024
MAX_EXPANDED = 32 * 1024 * 1024
MAX_FILE = 8 * 1024 * 1024
MAX_FILES = 512
EXTENSIONS = {".mjs", ".js", ".qml", ".json", ".png", ".jpg", ".svg", ".webp", ".ttf", ".otf", ".txt", ".md", ".map"}
QML_IMPORTS = {"QtQuick", "QtQuick.Layouts", "QtQuick.Controls", "QtQml", "QtQml.Models", "Spool.Ui"}
CAPABILITIES = {"search", "userState", "reporting", "segments", "streamQuality", "trickplay", "discovery",
                "groupPlayback", "remoteControl"}
UI_ROLES = {"login", "settings", "picker"}
ROOTS = ("manifest.json", "LICENSE", "NOTICE", "logic", "ui", "assets")
NATIVE_MAGIC = (b"\x7fELF", b"MZ", b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\0asm")
ID = re.compile(r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+")
VERSION = re.compile(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?")


def fail(message: str) -> NoReturn:
    raise ValueError(message)


def valid_path(name: str) -> bool:
    parts = name.split("/")
    suffix = pathlib.PurePosixPath(name).suffix.lower()
    return (0 < len(name) <= 200 and "\\" not in name and ":" not in name
            and all(part and not part.startswith(".") and not part.endswith(" ") for part in parts)
            and (suffix in EXTENSIONS or parts[-1] in {"LICENSE", "NOTICE"}))


def validate(files: dict[str, bytes]) -> dict:
    if not files or len(files) > MAX_FILES or sum(map(len, files.values())) > MAX_EXPANDED:
        fail("package is empty or too large")
    folded = set()
    for name, data in files.items():
        if not valid_path(name):
            fail(f"forbidden path: {name}")
        if name.casefold() in folded:
            fail(f"case-colliding path: {name}")
        folded.add(name.casefold())
        if len(data) > MAX_FILE or data.startswith(NATIVE_MAGIC):
            fail(f"oversized or native file: {name}")
    try:
        manifest = json.loads(files["manifest.json"])
    except (KeyError, ValueError):
        fail("manifest.json is missing or not JSON")
    if not isinstance(manifest, dict) or manifest.get("format") != 2 or manifest.get("api") != API:
        fail(f"manifest must declare format 2 and api {API}")
    for key in ("id", "name", "version", "entry"):
        if not isinstance(manifest.get(key), str) or not manifest[key].strip():
            fail(f"manifest.{key} is required")
    if not ID.fullmatch(manifest["id"]) or len(manifest["id"]) > 128:
        fail("manifest.id must look like publisher.name")
    if not VERSION.fullmatch(manifest["version"]):
        fail("manifest.version must be semantic (1.2.3 or 1.2.3-beta.1)")
    if len(manifest["name"]) > 64 or len(manifest.get("summary", "")) > 120:
        fail("name is limited to 64 characters and summary to 120")
    if not manifest["entry"].endswith(".mjs") or manifest["entry"] not in files:
        fail("manifest.entry must name a packaged .mjs module")
    if manifest.get("icon") and manifest["icon"] not in files:
        fail("manifest.icon names a missing file")
    ui = manifest.get("ui", {})
    if not isinstance(ui, dict) or not set(ui) <= UI_ROLES:
        fail(f"manifest.ui roles are {sorted(UI_ROLES)}")
    for role, path in ui.items():
        if path not in files or not path.endswith(".qml"):
            fail(f"manifest.ui.{role} names a missing QML file")
    unknown = set(manifest.get("capabilities", [])) - CAPABILITIES
    if unknown:
        fail(f"unknown capabilities: {sorted(unknown)}")
    for origin in manifest.get("origins", []):
        if origin != "*" and not re.fullmatch(r"https?://[^/\s]+", origin):
            fail(f"origins are scheme://host[:port] or *: {origin}")
    for name, data in files.items():
        if name.endswith(".qml"):
            imports = set(re.findall(r"^\s*import\s+([A-Za-z][\w.]*)", data.decode("utf-8"), re.MULTILINE))
            if not imports <= QML_IMPORTS:
                fail(f"{name} imports modules Spool does not provide: {sorted(imports - QML_IMPORTS)}")
    return manifest


def compress(data: bytes) -> bytes:
    try:
        from compression import zstd  # Python 3.14+
        return zstd.compress(data, level=19)
    except ImportError:
        pass
    if not shutil.which("zstd"):
        fail("needs Python 3.14 or the zstd command")
    return subprocess.run(["zstd", "-19", "-q", "-c", f"--stream-size={len(data)}"], input=data,
                          capture_output=True, check=True).stdout


def decompress(data: bytes) -> bytes:
    try:
        from compression import zstd
        return zstd.decompress(data)
    except ImportError:
        pass
    if not shutil.which("zstd"):
        fail("needs Python 3.14 or the zstd command")
    return subprocess.run(["zstd", "-d", "-q", "-c"], input=data, capture_output=True, check=True).stdout


def read(path: pathlib.Path) -> tuple[dict, dict[str, bytes]]:
    data = path.read_bytes()
    if len(data) > MAX_ARCHIVE:
        fail("archive is larger than 16 MiB")
    files = {}
    with tarfile.open(fileobj=io.BytesIO(decompress(data)), mode="r:") as archive:
        for member in archive.getmembers():
            if member.isdir():
                continue
            if not member.isfile():
                fail(f"links and special files are not allowed: {member.name}")
            files[member.name.removeprefix("./")] = archive.extractfile(member).read()
    return validate(files), files


def collect(source: pathlib.Path) -> dict[str, bytes]:
    files = {}
    for root in ROOTS:
        path = source / root
        for entry in [path] if path.is_file() else sorted(path.rglob("*")) if path.is_dir() else []:
            if entry.is_symlink():
                fail(f"symlinks are not allowed: {entry}")
            if entry.is_file():
                files[entry.relative_to(source).as_posix()] = entry.read_bytes()
    return files


def build(source: pathlib.Path, output: pathlib.Path | None) -> pathlib.Path:
    files = collect(source)
    manifest = validate(files)
    tar = io.BytesIO()
    with tarfile.open(fileobj=tar, mode="w:", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in sorted(files.items()):
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(data), 0o644, 0
            archive.addfile(info, io.BytesIO(data))
    output = output or source / "dist" / f"{manifest['id']}-{manifest['version']}.tar.zst"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(compress(tar.getvalue()))
    read(output)
    return output


def feed(path: pathlib.Path, url: str) -> dict:
    manifest, _ = read(path)
    data = path.read_bytes()
    entry = {key: manifest[key] for key in ("id", "name", "version", "api", "summary", "publisher", "homepage")
             if manifest.get(key)}
    entry.update(url=url, size=len(data), sha256=hashlib.sha256(data).hexdigest())
    return entry


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("build", "validate", "feed"))
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--url")
    args = parser.parse_args()
    if args.command == "build":
        print(build(args.path, args.output))
    elif args.command == "validate":
        manifest, _ = read(args.path)
        print(f"{manifest['id']} {manifest['version']} ok")
    else:
        if not args.url:
            parser.error("feed needs --url, the address the package is downloaded from")
        print(json.dumps(feed(args.path, args.url), indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        print(f"spool-provider: {error}", file=sys.stderr)
        raise SystemExit(1)
