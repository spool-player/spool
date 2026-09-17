#!/usr/bin/env python3
"""Prepare pinned Windows tools outside the Nix store; never build Spool here."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
STATE = Path(os.environ.get("SPOOL_WINDOWS_HOME", Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "spool/windows-proton"))
MANIFEST = json.loads((ROOT / "tools/manifests/windows-proton.json").read_text())
STATE.mkdir(parents=True, exist_ok=True)
STAMPS = STATE / ".installed"
STAMPS.mkdir(exist_ok=True)


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def download(url, path, expected):
    if path.is_file() and digest(path) == expected:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_name(path.name + ".part")
    print(f"Downloading {path.name}", flush=True)
    subprocess.run(["curl", "--fail", "--location", "--retry", "4", "--retry-all-errors", "--output", str(partial), url], check=True)
    if digest(partial) != expected:
        raise RuntimeError(f"Checksum mismatch: {path.name}")
    partial.replace(path)


for asset in MANIFEST["assets"]:
    source = STATE / asset["file"]
    destination = STATE / asset["destination"]
    stamp = STAMPS / asset["sha256"]
    if stamp.is_file() and destination.exists():
        continue
    download(asset["url"], source, asset["sha256"])
    print(f"Preparing {asset['destination']}", flush=True)
    if asset["kind"] == "file":
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source != destination:
            shutil.copyfile(source, destination)
    else:
        destination.mkdir(parents=True, exist_ok=True)
        if asset["kind"] == "zip":
            with zipfile.ZipFile(source) as archive:
                archive.extractall(destination)
        elif asset["kind"] == "tar":
            # The checksum-pinned Proton distribution intentionally includes
            # absolute Wine drive symlinks in its template prefix.
            with tarfile.open(source) as archive:
                archive.extractall(destination, filter="tar" if asset["destination"] == "runtimes" else "data")
        elif asset["kind"] == "7z":
            subprocess.run(["7z", "x", "-y", f"-o{destination}", str(source)], check=True)
        else:
            raise RuntimeError(f"Unknown archive kind: {asset['kind']}")
    stamp.touch()

python_root = STATE / "python"
# Embedded Python otherwise ignores PYTHONPATH, which libplacebo uses for
# its pinned glad and Jinja generators. Use Python's normal module search.
for pth in python_root.glob("python*._pth"):
    pth.unlink()

compiler = STATE / "msvc/VC/Tools/MSVC"
if not compiler.is_dir():
    bootstrap = STATE / "toolchain-bootstrap"
    if not bootstrap.exists():
        subprocess.run(["git", "clone", "https://github.com/mstorsjo/msvc-wine.git", str(bootstrap)], check=True)
    subprocess.run(["git", "-C", str(bootstrap), "checkout", "--detach", MANIFEST["msvcRevision"]], check=True)
    manifest = MANIFEST["msvcManifest"]
    manifest_path = STATE / "VisualStudio.vsman"
    download(manifest["url"], manifest_path, manifest["sha256"])
    print("Installing the licensed Microsoft toolchain for local use only.", flush=True)
    subprocess.run([
        "python3", str(bootstrap / "vsdownload.py"), "--manifest", str(manifest_path),
        "--major", "17", "--accept-license", "--architecture", "x64", "--host-arch", "x64",
        "--with-asan", "no", "--with-atl", "no", "--with-dia", "no", "--with-msbuild", "no",
        "--cache", str(STATE / "downloads/msvc"), "--dest", str(STATE / "msvc"),
    ], check=True)
print(f"Windows build tools ready: {STATE}")
