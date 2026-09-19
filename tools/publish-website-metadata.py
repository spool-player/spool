#!/usr/bin/env python3

"""Commit the exact public release manifest without checking out private code."""

import argparse
import json
import os
import re
import shlex
import subprocess
import tempfile
from pathlib import Path


WEBSITE_REMOTE = "git@github.com:spool-player/website.git"
# GitHub's Ed25519 host key, verified through https://api.github.com/meta.
GITHUB_HOST_KEY = "github.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl\n"
MANIFEST_PATH = "data/release.json"


def release_version(manifest: bytes) -> tuple[int, int, int]:
    try:
        document = json.loads(manifest)
        release = document["release"]
        version = release["version"]
        valid = (
            type(document["schemaVersion"]) is int
            and document["schemaVersion"] == 1
            and isinstance(version, str)
            and re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", version)
            and release["tag"] == f"v{version}"
            and release["releaseUrl"] == f"https://github.com/spool-player/spool/releases/tag/v{version}"
            and [platform["id"] for platform in release["platforms"]]
            == ["webos", "android", "windows", "macos", "linux"]
        )
    except (ValueError, KeyError, TypeError):
        raise ValueError("invalid release snapshot") from None
    if not valid:
        raise ValueError("invalid release snapshot")
    return tuple(int(component) for component in version.split("."))


def git(repository: Path, env: dict, *args: str, data: bytes | None = None) -> bytes:
    # stdout/stderr may contain private paths, code, or remote error details.
    # Never forward them to the public release log, even on failure.
    try:
        result = subprocess.run(
            ["git", "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", *args],
            cwd=repository,
            env=env,
            input=data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=180,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise ValueError(f"git {args[0]} timed out; private output withheld") from None
    if result.returncode:
        raise ValueError(f"git {args[0]} failed; private output withheld")
    return result.stdout


def update_repository(repository: Path, env: dict, manifest: bytes, tag: str) -> None:
    incoming = release_version(manifest)
    if tag != "v" + ".".join(str(component) for component in incoming):
        raise ValueError("release snapshot does not match the trusted release tag")
    parent = git(repository, env, "rev-parse", "HEAD").strip().decode("ascii")
    entry = git(repository, env, "ls-tree", "-z", parent, "--", MANIFEST_PATH)
    if entry:
        metadata, separator, path = entry.removesuffix(b"\0").partition(b"\t")
        fields = metadata.split()
        if not separator or path != MANIFEST_PATH.encode() or len(fields) != 3 or fields[:2] != [b"100644", b"blob"]:
            raise ValueError("website release snapshot is not a regular file")
        previous = git(repository, env, "cat-file", "blob", fields[2].decode("ascii"))
        if previous == manifest:
            print("Website already contains the exact public release snapshot; unchanged")
            return
        current = release_version(previous)
        if current > incoming:
            print("Website already contains a newer release; unchanged")
            return
        if current == incoming:
            raise ValueError("website already contains different bytes for the same release version")
    # A bare clone plus Git plumbing never checks out files, honors attributes,
    # runs a repository script, or exposes private content in the public log.
    git(repository, env, "read-tree", parent)
    blob = git(repository, env, "hash-object", "-w", "--stdin", data=manifest).strip().decode("ascii")
    git(repository, env, "update-index", "--add", "--cacheinfo", "100644", blob, MANIFEST_PATH)
    tree = git(repository, env, "write-tree").strip().decode("ascii")
    commit = git(repository, env, "commit-tree", tree, "-p", parent,
                 data=f"chore(release): publish {tag} download metadata\n".encode()).strip().decode("ascii")
    # Never force: a concurrent main update must fail visibly, not be overwritten.
    git(repository, env, "push", "--quiet", "origin", f"{commit}:refs/heads/main")
    print(f"Published exact public release {tag} to the website snapshot")


def publish(manifest: bytes, tag: str, deploy_key: str) -> None:
    if not deploy_key.strip():
        raise ValueError("WEBSITE_DEPLOY_KEY is required; configure the website-only write deploy key")
    incoming = release_version(manifest)
    if tag != "v" + ".".join(str(component) for component in incoming):
        raise ValueError("release snapshot does not match the trusted release tag")
    with tempfile.TemporaryDirectory(prefix="spool-website-publish-") as temporary:
        work = Path(temporary)
        key_path = work / "deploy-key"
        key_path.write_text(deploy_key.rstrip() + "\n", encoding="utf-8")
        key_path.chmod(0o600)
        known_hosts = work / "known_hosts"
        known_hosts.write_text(GITHUB_HOST_KEY, encoding="ascii")
        # Do not inherit tracing/configuration that could log private Git data,
        # and never pass the secret itself to any child process environment.
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("GIT_") and key != "WEBSITE_DEPLOY_KEY"}
        env.update({
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_AUTHOR_NAME": "github-actions[bot]",
            "GIT_AUTHOR_EMAIL": "41898282+github-actions[bot]@users.noreply.github.com",
            "GIT_COMMITTER_NAME": "github-actions[bot]",
            "GIT_COMMITTER_EMAIL": "41898282+github-actions[bot]@users.noreply.github.com",
            "GIT_SSH_COMMAND": shlex.join([
                "ssh", "-F", "/dev/null", "-i", str(key_path),
                "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o", "IdentityAgent=none",
                "-o", "StrictHostKeyChecking=yes", "-o", "HostKeyAlgorithms=ssh-ed25519",
                "-o", f"UserKnownHostsFile={known_hosts}", "-o", "GlobalKnownHostsFile=/dev/null",
                "-o", "UpdateHostKeys=no", "-o", "LogLevel=ERROR",
            ]),
        })
        repository = work / "website.git"
        git(work, env, "clone", "--quiet", "--bare", "--depth", "1", "--single-branch", "--branch", "main",
            WEBSITE_REMOTE, str(repository))
        update_repository(repository, env, manifest, tag)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    deploy_key = os.environ.pop("WEBSITE_DEPLOY_KEY", "")
    publish(args.manifest.read_bytes(), args.tag, deploy_key)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"website metadata publication failed: {error}") from None
