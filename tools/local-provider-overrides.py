#!/usr/bin/env python3
"""Discover validated provider checkouts immediately beside a Spool checkout."""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import sys

SPEC = importlib.util.spec_from_file_location(
    "spool_provider", pathlib.Path(__file__).resolve().parents[1] / "sdk/spool-provider.py"
)
SDK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SDK)
MAX_DIRECTORIES = 200


def required_file(checkout: pathlib.Path, name: str) -> bytes:
    if not SDK.valid_path(name) or name.split("/")[0] not in SDK.ROOTS:
        raise ValueError(f"forbidden or unpackaged path: {name}")
    path = checkout
    for part in name.split("/"):
        path /= part
        if path.is_symlink():
            raise ValueError(f"symlinks are not allowed: {name}")
    if not path.is_file():
        raise ValueError(f"required file is missing: {name}")
    with path.open("rb") as stream:
        data = stream.read(SDK.MAX_FILE + 1)
    if len(data) > SDK.MAX_FILE:
        raise ValueError(f"oversized file: {name}")
    return data


def provider_id(checkout: pathlib.Path) -> str:
    files = {"manifest.json": required_file(checkout, "manifest.json")}
    manifest = json.loads(files["manifest.json"])
    if isinstance(manifest, dict):
        references = [manifest.get("entry"), manifest.get("icon")]
        ui = manifest.get("ui", {})
        if isinstance(ui, dict):
            references.extend(ui.values())
        for name in references:
            if isinstance(name, str) and name:
                files[name] = required_file(checkout, name)
    # Reuse the SDK contract without crawling unrelated files or entire workspaces.
    # Full package validation remains the SDK's job; only declared files are read here.
    return SDK.validate(files)["id"]


def discover(root: pathlib.Path) -> str:
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError(f"Spool root is not a directory: {root}")
    directories = []
    for path in root.parent.iterdir():
        if path.is_dir():
            directories.append(path)
            if len(directories) > MAX_DIRECTORIES:
                raise ValueError(
                    f"workspace {root.parent} contains more than {MAX_DIRECTORIES} immediate directories; "
                    "put Spool and its provider checkouts in a smaller workspace"
                )
    providers = {}
    for checkout in sorted(directories):
        if checkout == root:
            continue
        manifest = checkout / "manifest.json"
        if not manifest.exists() and not manifest.is_symlink():
            continue
        try:
            if any(character in str(checkout) for character in ";\r\n"):
                raise ValueError("checkout paths cannot contain semicolons or newlines")
            identifier = provider_id(checkout)
        except (OSError, ValueError, TypeError) as error:
            raise ValueError(f"invalid provider candidate {checkout}: {error}") from error
        if identifier in providers:
            raise ValueError(f"duplicate provider ID {identifier}: {providers[identifier]} and {checkout}")
        providers[identifier] = checkout
    return ";".join(f"{identifier}={providers[identifier]}" for identifier in sorted(providers))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=pathlib.Path, help="Spool checkout root")
    arguments = parser.parse_args()
    try:
        print(discover(arguments.root))
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
