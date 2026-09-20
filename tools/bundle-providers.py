#!/usr/bin/env python3
"""Verify repository-pinned source packages and materialise Qt build resources.

This is build tooling, not an application installer. Trusted repository pins
supply the expected bytes; no mutable latest URL or package code is executed.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location("provider_package", pathlib.Path(__file__).with_name("provider-package.py"))
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def materialize(lock_path, output, override=None):
    lock = json.loads(lock_path.read_text(), object_pairs_hook=package.unique_object)
    if set(lock) != {"format", "providers"} or lock["format"] != 1 or not isinstance(lock["providers"], list):
        raise ValueError("unsupported provider lock")
    root = ET.Element("RCC")
    seen = set()
    for pin in lock["providers"]:
        if set(pin) != {"id", "version", "repository", "revision", "archive", "size", "sha256"}:
            raise ValueError("invalid provider pin")
        if not re.fullmatch(r"[0-9a-f]{40}", pin["revision"]) or not re.fullmatch(r"[0-9a-f]{64}", pin["sha256"]):
            raise ValueError("invalid immutable source identity")
        archive = lock_path.parent / str(package.path_name(pin["archive"]))
        using_override = pin["id"] == "spool.jellyfin" and override is not None
        if using_override:
            archive = output / "override.zip"
            package.build(override, archive)
        else:
            if archive.stat().st_size != pin["size"]:
                raise ValueError("pinned package size mismatch")
            with archive.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            if digest != pin["sha256"]:
                raise ValueError("pinned package digest mismatch")
        manifest, files = package.read_package(archive)
        if manifest["id"] != pin["id"] or (not using_override and manifest["version"] != pin["version"]):
            raise ValueError("package identity does not match pin")
        if manifest["id"] in seen:
            raise ValueError("duplicate module in bundle")
        seen.add(manifest["id"])
        resource = ET.SubElement(root, "qresource", prefix="/providers/" + manifest["id"])
        directory = output / manifest["id"]
        for name, data in sorted(files.items()):
            target = directory / name
            target.parent.mkdir(parents=True, exist_ok=True)
            # Preserve timestamps for identical contents in incremental builds.
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
            ET.SubElement(resource, "file", alias=name).text = str(target.resolve())
    output.mkdir(parents=True, exist_ok=True)
    qrc = output / "providers.qrc"
    data = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    if not qrc.exists() or qrc.read_bytes() != data:
        qrc.write_bytes(data)
    return qrc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("lock", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--jellyfin-source", type=pathlib.Path)
    args = parser.parse_args()
    print(materialize(args.lock, args.output, args.jellyfin_source))


if __name__ == "__main__":
    main()
