#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import plistlib
import runpy
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def run(script: Path, *args: str, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([sys.executable, str(script), *args], text=True, capture_output=True, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {' '.join(args)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


# Nix's compiler wrapper and dev shell add RPATHs (store paths, the shell's
# $out), which the audit rightly rejects; a packaged binary never has them.
COMPILE_ENV = {key: value for key, value in os.environ.items() if not key.startswith("NIX_LDFLAGS")}
COMPILE_ENV["NIX_DONT_SET_RPATH"] = "1"


def compile_fixture(root: Path) -> None:
    (root / "lib").mkdir()
    (root / "dep.c").write_text("int package_audit_dep(void) { return 42; }\n", encoding="utf-8")
    (root / "main.c").write_text(
        "extern int package_audit_dep(void); int main(void) { return package_audit_dep() != 42; }\n",
        encoding="utf-8",
    )
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-Wl,-soname,libpackage-audit-dep.so.1", "-o",
         str(root / "lib/libpackage-audit-dep.so.1"), str(root / "dep.c")],
        check=True, env=COMPILE_ENV,
    )
    os.symlink("libpackage-audit-dep.so.1", root / "lib/libpackage-audit-dep.so")
    subprocess.run(
        ["cc", "-o", str(root / "app"), str(root / "main.c"), f"-L{root / 'lib'}",
         "-lpackage-audit-dep", "-Wl,-rpath,$ORIGIN/lib"],
        check=True, env=COMPILE_ENV,
    )
    subprocess.run(["strip", "--strip-unneeded", str(root / "app"), str(root / "lib/libpackage-audit-dep.so.1")],
                   check=True)


def macho_fixture(*, cpu: int = 0x0100000C, platform: int = 3, kind: int = 2,
                  dependency: str | None = None, rpath: str | None = None) -> bytes:
    # Synthetic bytes exercise the package protocol, not Apple runtime behavior.
    commands = [struct.pack("<6I", 0x32, 24, platform, 0x100000, 0x100000, 0)]
    for command, value, minimum in ((0xC, dependency, 24), (0x8000001C, rpath, 12)):
        if value is not None:
            string = value.encode() + b"\0"
            size = (minimum + len(string) + 7) & ~7
            data = struct.pack("<3I", command, size, minimum).ljust(minimum, b"\0") + string
            commands.append(data.ljust(size, b"\0"))
    payload = b"".join(commands)
    return struct.pack("<8I", 0xFEEDFACF, cpu, 0, kind, len(commands), len(payload), 0, 0) + payload


def ipa_fixture(path: Path, *, version: str = "0.9.0", binary: bytes | None = None,
                metadata: dict | None = None, extras: tuple = ()) -> bytes:
    info = {
        "CFBundleExecutable": "Spool", "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": version, "CFBundleVersion": version,
        "CFBundleSupportedPlatforms": ["AppleTVOS"], "UIDeviceFamily": [3],
    }
    info.update(metadata or {})
    binary = macho_fixture() if binary is None else binary
    with zipfile.ZipFile(path, "w") as archive:
        for name, data, mode in (
            ("Payload/Spool.app/Info.plist", plistlib.dumps(info, fmt=plistlib.FMT_BINARY), 0o100644),
            ("Payload/Spool.app/Spool", binary, 0o100755),
            ("Payload/Spool.app/resources/data", b"bundled-resource", 0o100644),
            *extras,
        ):
            entry = zipfile.ZipInfo(name)
            entry.create_system = 3
            entry.external_attr = mode << 16
            archive.writestr(entry, data)
    return binary


def tvos_contract(script: Path) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        ipa = root / "Spool-0.9.0-tvOS-arm64.ipa"
        extracted = root / "extracted"
        binary = ipa_fixture(ipa, extras=(
            ("Payload/Spool.app/resources/alias", b"data", stat.S_IFLNK | 0o777),
        ))
        result = run(script, "tvos-ipa", str(ipa), "--version", "0.9.0", "--extract", str(extracted))
        assert "signing/provisioning and runtime not verified" in result.stdout
        app = extracted / "Payload/Spool.app"
        assert (app / "Spool").read_bytes() == binary
        assert (app / "Spool").stat().st_mode & 0o111
        assert (app / "resources/alias").is_symlink()
        assert (app / "resources/alias").read_bytes() == b"bundled-resource"
        nonempty = run(script, "tvos-ipa", str(ipa), "--version", "0.9.0",
                       "--extract", str(extracted), expected=1)
        assert "destination must be empty" in nonempty.stderr

        cases = (
            ({"version": "0.8.0"}, "embedded bundle version"),
            ({"metadata": {"CFBundleVersion": "99"}}, "embedded bundle version"),
            ({"metadata": {"CFBundleSupportedPlatforms": ["AppleTVSimulator"]}}, "AppleTVOS"),
            ({"metadata": {"UIDeviceFamily": [1, 2]}}, "device family"),
            ({"metadata": {"CFBundleExecutable": "../Spool"}}, "Spool application executable"),
            ({"binary": b"not Mach-O"}, "not Mach-O"),
            ({"binary": macho_fixture(cpu=0x01000007)}, "architecture"),
            ({"binary": macho_fixture(platform=8)}, "device tvOS"),
            ({"binary": macho_fixture(platform=2)}, "device tvOS"),
            ({"binary": macho_fixture(kind=6)}, "not an executable"),
            ({"binary": macho_fixture()[:-1]}, "load command bounds"),
            ({"binary": macho_fixture(dependency="@rpath/missing.dylib",
                                      rpath="@executable_path/Frameworks")}, "unresolved Mach-O dependency"),
            ({"binary": macho_fixture(dependency="/nix/store/build/lib.dylib")}, "forbidden Mach-O dependency"),
            ({"extras": (("../escape", b"bad", 0o100644),)}, "unsafe or unexpected IPA member"),
            ({"extras": (("Payload/Spool.app/../../escape", b"bad", 0o100644),)}, "unsafe or unexpected"),
            ({"extras": (("/Payload/Spool.app/escape", b"bad", 0o100644),)}, "unsafe or unexpected"),
            ({"extras": (("Payload/Other.app/file", b"bad", 0o100644),)}, "unsafe or unexpected"),
            ({"extras": (("Payload/Spool.app/Spool", b"duplicate", 0o100644),)}, "duplicate IPA member"),
            ({"extras": (("Payload/Spool.app/embedded.mobileprovision", b"profile", 0o100644),)}, "unsigned/unprovisioned"),
            ({"extras": (("Payload/Spool.app/_CodeSignature/CodeResources", b"signature", 0o100644),)},
             "unsigned/unprovisioned"),
            ({"extras": (("Payload/Spool.app/resources/link", b"/tmp/outside", stat.S_IFLNK | 0o777),)},
             "unsafe IPA symlink"),
            ({"extras": (("Payload/Spool.app/resources/link", b"../../../escape", stat.S_IFLNK | 0o777),)},
             "escaping or dangling IPA symlink"),
            ({"extras": (("Payload/Spool.app/resources/link", b"missing", stat.S_IFLNK | 0o777),)},
             "escaping or dangling IPA symlink"),
            ({"extras": (("Payload/Spool.app/resources/link", b"data", stat.S_IFLNK | 0o777),
                         ("Payload/Spool.app/resources/link/file", b"bad", 0o100644))},
             "non-directory IPA ancestor"),
            ({"extras": (("Payload/Spool.app/orphan.dylib", macho_fixture(kind=6), 0o100755),)},
             "unreferenced packaged Mach-O"),
        )
        for options, reason in cases:
            ipa_fixture(ipa, **options)
            rejected = run(script, "tvos-ipa", str(ipa), "--version", "0.9.0", expected=1)
            assert reason in rejected.stderr, (options, rejected.stderr)
        dependency = "@rpath/helper.dylib"
        ipa_fixture(ipa, binary=macho_fixture(dependency=dependency, rpath="@executable_path/Frameworks"),
                    extras=(("Payload/Spool.app/Frameworks/helper.dylib", macho_fixture(kind=6), 0o100755),))
        run(script, "tvos-ipa", str(ipa), "--version", "0.9.0")
        ipa_fixture(ipa, binary=macho_fixture(dependency="/System/Library/Frameworks/UIKit.framework/UIKit"))
        run(script, "tvos-ipa", str(ipa), "--version", "0.9.0")
        wrong = root / "Spool-0.9.0-tvOS-simulator-arm64.ipa"
        ipa.rename(wrong)
        rejected = run(script, "tvos-ipa", str(wrong), "--version", "0.9.0", expected=1)
        assert "named Spool-0.9.0-tvOS-arm64.ipa" in rejected.stderr


def website_contract(script: Path) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        assets = root / "assets"
        assets.mkdir()
        # Independent public release contract; deliberately not imported from
        # the generator's inventory so missing platform declarations fail.
        names = (
            "com.sachk.spool_0.9.0_arm.ipk",
            "spool-universal.apk", "spool-arm64-v8a.apk", "spool-armeabi-v7a.apk", "spool-x86_64.apk",
            "Spool-0.9.0-Windows-x64-Setup.exe", "Spool-0.9.0-Windows-x64-Portable.exe",
            "Spool-0.9.0-macOS-arm64.dmg", "Spool-0.9.0-macOS-x86_64.dmg",
            "Spool-0.9.0-x86_64.AppImage", "Spool-0.9.0-linux-x86_64.tar.zst",
            "spool-bin-0.9.0-1-x86_64.pkg.tar.zst", "Spool-0.9.0-tvOS-arm64.ipa",
        )
        for name in names:
            (assets / name).write_bytes(b"package-protocol-fixture")
        output = root / "website-release.json"
        args = (str(assets), str(output), "--tag", "v0.9.0")
        run(script, *args)
        manifest = json.loads(output.read_text())
        platforms = {item["id"]: item for item in manifest["release"]["platforms"]}
        assert list(platforms) == ["webos", "android", "windows", "macos", "linux", "tvos"]
        downloads = [item for platform in platforms.values() for item in platform["downloads"]]
        assert {item["name"] for item in downloads} == set(names)
        tvos = platforms["tvos"]["downloads"][0]
        assert tvos["architecture"] == "arm64" and tvos["format"] == "ipa"
        assert tvos["signing"] == "unsigned" and "signing required" in tvos["label"]
        assert "provisioning" in tvos["note"] and "Not on the App Store" in tvos["note"]
        assert tvos["sha256"] == hashlib.sha256(b"package-protocol-fixture").hexdigest()
        assert tvos["size"] == len(b"package-protocol-fixture")
        assert tvos["url"].endswith("/v0.9.0/Spool-0.9.0-tvOS-arm64.ipa")
        publisher_contract(script.with_name("publish-website-metadata.py"), output.read_bytes())
        for name in names:
            package = assets / name
            package.unlink()
            rejected = run(script, *args, expected=1)
            assert "expected exactly one" in rejected.stderr, rejected.stderr
            package.write_bytes(b"package-protocol-fixture")
        for name in ("Spool-0.9.0-tvOS-simulator-arm64.ipa", "Spool-0.8.0-tvOS-arm64.ipa",
                     "new-platform.ipa", "new-platform.apk"):
            extra = assets / name
            extra.write_bytes(b"extra")
            rejected = run(script, *args, expected=1)
            assert "omitted from website metadata" in rejected.stderr
            extra.unlink()
        tvos_file = assets / tvos["name"]
        tvos_file.write_bytes(b"")
        assert "empty" in run(script, *args, expected=1).stderr
        tvos_file.unlink()
        tvos_file.symlink_to(assets / names[0])
        assert "regular final package" in run(script, *args, expected=1).stderr


def workflow_artifact_contract(script: Path) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        tools = root / "tools"
        workflows = root / ".github/workflows"
        tools.mkdir()
        workflows.mkdir(parents=True)
        checker = tools / script.name
        shutil.copyfile(script, checker)
        release = workflows / "release.yml"
        release.write_text("pattern: spool-*\n")
        for names, expected, reason in (
            (("spool-linux", "spool-tvos-device-arm64", "internal-tvos-simulator-arm64"), 0, "checked"),
            (("spool-linux",), 1, "exactly one spool-tvos-device-arm64"),
            (("spool-tvos-device-arm64", "spool-tvos-simulator-arm64"), 1, "must be internal-*"),
            (("spool-tvos-device-arm64", "spool-tvos-device-arm64"), 1, "exactly one"),
            (("spool-tvos-device-arm64", "loose-build"), 1, "neither spool-*"),
        ):
            (workflows / "build-artifacts.yml").write_text("".join(
                f"- uses: actions/upload-artifact@v4\n  with:\n    name: {name}\n" for name in names
            ))
            result = subprocess.run(["bash", str(checker)], text=True, capture_output=True)
            assert result.returncode == expected and reason in result.stdout + result.stderr, result
        release.write_text("pattern: *\n")
        result = subprocess.run(["bash", str(checker)], text=True, capture_output=True)
        assert result.returncode == 1 and "must ask for spool-*" in result.stderr


def publisher_contract(script: Path, manifest: bytes) -> None:
    publisher = runpy.run_path(str(script))
    document = json.loads(manifest)
    historical = json.loads(manifest)
    historical["release"].update(version="0.8.11", tag="v0.8.11",
                                 releaseUrl="https://github.com/spool-player/spool/releases/tag/v0.8.11")
    historical["release"]["platforms"] = [
        item for item in historical["release"]["platforms"] if item["id"] != "tvos"
    ]
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null",
               GIT_AUTHOR_NAME="Package contract", GIT_AUTHOR_EMAIL="fixture@example.invalid",
               GIT_COMMITTER_NAME="Package contract", GIT_COMMITTER_EMAIL="fixture@example.invalid")
    env = {key: value for key, value in env.items() if not key.startswith("GIT_TRACE")}
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)

        def git(*args: str, cwd: Path = root) -> bytes:
            result = subprocess.run(["git", "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                                     *args], cwd=cwd, env=env, capture_output=True, check=True)
            return result.stdout

        seed = root / "seed"
        git("init", "-b", "main", str(seed))
        (seed / "data").mkdir()
        (seed / "data/release.json").write_text(json.dumps(historical))
        (seed / "unrelated.txt").write_bytes(b"untouched-private-site-file")
        git("add", ".", cwd=seed)
        git("commit", "-m", "Historical five-platform snapshot", cwd=seed)
        origin = root / "origin.git"
        work = root / "work.git"
        git("clone", "--bare", str(seed), str(origin))
        git("clone", "--bare", str(origin), str(work))
        publisher["update_repository"](work, env, manifest, "v0.9.0")
        assert git("show", "main:data/release.json", cwd=origin) == manifest
        assert git("show", "main:unrelated.txt", cwd=origin) == b"untouched-private-site-file"
        # Plumbing publishes a child commit without moving its bare checkout HEAD.
        git("fetch", "origin", "main:main", cwd=work)
        head = git("rev-parse", "main", cwd=origin)
        publisher["update_repository"](work, env, manifest, "v0.9.0")
        assert git("rev-parse", "main", cwd=origin) == head
        older = json.loads(manifest)
        older["release"].update(version="0.8.12", tag="v0.8.12",
                                releaseUrl="https://github.com/spool-player/spool/releases/tag/v0.8.12")
        publisher["update_repository"](work, env, json.dumps(older).encode(), "v0.8.12")
        assert git("rev-parse", "main", cwd=origin) == head
        conflict = json.loads(manifest)
        conflict["release"]["platforms"][0]["label"] = "different same-version metadata"
        incomplete = json.loads(manifest)
        incomplete["release"]["platforms"] = [
            item for item in incomplete["release"]["platforms"] if item["id"] != "tvos"
        ]
        for payload, tag, reason in (
            (json.dumps(conflict).encode(), "v0.9.0", "different bytes for the same release"),
            (json.dumps(incomplete).encode(), "v0.9.0", "expected all six"),
            (manifest, "v0.8.11", "trusted release tag"),
        ):
            try:
                publisher["update_repository"](work, env, payload, tag)
            except ValueError as error:
                assert reason in str(error), error
            else:
                raise AssertionError("invalid publication was accepted")
            assert git("rev-parse", "main", cwd=origin) == head


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: PackageAuditTest.py <package-audit.py>")
    script = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "one").write_bytes(b"duplicate")
        (root / "nested").mkdir()
        (root / "nested/two").write_bytes(b"duplicate")
        inventory = run(script, "inventory", str(root))
        assert inventory.stdout.splitlines() == sorted(inventory.stdout.splitlines())
        assert "UNIQUE_REGULAR_BYTES\t9" in inventory.stderr
        assert "DUPLICATE\t" in inventory.stderr
        os.symlink("/outside", root / "escape")
        escaping = run(script, "inventory", str(root), expected=1)
        assert "ESCAPING_SYMLINK\tescape" in escaping.stderr

    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        compile_fixture(root)
        run(script, "elf", str(root), "--root", "app", "--allow-system", "libc.so.6")
        orphan = root / "lib/liborphan.so.1"
        subprocess.run(
            ["cc", "-shared", "-fPIC", "-Wl,-soname,liborphan.so.1", "-o", str(orphan), str(root / "dep.c")],
            check=True, env=COMPILE_ENV,
        )
        subprocess.run(["strip", "--strip-unneeded", str(orphan)], check=True)
        unreachable = run(
            script, "elf", str(root), "--root", "app", "--allow-system", "libc.so.6", expected=1
        )
        assert "UNREACHABLE\tlib/liborphan.so.1" in unreachable.stdout
        orphan.unlink()
        (root / "lib/libpackage-audit-dep.so").unlink()
        run(script, "elf", str(root), "--root", "app", "--allow-system", "libc.so.6")
        # The loader finds libpackage-audit-dep.so.1 as a file; a copy under
        # another name with that SONAME does not satisfy it.
        (root / "lib/libpackage-audit-dep.so.1").rename(root / "lib/libpackage-audit-dep.so")
        renamed = run(script, "elf", str(root), "--root", "app", "--allow-system", "libc.so.6", expected=1)
        assert "missing ELF dependency" in renamed.stderr
        (root / "lib/libpackage-audit-dep.so").unlink()
        missing = run(script, "elf", str(root), "--root", "app", "--allow-system", "libc.so.6", expected=1)
        assert "missing ELF dependency" in missing.stderr
    tvos_contract(script)
    website_contract(script.with_name("generate-website-manifest.py"))
    workflow_artifact_contract(script.with_name("check-release-artifacts.sh"))
    return 0



if __name__ == "__main__":
    raise SystemExit(main())
