#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import os
import re
import stat
import subprocess
import sys
from collections import defaultdict, deque
from dataclasses import dataclass
from pathlib import Path
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
    for path, info in infos.items():
        providers[path.name].add(path)
        if info.soname:
            providers[info.soname].add(path)
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


def audit_macho(args: argparse.Namespace) -> int:
    app = args.app.resolve()
    executable = app / "Contents/MacOS/Spool"
    if not executable.is_file():
        raise AuditError(f"main Mach-O executable is missing: {executable}")
    files: list[MachOInfo] = []
    for path in app.rglob("*"):
        if path.is_file() and not path.is_symlink():
            info = macho_info(path, args.file_tool, args.lipo, args.otool)
            if info:
                files.append(info)
    by_path = {info.path.resolve(): info for info in files}
    main = by_path.get(executable.resolve())
    if not main:
        raise AuditError(f"main executable is not Mach-O: {executable}")
    expected = frozenset(args.architecture) if args.architecture else main.architectures
    errors: list[str] = []
    executable_dir = executable.parent
    roots = {
        info.path.resolve()
        for info in files
        if info.path.resolve() == executable.resolve()
        or info.kind == "executable"
        or relative(info.path, app).startswith(("Contents/PlugIns/", "Contents/Resources/qml/"))
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
