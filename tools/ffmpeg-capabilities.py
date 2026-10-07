#!/usr/bin/env python3
"""Generate and audit the project's manifest-controlled FFmpeg feature set."""

from __future__ import annotations

import argparse
import fnmatch
import json
import pathlib
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "tools" / "manifests" / "ffmpeg-capabilities.json"
# Every platform the app builds FFmpeg for. A platform missing from here is
# rejected by the CLI rather than quietly falling back to FFmpeg's defaults.
SUPPORTED_PLATFORMS = ("android", "ios", "linux", "macos", "tvos", "webos", "windows")

CATEGORIES = {
    "protocols": "protocol",
    "demuxers": "demuxer",
    "parsers": "parser",
    "decoders": "decoder",
    "encoders": "encoder",
    "filters": "filter",
    "muxers": "muxer",
    "bitstreamFilters": "bsf",
}
RUNTIME_DEMUXER_NAMES = {
    "mpegps": "mpeg",
    "pcm_s16le": "s16le",
    "pcm_s24le": "s24le",
    "pcm_s32le": "s32le",
}
RUNTIME_DECODER_NAMES = {
    "movtext": "mov_text",
}
# libcurl, not FFmpeg, performs every network transfer: mpv points lavf's
# io_open callback at stream_curl. lavf's HLS demuxer still resolves each
# playlist and segment URL with avio_find_protocol_name() before it calls that
# callback, so a build without these protocols registered rejects every
# transcoded stream with "https or dtls protocol not found" long before curl
# is consulted. tls and tcp come with https whether they are listed or not;
# naming them keeps the audit's allow-list closed.
TLS_PROTOCOLS = ("http", "https", "tcp", "tls")


def load_manifest(path: pathlib.Path) -> dict:
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schemaVersion") != 1:
        raise ValueError("unsupported FFmpeg capability manifest schema")
    required = {
        "--disable-everything",
        "--disable-gpl",
        "--disable-version3",
        "--disable-nonfree",
        "--disable-autodetect",
    }
    if set(data.get("requiredDisableFlags", [])) != required:
        raise ValueError("requiredDisableFlags must contain the complete license/autodetect baseline")
    for key in ("libraries", *CATEGORIES):
        values = data.get(key)
        if not isinstance(values, list) or any(not isinstance(value, str) or not value for value in values):
            raise ValueError(f"{key} must be a list of non-empty strings")
        if values != sorted(set(values)):
            raise ValueError(f"{key} must be sorted and contain no duplicates")
    image_decoders = data.get("forbiddenImageDecoders")
    if not isinstance(image_decoders, list) or image_decoders != sorted(set(image_decoders)):
        raise ValueError("forbiddenImageDecoders must be sorted and contain no duplicates")
    all_decoders = set(data["decoders"])
    for platform_data in data.get("platforms", {}).values():
        all_decoders |= set(platform_data.get("decoders", []))
    enabled_images = set(image_decoders) & all_decoders
    if enabled_images:
        raise ValueError(f"FFmpeg image decoders must remain disabled: {sorted(enabled_images)}")
    for platform in SUPPORTED_PLATFORMS:
        if platform not in data.get("platforms", {}):
            raise ValueError(f"missing platform capability set: {platform}")
        if not isinstance(data["platforms"][platform].get("gpl"), bool):
            raise ValueError(f"{platform} gpl must be a boolean")
        hardware_accelerators = data["platforms"][platform].get("hardwareAccelerators")
        if (
            not isinstance(hardware_accelerators, list)
            or hardware_accelerators != sorted(set(hardware_accelerators))
        ):
            raise ValueError(f"{platform} hardwareAccelerators must be sorted and contain no duplicates")
    missing_tls = sorted(set(TLS_PROTOCOLS) - set(data["protocols"]))
    if missing_tls:
        raise ValueError(
            "protocols must keep the network set lavf's HLS demuxer resolves by name: "
            + ", ".join(missing_tls)
        )
    forbidden = set(data.get("forbiddenConfigureFlags", []))
    emitted = set(configure_flags(data, "webos"))
    overlap = forbidden & emitted
    if overlap:
        raise ValueError(f"forbidden configure flags are enabled: {sorted(overlap)}")
    return data


def platform_protocols(data: dict, platform: str) -> list[str]:
    return sorted(set(data["protocols"]) | set(data["platforms"][platform].get("protocols", [])))


# A platform's own decoders sit alongside the shared set rather than replacing
# it: Android's MediaCodec wrappers exist nowhere else, and the software
# decoders stay as the fallback when the hardware refuses a stream.
def platform_decoders(data: dict, platform: str) -> list[str]:
    return sorted(set(data["decoders"]) | set(data["platforms"][platform].get("decoders", [])))


def platform_values(data: dict, platform: str, key: str) -> list[str]:
    if key == "protocols":
        return platform_protocols(data, platform)
    if key == "decoders":
        return platform_decoders(data, platform)
    return sorted(set(data[key]) | set(data["platforms"][platform].get(key, [])))


def configure_flags(data: dict, platform: str) -> list[str]:
    if platform not in data["platforms"]:
        raise ValueError(f"unknown FFmpeg platform: {platform}")
    flags = list(data["requiredDisableFlags"])
    flags.extend(data["commonConfigureFlags"])
    flags.extend(data["platforms"][platform].get("configureFlags", []))
    if data["platforms"][platform]["gpl"]:
        flags.append("--enable-gpl")
    flags.extend(f"--enable-{library}" for library in data["libraries"])
    for key, configure_name in CATEGORIES.items():
        values = platform_values(data, platform, key)
        flags.extend(f"--enable-{configure_name}={value}" for value in values)
    flags.extend(
        f"--enable-hwaccel={value}" for value in data["platforms"][platform]["hardwareAccelerators"]
    )
    return flags


def split_enabled_values(configuration: str) -> Iterable[tuple[str, str]]:
    pattern = re.compile(
        r"--enable-(protocol|demuxer|parser|decoder|encoder|filter|muxer|bsf|hwaccel)=([^\s'\"]+)"
    )
    for match in pattern.finditer(configuration):
        for value in match.group(2).split(","):
            yield match.group(1), value

def split_enabled_components(configuration: str) -> Iterable[tuple[str, str]]:
    suffixes = {
        "PROTOCOL": "protocol",
        "DEMUXER": "demuxer",
        "PARSER": "parser",
        "DECODER": "decoder",
        "ENCODER": "encoder",
        "FILTER": "filter",
        "MUXER": "muxer",
        "BSF": "bsf",
        "HWACCEL": "hwaccel",
    }
    pattern = re.compile(
        r"^#define CONFIG_(.+)_(PROTOCOL|DEMUXER|PARSER|DECODER|ENCODER|FILTER|MUXER|BSF|HWACCEL) 1$",
        re.MULTILINE,
    )
    for match in pattern.finditer(configuration):
        yield suffixes[match.group(2)], match.group(1).lower()


def cpp_header(data: dict, platform: str) -> str:
    arrays = {
        "Protocols": platform_protocols(data, platform),
        "Demuxers": [RUNTIME_DEMUXER_NAMES.get(value, value) for value in data["demuxers"]],
        "Decoders": [RUNTIME_DECODER_NAMES.get(value, value) for value in platform_decoders(data, platform)],
        "Encoders": data["encoders"],
        "HardwareAccelerators": data["platforms"][platform]["hardwareAccelerators"],
        "Filters": data["filters"],
        "Muxers": data["muxers"],
        "BitstreamFilters": platform_values(data, platform, "bitstreamFilters"),
        "ForbiddenImageDecoders": data["forbiddenImageDecoders"],
    }
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <string_view>",
        "",
        "namespace FfmpegCapabilities {",
        f'inline constexpr std::string_view kPlatform = "{platform}";',
        f"inline constexpr bool kGplEnabled = {'true' if data['platforms'][platform]['gpl'] else 'false'};",
    ]
    for name, values in arrays.items():
        lines.append(f"inline constexpr std::array<std::string_view, {len(values)}> k{name} {{")
        lines.extend(f'    "{value}",' for value in values)
        lines.append("};")
    lines.extend(("} // namespace FfmpegCapabilities", ""))
    return "\n".join(lines)


def audit_components(data: dict, platform: str, enabled: list[tuple[str, str]], protocols_only: bool = False) -> None:
    allowed: dict[str, set[str]] = {}
    for key, configure_name in CATEGORIES.items():
        allowed[configure_name] = set(platform_values(data, platform, key))
    allowed["hwaccel"] = set(data["platforms"][platform]["hardwareAccelerators"])
    if not protocols_only:
        unexpected = sorted(
            f"{category}={value}" for category, value in enabled if value not in allowed[category]
        )
        if unexpected:
            raise ValueError(f"effective FFmpeg configuration enables unlisted features: {', '.join(unexpected)}")
    # An unlisted feature is a licensing or size problem; a missing protocol is
    # a playback outage, because lavf resolves protocols by name before mpv's
    # libcurl backend ever sees the URL. Audit both directions.
    absent = sorted(allowed["protocol"] - {value for category, value in enabled if category == "protocol"})
    if absent:
        raise ValueError(f"effective FFmpeg configuration is missing protocols: {', '.join(absent)}")
    if not protocols_only:
        for category in ("bsf", "hwaccel"):
            absent = sorted(allowed[category] - {v for k, v in enabled if k == category})
            if absent:
                raise ValueError(f"effective FFmpeg configuration is missing {category}: {', '.join(absent)}")


def audit_configuration(data: dict, platform: str, configuration: str) -> None:
    missing = [flag for flag in data["requiredDisableFlags"] if flag not in configuration]
    if missing:
        raise ValueError(f"effective FFmpeg configuration is missing: {', '.join(missing)}")
    present_forbidden = [flag for flag in data["forbiddenConfigureFlags"] if flag in configuration]
    if present_forbidden:
        raise ValueError(f"effective FFmpeg configuration enables forbidden licensing: {', '.join(present_forbidden)}")
    gpl_enabled = "--enable-gpl" in configuration
    if gpl_enabled != data["platforms"][platform]["gpl"]:
        raise ValueError(f"effective FFmpeg GPL policy does not match platform {platform}")
    enabled = list(split_enabled_values(configuration))
    enabled.extend(split_enabled_components(configuration))
    audit_components(data, platform, enabled)


def candidate_files(paths: list[pathlib.Path]) -> Iterable[pathlib.Path]:
    for path in paths:
        if path.is_file():
            yield path
        elif path.is_dir():
            yield from (candidate for candidate in path.rglob("*") if candidate.is_file())


def audit_closure(data: dict, paths: list[pathlib.Path]) -> None:
    patterns = data["forbiddenLibraryPatterns"]
    readelf = shutil.which("readelf")
    violations: set[str] = set()
    for path in candidate_files(paths):
        if any(fnmatch.fnmatch(path.name, pattern) for pattern in patterns):
            violations.add(str(path))
        if not readelf:
            continue
        result = subprocess.run([readelf, "-d", path], text=True, capture_output=True, check=False)
        if result.returncode != 0:
            continue
        for dependency in re.findall(r"Shared library: \[([^]]+)]", result.stdout):
            if any(fnmatch.fnmatch(dependency, pattern) for pattern in patterns):
                violations.add(f"{path}: NEEDED {dependency}")
    if violations:
        raise ValueError("forbidden FFmpeg dependency closure entries:\n" + "\n".join(sorted(violations)))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    subparsers = parser.add_subparsers(dest="command", required=True)

    configure = subparsers.add_parser("configure")
    configure.add_argument("--platform", required=True, choices=SUPPORTED_PLATFORMS)

    audit = subparsers.add_parser("audit-config")
    audit.add_argument("--platform", required=True, choices=SUPPORTED_PLATFORMS)
    audit.add_argument("configuration", nargs="+", type=pathlib.Path)

    components = subparsers.add_parser("audit-components")
    components.add_argument("--platform", required=True, choices=SUPPORTED_PLATFORMS)
    components.add_argument("--protocols-only", action="store_true")
    components.add_argument("configuration", nargs="+", type=pathlib.Path)

    closure = subparsers.add_parser("audit-closure")
    closure.add_argument("paths", nargs="+", type=pathlib.Path)

    header = subparsers.add_parser("cpp-header")
    header.add_argument("--platform", required=True, choices=SUPPORTED_PLATFORMS)
    header.add_argument("--output", required=True, type=pathlib.Path)

    subparsers.add_parser("validate")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        data = load_manifest(args.manifest)
        if args.command == "configure":
            print("\n".join(configure_flags(data, args.platform)))
        elif args.command in ("audit-config", "audit-components"):
            configuration = "\n".join(
                path.read_text(encoding="utf-8", errors="replace") for path in args.configuration
            )
            if args.command == "audit-config":
                audit_configuration(data, args.platform, configuration)
            else:
                audit_components(
                    data, args.platform, list(split_enabled_components(configuration)), args.protocols_only
                )
        elif args.command == "cpp-header":
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(cpp_header(data, args.platform), encoding="utf-8")
        elif args.command == "audit-closure":
            audit_closure(data, args.paths)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
