#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import plistlib
import re
import stat
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Iterable


class AuditError(RuntimeError):
    pass


def run_tool(*args: str) -> str:
    result = subprocess.run(
        args, text=True, encoding="utf-8", errors="replace", capture_output=True, check=False
    )
    if result.returncode:
        raise AuditError(f"{' '.join(args)} failed:\n{result.stderr.strip()}")
    return result.stdout


def relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inventory_rows(root: Path) -> tuple[list[tuple[str, str, int, str]], dict[str, list[str]], list[str]]:
    rows: list[tuple[str, str, int, str]] = []
    hashes: dict[str, list[str]] = defaultdict(list)
    escaping: list[str] = []
    root_real = root.resolve()
    for path in sorted(root.rglob("*"), key=lambda item: relative(item, root)):
        rel = relative(path, root)
        mode = path.lstat().st_mode
        if stat.S_ISLNK(mode):
            target = os.readlink(path)
            rows.append((rel, "symlink", 0, target))
            resolved = (path.parent / target).resolve()
            if resolved != root_real and root_real not in resolved.parents:
                escaping.append(rel)
        elif stat.S_ISREG(mode):
            size = path.stat().st_size
            digest = sha256(path)
            rows.append((rel, "file", size, digest))
            hashes[digest].append(rel)
        elif stat.S_ISDIR(mode):
            rows.append((rel, "directory", 0, "-"))
        else:
            rows.append((rel, "other", 0, "-"))
    return rows, hashes, escaping


def inventory(args: argparse.Namespace) -> int:
    root = args.root.resolve()
    if not root.is_dir():
        raise AuditError(f"inventory root is not a directory: {root}")
    rows, hashes, escaping = inventory_rows(root)
    text = "".join(f"{rel}\t{kind}\t{size}\t{value}\n" for rel, kind, size, value in rows)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)

    totals: dict[str, int] = defaultdict(int)
    sizes = {rel: size for rel, kind, size, _ in rows if kind == "file"}
    for rel, kind, size, _ in rows:
        if kind == "file":
            totals[rel.split("/", 1)[0]] += size
    for directory, size in sorted(totals.items()):
        print(f"DIRECTORY\t{directory}\t{size}", file=sys.stderr)
    unique_bytes = sum(sizes[paths[0]] for paths in hashes.values())
    print(f"UNIQUE_REGULAR_BYTES\t{unique_bytes}", file=sys.stderr)
    for digest, paths in sorted(hashes.items()):
        if len(paths) > 1:
            print(f"DUPLICATE\t{digest}\t" + "\t".join(paths), file=sys.stderr)
    for rel in escaping:
        print(f"ESCAPING_SYMLINK\t{rel}", file=sys.stderr)
    if escaping:
        raise AuditError(f"inventory contains {len(escaping)} symlink(s) escaping {root}")
    return 0


@dataclass(frozen=True)
class ElfInfo:
    path: Path
    soname: str | None
    needed: tuple[str, ...]
    runpaths: tuple[str, ...]
    sections: tuple[str, ...]


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def elf_info(path: Path, readelf: str) -> ElfInfo:
    dynamic = run_tool(readelf, "-W", "-d", str(path))
    needed = tuple(re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic))
    sonames = re.findall(r"\(SONAME\).*?\[(.*?)\]", dynamic)
    runpaths: list[str] = []
    for value in re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic):
        runpaths.extend(part for part in value.split(":") if part)
    section_text = run_tool(readelf, "-W", "-S", str(path))
    sections = tuple(re.findall(r"\[\s*\d+\]\s+(\S+)", section_text))
    return ElfInfo(path, sonames[0] if sonames else None, needed, tuple(runpaths), sections)


def allowed_system(name: str, allowed: Iterable[str]) -> bool:
    return name in allowed


def audit_elf(args: argparse.Namespace) -> int:
    root = args.root.resolve()
    if not root.is_dir():
        raise AuditError(f"ELF root is not a directory: {root}")
    readelf = args.readelf or "readelf"
    regular_elfs = [path for path in root.rglob("*") if path.is_file() and not path.is_symlink() and is_elf(path)]
    infos = {path.resolve(): elf_info(path, readelf) for path in regular_elfs}
    providers: dict[str, set[Path]] = defaultdict(set)
    # The dynamic loader finds a NEEDED name as a file, never by SONAME.
    for path in infos:
        providers[path.name].add(path)
    for link in root.rglob("*"):
        if link.is_symlink() and is_elf(link):
            providers[link.name].add(link.resolve())

    errors: list[str] = []
    for info in infos.values():
        for runpath in info.runpaths:
            lower = runpath.lower()
            if runpath.startswith("/") or "/nix/store/" in lower or re.search(r"(^|/)build(/|$)", lower):
                errors.append(f"forbidden RPATH/RUNPATH: {relative(info.path, root)} -> {runpath}")
        forbidden_sections = sorted(section for section in info.sections if section == ".symtab" or section.startswith(".debug_"))
        if forbidden_sections:
            errors.append(f"unstripped ELF: {relative(info.path, root)} -> {', '.join(forbidden_sections)}")

    roots: list[Path] = []
    for rel in args.root_binary:
        candidate = (root / rel).resolve()
        if candidate not in infos:
            errors.append(f"ELF root is missing or not a regular ELF: {rel}")
        else:
            roots.append(candidate)

    reachable: set[Path] = set()
    queue = deque(roots)
    while queue:
        path = queue.popleft()
        if path in reachable:
            continue
        reachable.add(path)
        for name in infos[path].needed:
            matches = providers.get(name, set())
            if len(matches) > 1:
                errors.append(
                    f"ambiguous ELF provider: {relative(path, root)} -> {name}: "
                    + ", ".join(sorted(relative(match, root) for match in matches))
                )
            elif len(matches) == 1:
                queue.append(next(iter(matches)))
            elif not allowed_system(name, args.allow_system):
                errors.append(f"missing ELF dependency: {relative(path, root)} -> {name}")

    unreachable = sorted((path for path in infos if path not in reachable), key=lambda item: relative(item, root))
    for path in unreachable:
        print(f"UNREACHABLE\t{relative(path, root)}")
    if unreachable:
        errors.append(f"unreachable packaged ELFs: {len(unreachable)}")
    if errors:
        raise AuditError("ELF audit failed:\n" + "\n".join(sorted(set(errors))))
    print(f"ELF closure passed for {len(infos)} files.", file=sys.stderr)
    return 0


@dataclass(frozen=True)
class MachOInfo:
    path: Path
    architectures: frozenset[str]
    kind: str
    install_name: str | None
    dependencies: tuple[str, ...]
    rpaths: tuple[str, ...]


def macho_architectures(path: Path, file_tool: str, lipo: str) -> tuple[frozenset[str], str]:
    description = run_tool(file_tool, "-b", str(path))
    if "Mach-O" not in description:
        return frozenset(), description
    result = subprocess.run([lipo, "-archs", str(path)], text=True, capture_output=True, check=False)
    if result.returncode:
        match = re.search(r"Mach-O \S+ (?:executable|dynamically linked shared library|bundle) (\S+)", description)
        return (frozenset(match.groups()) if match else frozenset()), description
    return frozenset(result.stdout.split()), description


def macho_info(path: Path, file_tool: str, lipo: str, otool: str) -> MachOInfo | None:
    architectures, description = macho_architectures(path, file_tool, lipo)
    if not architectures:
        return None
    load_commands = run_tool(otool, "-l", str(path))
    rpaths = tuple(re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (\S+) \(offset", load_commands))
    lines = run_tool(otool, "-L", str(path)).splitlines()[1:]
    names = tuple(line.strip().split(" (compatibility", 1)[0] for line in lines if line.strip())
    is_library = "dynamically linked shared library" in description
    install_name = names[0] if is_library and names else None
    dependencies = names[1:] if install_name else names
    if "executable" in description:
        kind = "executable"
    elif "bundle" in description:
        kind = "bundle"
    else:
        kind = "library"
    return MachOInfo(path, architectures, kind, install_name, dependencies, rpaths)


def expand_macho_name(name: str, owner: MachOInfo, executable_dir: Path) -> Path | None:
    if name.startswith("@loader_path/"):
        return (owner.path.parent / name.removeprefix("@loader_path/")).resolve()
    if name.startswith("@executable_path/"):
        return (executable_dir / name.removeprefix("@executable_path/")).resolve()
    if name.startswith("@"):
        return None
    return Path(name).resolve()


def tvos_macho_info(path: Path) -> MachOInfo | None:
    """Read device load commands directly, including on Linux release runners."""
    with path.open("rb") as source:
        magic = source.read(4)
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe",
                         b"\xfe\xed\xfa\xce", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                         b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            return None
        if magic != b"\xcf\xfa\xed\xfe":
            raise AuditError(f"tvOS binary must be thin little-endian ARM64 Mach-O: {path}")
        header = source.read(28)
        if len(header) != 28:
            raise AuditError(f"truncated Mach-O header: {path}")
        cpu, subtype, filetype, count, size, flags, reserved = struct.unpack("<7I", header)
        if cpu != 0x0100000C or subtype & 0x00FFFFFF != 0:
            raise AuditError(f"wrong tvOS Mach-O architecture (expected arm64): {path}")
        if filetype not in (2, 6, 8):
            raise AuditError(f"unsupported tvOS Mach-O file type: {path}")
        if size > path.stat().st_size - 32 or count > size // 8:
            raise AuditError(f"invalid Mach-O load command bounds: {path}")
        commands = source.read(size)
    dependencies: list[str] = []
    rpaths: list[str] = []
    install_name = None
    platforms: list[int] = []
    offset = 0
    for _ in range(count):
        if offset + 8 > len(commands):
            raise AuditError(f"truncated Mach-O load command: {path}")
        command, length = struct.unpack_from("<2I", commands, offset)
        if length < 8 or length % 8 or offset + length > len(commands):
            raise AuditError(f"invalid Mach-O load command size: {path}")
        data = commands[offset:offset + length]
        if command == 0x32:  # LC_BUILD_VERSION: tvOS device is 3, simulator is 8.
            if length < 24:
                raise AuditError(f"invalid LC_BUILD_VERSION: {path}")
            platforms.append(struct.unpack_from("<I", data, 8)[0])
        elif command in (0xC, 0xD, 0x18 | 0x80000000, 0x1F | 0x80000000, 0x20,
                         0x23 | 0x80000000, 0x1C | 0x80000000):
            minimum = 12 if command == 0x1C | 0x80000000 else 24
            if length < minimum:
                raise AuditError(f"invalid Mach-O path command: {path}")
            start = struct.unpack_from("<I", data, 8)[0]
            if not minimum <= start < length or b"\0" not in data[start:]:
                raise AuditError(f"invalid Mach-O path string: {path}")
            try:
                value = data[start:].split(b"\0", 1)[0].decode("utf-8")
            except UnicodeDecodeError as error:
                raise AuditError(f"invalid Mach-O path encoding: {path}") from error
            if command == 0x1C | 0x80000000:
                rpaths.append(value)
            elif command == 0xD:
                install_name = value
            else:
                dependencies.append(value)
        offset += length
    if offset != size or platforms != [3]:
        raise AuditError(f"Mach-O must declare exactly one device tvOS LC_BUILD_VERSION: {path}")
    kind = {2: "executable", 6: "library", 8: "bundle"}[filetype]
    return MachOInfo(path, frozenset({"arm64"}), kind, install_name, tuple(dependencies), tuple(rpaths))


def audit_tvos_ipa(args: argparse.Namespace) -> int:
    """Validate an unsigned/signable device IPA, not Apple signing or runtime."""
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.version):
        raise AuditError("tvOS package version must be MAJOR.MINOR.PATCH")
    expected = f"Spool-{args.version}-tvOS-arm64.ipa"
    if args.ipa.name != expected or args.ipa.is_symlink() or not args.ipa.is_file():
        raise AuditError(f"expected regular tvOS device package named {expected}")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        try:
            with zipfile.ZipFile(args.ipa) as archive:
                names: set[str] = set()
                links: dict[str, str] = {}
                kinds: dict[str, int] = {}
                entries = archive.infolist()
                if not entries:
                    raise AuditError("empty tvOS IPA")
                for entry in entries:
                    name = entry.filename.rstrip("/")
                    parts = PurePosixPath(name).parts
                    mode = entry.external_attr >> 16
                    if (not name or "\\" in name or "\0" in name or name.startswith("/")
                            or any(part in ("", ".", "..") for part in name.split("/"))
                            or parts[:2] not in (("Payload",), ("Payload", "Spool.app"))
                            or (parts == ("Payload",) and not entry.is_dir())
                            or (parts == ("Payload", "Spool.app") and not entry.is_dir())):
                        raise AuditError(f"unsafe or unexpected IPA member: {entry.filename}")
                    if "_CodeSignature" in parts or "embedded.mobileprovision" in parts:
                        raise AuditError(f"public tvOS IPA must be unsigned/unprovisioned: {name}")
                    if name in names:
                        raise AuditError(f"duplicate IPA member: {name}")
                    names.add(name)
                    kind = stat.S_IFMT(mode)
                    if kind not in (0, stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
                        raise AuditError(f"unsupported IPA member type: {name}")
                    if kind == stat.S_IFDIR and not entry.is_dir():
                        raise AuditError(f"invalid IPA directory: {name}")
                    kinds[name] = stat.S_IFDIR if entry.is_dir() else kind
                    if kind == stat.S_IFLNK:
                        if entry.is_dir() or entry.file_size > 4096:
                            raise AuditError(f"invalid IPA symlink: {name}")
                        try:
                            link = archive.read(entry).decode("utf-8")
                        except UnicodeDecodeError as error:
                            raise AuditError(f"invalid IPA symlink encoding: {name}") from error
                        if not link or link.startswith("/") or "\\" in link or "\0" in link:
                            raise AuditError(f"unsafe IPA symlink: {name}")
                        links[name] = link
                # Validate every path before extraction; never write through a link.
                for name in names:
                    for ancestor in PurePosixPath(name).parents:
                        if ancestor.as_posix() in kinds and kinds[ancestor.as_posix()] != stat.S_IFDIR:
                            raise AuditError(f"non-directory IPA ancestor: {ancestor}")
                for entry in entries:
                    name = entry.filename.rstrip("/")
                    if name in links:
                        continue
                    mode = entry.external_attr >> 16
                    target = root / name
                    if entry.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with archive.open(entry) as source, target.open("xb") as destination:
                            shutil.copyfileobj(source, destination)
                        target.chmod(mode & 0o777 or 0o644)
                for name, link in links.items():
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.symlink_to(link)
                app_root = root / "Payload/Spool.app"
                for name in links:
                    target = root / name
                    resolved = target.resolve()
                    if not resolved.is_relative_to(app_root) or not resolved.exists():
                        raise AuditError(f"escaping or dangling IPA symlink: {name}")
        except (OSError, ValueError, RuntimeError, zipfile.BadZipFile) as error:
            if isinstance(error, AuditError):
                raise
            raise AuditError(f"invalid tvOS IPA: {error}") from error
        app = root / "Payload/Spool.app"
        try:
            with (app / "Info.plist").open("rb") as source:
                metadata = plistlib.load(source)
        except (OSError, ValueError, plistlib.InvalidFileException) as error:
            raise AuditError(f"missing or invalid tvOS Info.plist: {error}") from error
        if not isinstance(metadata, dict):
            raise AuditError("tvOS Info.plist must be a dictionary")
        if metadata.get("CFBundleExecutable") != "Spool" or metadata.get("CFBundlePackageType") != "APPL":
            raise AuditError("tvOS Info.plist must describe the Spool application executable")
        if (metadata.get("CFBundleSupportedPlatforms") != ["AppleTVOS"]
                or metadata.get("UIDeviceFamily") != [3]):
            raise AuditError("tvOS Info.plist must target AppleTVOS device family 3")
        if any(metadata.get(key) != args.version for key in ("CFBundleShortVersionString", "CFBundleVersion")):
            raise AuditError(f"tvOS embedded bundle version must match {args.version}")
        closure = argparse.Namespace(app=app, tvos=True, architecture=["arm64"], root_binary=[])
        audit_macho(closure)
        if args.extract:
            args.extract.mkdir(parents=True, exist_ok=True)
            if any(args.extract.iterdir()):
                raise AuditError("IPA extraction destination must be empty")
            shutil.copytree(root / "Payload", args.extract / "Payload", symlinks=True)
    print(f"tvOS device IPA passed: {expected} (signing/provisioning and runtime not verified).")
    return 0


def audit_macho(args: argparse.Namespace) -> int:
    app = args.app.resolve()
    tvos = getattr(args, "tvos", False)
    executable = app / ("Spool" if tvos else "Contents/MacOS/Spool")
    if not executable.is_file():
        raise AuditError(f"main Mach-O executable is missing: {executable}")
    files: list[MachOInfo] = []
    for path in app.rglob("*"):
        if path.is_file() and not path.is_symlink():
            info = tvos_macho_info(path) if tvos else macho_info(path, args.file_tool, args.lipo, args.otool)
            if info:
                files.append(info)
    by_path = {info.path.resolve(): info for info in files}
    main = by_path.get(executable.resolve())
    if not main:
        raise AuditError(f"main executable is not Mach-O: {executable}")
    if main.kind != "executable":
        raise AuditError(f"main Mach-O is not an executable: {executable}")
    expected = frozenset(args.architecture) if args.architecture else main.architectures
    errors: list[str] = []
    executable_dir = executable.parent
    roots = {
        info.path.resolve()
        for info in files
        if info.path.resolve() == executable.resolve()
        or info.kind == "executable"
        or relative(info.path, app).startswith(
            ("PlugIns/", "qml/") if tvos else ("Contents/PlugIns/", "Contents/Resources/qml/")
        )
    }
    for root in args.root_binary:
        path = (app / root).resolve()
        if not path.is_relative_to(app) or path not in by_path:
            raise AuditError(f"Mach-O root is not a bundled binary: {root}")
        roots.add(path)
    reachable: set[Path] = set()
    pending = deque(sorted(roots))
    for info in files:
        if info.architectures != expected:
            errors.append(
                f"wrong Mach-O architecture: {relative(info.path, app)} -> "
                f"{','.join(sorted(info.architectures))}; expected {','.join(sorted(expected))}"
            )
        for value in (*info.rpaths, *((info.install_name,) if info.install_name else ())):
            if value and (value.startswith("/nix/store/") or re.search(r"(^|/)build(/|$)", value)):
                errors.append(f"forbidden Mach-O path: {relative(info.path, app)} -> {value}")

    while pending:
        path = pending.popleft()
        if path in reachable:
            continue
        reachable.add(path)
        info = by_path[path]
        for dependency in info.dependencies:
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            if dependency.startswith("/nix/store/") or re.search(r"(^|/)build(/|$)", dependency):
                errors.append(f"forbidden Mach-O dependency: {relative(info.path, app)} -> {dependency}")
                continue
            candidates: list[Path] = []
            if dependency.startswith("@rpath/"):
                suffix = dependency.removeprefix("@rpath/")
                for rpath in dict.fromkeys((*info.rpaths, *main.rpaths)):
                    base = expand_macho_name(rpath, info, executable_dir)
                    if base:
                        candidates.append((base / suffix).resolve())
            else:
                candidate = expand_macho_name(dependency, info, executable_dir)
                if candidate:
                    candidates.append(candidate)
            matches = {candidate for candidate in candidates if candidate in by_path}
            if len(matches) == 1:
                pending.append(next(iter(matches)))
            elif len(matches) > 1:
                errors.append(f"ambiguous Mach-O dependency: {relative(info.path, app)} -> {dependency}")
            else:
                errors.append(f"unresolved Mach-O dependency: {relative(info.path, app)} -> {dependency}")
    unreferenced = sorted(
        (info.path.resolve() for info in files if info.path.resolve() not in reachable),
        key=lambda item: relative(item, app),
    )
    for path in unreferenced:
        print(f"UNREFERENCED\\t{relative(path, app)}")
    if unreferenced:
        errors.append(f"unreferenced packaged Mach-O files: {len(unreferenced)}")
    if errors:
        raise AuditError("Mach-O audit failed:\n" + "\n".join(sorted(set(errors))))
    print(f"Mach-O closure passed for {len(files)} files.", file=sys.stderr)
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description="Audit deterministic release package payloads")
    commands = result.add_subparsers(dest="command", required=True)
    inventory_parser = commands.add_parser("inventory")
    inventory_parser.add_argument("root", type=Path)
    inventory_parser.add_argument("--output", type=Path)
    inventory_parser.set_defaults(handler=inventory)
    elf_parser = commands.add_parser("elf")
    elf_parser.add_argument("root", type=Path)
    elf_parser.add_argument("--root", dest="root_binary", action="append", required=True, metavar="RELATIVE")
    elf_parser.add_argument("--allow-system", action="append", default=[], metavar="SONAME")
    elf_parser.add_argument("--readelf")
    elf_parser.set_defaults(handler=audit_elf)
    macho_parser = commands.add_parser("macho")
    macho_parser.add_argument("app", type=Path)
    macho_parser.add_argument("--root", dest="root_binary", action="append", default=[], metavar="RELATIVE")
    macho_parser.add_argument("--architecture", action="append", default=[])
    macho_parser.add_argument("--file-tool", default="file")
    macho_parser.add_argument("--lipo", default="lipo")
    macho_parser.add_argument("--otool", default="otool")
    macho_parser.set_defaults(handler=audit_macho)
    tvos_parser = commands.add_parser("tvos-ipa", help="Audit an unsigned/signable device tvOS IPA")
    tvos_parser.add_argument("ipa", type=Path)
    tvos_parser.add_argument("--version", required=True)
    tvos_parser.add_argument("--extract", type=Path, help="Extract validated Payload into an empty directory")
    tvos_parser.set_defaults(handler=audit_tvos_ipa)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        return args.handler(args)
    except AuditError as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
