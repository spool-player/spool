#!/usr/bin/env python3
"""Build and validate reviewed provider source ZIPs; never execute their code.

ZIP/hash validation does not authenticate a publisher. Installation and catalogue
trust belong to the host; this tool deliberately cannot activate a package.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import stat
import sys
import zipfile

MAX_ARCHIVE = 16 * 1024 * 1024
MAX_EXPANDED = 32 * 1024 * 1024
MAX_FILE = 8 * 1024 * 1024
MAX_FILES = 512
EXTENSIONS = {".mjs", ".js", ".qml", ".json", ".png", ".jpg", ".jpeg", ".svg", ".webp", ".ttf", ".otf", ".txt", ".map"}
TEXT = {".mjs", ".js", ".qml", ".json", ".svg", ".txt", ".map"}
MODULES = {"QtQuick", "QtQuick.Controls", "QtQuick.Layouts", "QtQml", "Spool.Ui"}
PERMISSIONS = {"network:configured-origins", "storage:source", "credentials:source"}
CHANNELS = {"desktop", "webos", "android-direct", "google-play", "apple-bundled"}
NATIVE_MAGIC = (b"\x7fELF", b"MZ", b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\0asm")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def path_name(name: str) -> pathlib.PurePosixPath:
    if not name or "\\" in name or ":" in name or any(ord(c) < 32 for c in name):
        raise ValueError("invalid package path")
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or any(part in ("", ".", "..") for part in name.split("/")):
        raise ValueError("non-canonical package path")
    if any(part.rstrip(" .") != part for part in path.parts):
        raise ValueError("ambiguous package path")
    if any(part.split(".")[0].upper() in {"CON", "PRN", "AUX", "NUL", *[f"COM{i}" for i in range(10)], *[f"LPT{i}" for i in range(10)]} for part in path.parts):
        raise ValueError("reserved package path")
    return path


def validate_manifest(manifest: dict, paths: set[str]) -> None:
    required = {"format", "id", "version", "api", "entry", "publisher", "license", "offers", "requires", "optional", "permissions", "channels", "stateSchema", "ui"}
    if not isinstance(manifest, dict) or set(manifest) != required:
        raise ValueError("unknown or missing manifest fields")
    if manifest["format"] != 1 or manifest["api"] != "0.1":
        raise ValueError("unsupported manifest format or API revision")
    if not isinstance(manifest["id"], str) or not re.fullmatch(r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+", manifest["id"]):
        raise ValueError("invalid module identity")
    if not isinstance(manifest["version"], str) or not re.fullmatch(r"0\.\d+\.\d+(?:-[a-z0-9.-]+)?", manifest["version"]):
        raise ValueError("invalid experimental release version")
    if not isinstance(manifest["publisher"], str) or not manifest["publisher"] or not isinstance(manifest["license"], str) or not manifest["license"]:
        raise ValueError("publisher and license required")
    if type(manifest["stateSchema"]) is not int or manifest["stateSchema"] < 1:
        raise ValueError("invalid state schema")
    for key, known in (("permissions", PERMISSIONS), ("channels", CHANNELS)):
        value = manifest[key]
        if not isinstance(value, list) or not value or any(not isinstance(v, str) for v in value) or len(set(value)) != len(value) or not set(value) <= known:
            raise ValueError(f"unknown or duplicate {key}")
    for key in ("offers", "requires", "optional"):
        if not isinstance(manifest[key], dict) or any(not isinstance(name, str) or revision != "0.1" for name, revision in manifest[key].items()):
            raise ValueError(f"invalid extension declarations: {key}")
    # Offered unknown extensions are harmless. Required unknown host services
    # are not: reject rather than evaluating a module that cannot work.
    supported = {"http", "source-context", "native-list", "ui-host"}
    if not set(manifest["requires"]) <= supported:
        raise ValueError("unsupported required host extension")
    if not isinstance(manifest["entry"], str) or manifest["entry"] not in paths or not manifest["entry"].endswith(".mjs"):
        raise ValueError("missing JS entry point")
    ui = manifest["ui"]
    if not isinstance(ui, dict) or set(ui) != {"modules", "components"}:
        raise ValueError("invalid UI declaration")
    if not isinstance(ui["modules"], list) or any(not isinstance(v, str) for v in ui["modules"]) or not set(ui["modules"]) <= MODULES:
        raise ValueError("unsupported QML import")
    if not isinstance(ui["components"], list) or any(not isinstance(v, str) or v not in paths or not v.endswith(".qml") for v in ui["components"]):
        raise ValueError("missing UI component")
    if set(ui["components"]) != {p for p in paths if p.endswith(".qml")}:
        raise ValueError("every QML component must be declared for validation and warming")
    if "LICENSE" not in paths or "NOTICE" not in paths:
        raise ValueError("license and attribution notices required")


def validate_files(files: dict[str, bytes]) -> dict:
    if not files or len(files) > MAX_FILES or sum(map(len, files.values())) > MAX_EXPANDED:
        raise ValueError("expanded package limit")
    folded = set()
    for name, data in files.items():
        path = path_name(name)
        if name.casefold() in folded:
            raise ValueError("case-colliding paths")
        folded.add(name.casefold())
        if path.name not in {"LICENSE", "NOTICE"} and path.suffix.lower() not in EXTENSIONS:
            raise ValueError("unsupported payload type")
        if len(data) > MAX_FILE or data.startswith(NATIVE_MAGIC):
            raise ValueError("oversized or executable payload")
        if path.suffix.lower() in TEXT or path.name in {"LICENSE", "NOTICE"}:
            text = data.decode("utf-8")
            if "\x00" in text:
                raise ValueError("binary data in source")
    if "manifest.json" not in files:
        raise ValueError("missing manifest")
    manifest = json.loads(files["manifest.json"], object_pairs_hook=unique_object)
    validate_manifest(manifest, set(files))
    # Static checks are review aids, NOT a QML sandbox. Dynamic import/resource
    # loading still requires trusted code and host channel policy.
    for name, data in files.items():
        suffix = pathlib.PurePosixPath(name).suffix
        if suffix not in {".qml", ".js", ".mjs"}:
            continue
        text = data.decode("utf-8")
        for target in re.findall(r"(?:\bfrom\s*|\bimport\s*\(?\s*)['\"]([^'\"]+)['\"]", text):
            if not target.startswith("./") or ".." in target.split("/") or "\\" in target or ":" in target:
                raise ValueError("script imports must stay within the package directory")
            resolved = str(pathlib.PurePosixPath(name).parent / target[2:])
            if resolved not in files and not any(p.startswith(resolved + "/") for p in files):
                raise ValueError("missing local import")
        if suffix == ".qml":
            imports = re.findall(r"^\s*import\s+([A-Za-z][\w.]*)", text, re.MULTILINE)
            if not set(imports) <= set(manifest["ui"]["modules"]):
                raise ValueError("undeclared QML import")
    return manifest


def read_package(path: pathlib.Path) -> tuple[dict, dict[str, bytes]]:
    if path.stat().st_size > MAX_ARCHIVE:
        raise ValueError("archive size limit")
    files = {}
    total = 0
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_FILES:
            raise ValueError("archive entry limit")
        for entry in entries:
            path_name(entry.filename)
            mode = entry.external_attr >> 16
            if entry.is_dir() or stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)) or mode & 0o111:
                raise ValueError("only non-executable regular files are allowed")
            if entry.flag_bits & 1 or entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise ValueError("unsupported ZIP encoding")
            total += entry.file_size
            if entry.file_size > MAX_FILE or total > MAX_EXPANDED:
                raise ValueError("expanded archive size limit")
            if entry.filename in files:
                raise ValueError("duplicate archive path")
            with archive.open(entry) as member:
                data = member.read(MAX_FILE + 1)
            if len(data) != entry.file_size or len(data) > MAX_FILE:
                raise ValueError("invalid archive member size")
            files[entry.filename] = data
    return validate_files(files), files


def build(source: pathlib.Path, destination: pathlib.Path) -> dict:
    files = {}
    for root in ("manifest.json", "logic", "ui", "resources", "LICENSE", "NOTICE"):
        path = source / root
        if not path.exists():
            continue
        if path.is_symlink():
            raise ValueError("symlink in package source")
        candidates = [path] if path.is_file() else path.rglob("*")
        for entry in candidates:
            if entry.is_symlink():
                raise ValueError("symlink in package source")
            if entry.is_file():
                if entry.stat().st_size > MAX_FILE:
                    raise ValueError("source file size limit")
                files[entry.relative_to(source).as_posix()] = entry.read_bytes()
                if len(files) > MAX_FILES or sum(map(len, files.values())) > MAX_EXPANDED:
                    raise ValueError("source package size limit")
    manifest = validate_files(files)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IFREG | 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    read_package(destination)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("build", "validate"))
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    if args.command == "build":
        if not args.output:
            parser.error("build requires --output")
        manifest = build(args.path, args.output)
        archive = args.output
    else:
        manifest, _ = read_package(args.path)
        archive = args.path
    print(json.dumps({"id": manifest["id"], "version": manifest["version"], "size": archive.stat().st_size,
                      "sha256": hashlib.file_digest(archive.open("rb"), "sha256").hexdigest()}, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print(f"provider package rejected: {error}", file=sys.stderr)
        raise SystemExit(1)
