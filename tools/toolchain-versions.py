#!/usr/bin/env python3
"""Validate version pins and fingerprint the inputs to CI's build caches."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = "tools/manifests"
NIX_INSTALL_ACTION = ".github/actions/install-nix/action.yml"
NIX_INSTALLER_REPO = "nixbuild/nix-quick-install-action"
NIX_INSTALLER_PIN = re.compile(rf"uses: {NIX_INSTALLER_REPO}@([0-9a-f]{{40}})\s+# (v\d+)")


def load(path):
    return json.loads(path.read_text(encoding="utf-8"))


def fingerprint(root, *data, files=()):
    paths = sorted({path for pattern in files for path in root.glob(pattern) if path.is_file()})
    inputs = [(path.relative_to(root).as_posix(), hashlib.sha256(path.read_bytes()).hexdigest())
              for path in paths]
    return hashlib.sha256(json.dumps((data, inputs), sort_keys=True).encode()).hexdigest()[:24]


def outputs(root, mpv_revision):
    manifests = root / MANIFESTS
    pins = load(manifests / "toolchain.json")
    qt, apple, android, qcoro = (pins[name] for name in ("qt", "apple", "android", "qcoro"))
    android_qt = load(manifests / "qt-android.json")
    webos_qt = load(manifests / "qt-webos.json")
    android_sources = load(manifests / "android-third-party.json")["sources"]
    streaming_sources = load(manifests / "streaming-third-party.json")["sources"]

    def qt_sources(modules, mirror):
        return (qt["version"], qt["series"], qt[mirror], modules,
                {module: qt["sources"][module] for module in modules})

    streaming = fingerprint(root, streaming_sources, files=(
        "tools/prepare-streaming-sources.sh", "tools/streaming-fingerprint.py",
        "tools/check-streaming-dependencies.py", "tools/cmake/StreamingDependencies.cmake",
        "tools/cmake/QuicBatchingProbe/CMakeLists.txt", "tools/lib/streaming-build.sh",
        "tools/patches/curl-*.patch",
    ))
    ffmpeg = fingerprint(root, pins["ffmpeg"], files=(
        f"{MANIFESTS}/ffmpeg-capabilities.json", "tools/ffmpeg-capabilities.py",
    ))
    native_shell = fingerprint(root, qt, qcoro, ffmpeg, streaming, pins["nix"], files=(
        "flake.nix", "flake.lock", NIX_INSTALL_ACTION, "tools/patches/**/*",
        "tools/webos-native/patches/qtdeclarative-*-qmlimportscanner-exclude-subtrees.patch",
    ))
    mpv = fingerprint(root, mpv_revision, files=(f"{MANIFESTS}/mpv-native.json",))
    apple_tools = fingerprint(root, files=(f"{MANIFESTS}/macos-tools.txt", "tools/apple/install-build-tools.sh"))
    apple_host = fingerprint(root, qt["version"], apple["aqtinstall"], apple["qtModules"])
    ios_kit = fingerprint(root, apple_host, files=("tools/apple/aqt-ios.py",))
    tvos_qt = fingerprint(root, apple_host, apple_tools, apple["tvosDeploymentTarget"],
                          qt_sources(apple["tvosQtSourceModules"], "archiveBaseUrl"), files=(
        "tools/apple/build-qt-tvos.sh", "tools/apple/patches/qt-tvos-*.patch",
        "tools/apple/qmlimportscanner/CMakeLists.txt", "tools/lib/manifest-sources.sh",
        "tools/webos-native/patches/qtdeclarative-*-qmlimportscanner-exclude-subtrees.patch",
    ))
    apple_sources = {name: android_sources[name] for name in
                     ("freetype", "qcoro", "fribidi", "harfbuzz", "libass", "libplacebo")}
    apple_deps = fingerprint(root, apple_sources, apple_tools, streaming, ffmpeg, files=(
        "tools/apple/build-dependencies.sh", "tools/lib/manifest-sources.sh",
    ))
    # Qt probes the target's TLS and FreeType libraries, independently of the
    # FFmpeg, mpv and QCoro builds in the same prefix.
    android_qt_key = fingerprint(root, android, streaming_sources["openssl"], android_sources["freetype"],
                                 qt_sources(android_qt["modules"], "fauArchiveBaseUrl"), files=(
        "tools/android/build-qt6.sh", "tools/android/build-dependencies.sh",
        "tools/android/patches/qt-android-*.patch",
        "tools/lib/manifest-sources.sh",
    ))
    android_media_sources = {name: source for name, source in android_sources.items() if name != "qcoro"}
    android_deps = fingerprint(root, android, android_media_sources, ffmpeg, streaming, files=(
        "tools/android/build-dependencies.sh",
        "tools/android/gradle-mirror.sh", "tools/android/patches/*.patch",
        "tools/lib/manifest-sources.sh", "tools/lib/build-common.sh",
    ))
    webos_deps = fingerprint(root, webos_qt, qt_sources(webos_qt["modules"], "fauArchiveBaseUrl"),
                             streaming, ffmpeg, files=(
        f"{MANIFESTS}/webos-third-party.json", "tools/lib/manifest-sources.sh",
        "tools/lib/build-common.sh", "tools/webos-native/build-webos.sh",
        "tools/webos-native/build-qt6.sh", "tools/webos-native/build-qcoro.sh",
        "tools/webos-native/build-third-party.sh", "tools/webos-native/build-ffmpeg.sh",
        "tools/webos-native/build-curl.sh", "tools/webos-native/restore-toolchain.sh",
        "tools/webos-native/nixos-sdk-compat.sh", "tools/webos-native/*.cmake",
        "tools/webos-native/patches/*.patch",
    ))
    return {
        "qt_version": qt["version"],
        "qt_series": qt["series"],
        "qt_build_tag": qt["series"].replace(".", ""),
        "qt_windows_kit": qt["windowsKit"],
        "ffmpeg_version": pins["ffmpeg"]["version"],
        "qcoro_revision": qcoro["revision"],
        "qcoro_windows_prefix": qcoro["windowsPrefix"],
        "ios_deployment_target": apple["iosDeploymentTarget"],
        "tvos_deployment_target": apple["tvosDeploymentTarget"],
        "apple_qt_modules": " ".join(apple["qtModules"]),
        "aqt_version": apple["aqtinstall"],
        "android_build_tools": android["buildTools"],
        "key_qt": fingerprint(root, qt),
        "key_mpv": mpv,
        "key_streaming": streaming,
        "key_ffmpeg": ffmpeg,
        "key_native_shell": native_shell,
        "key_linux_build": fingerprint(root, native_shell, mpv, files=(
            "tools/build-linux-release.sh", "tools/lib/build-common.sh",
        )),
        "key_macos_build": fingerprint(root, native_shell, mpv, files=(
            "tools/build-macos.sh", "tools/lib/build-common.sh",
        )),
        "key_apple_host": apple_host,
        "key_ios_kit": ios_kit,
        "key_tvos_qt": tvos_qt,
        "key_apple_deps": apple_deps,
        "key_ios_app": fingerprint(root, files=("tools/build-ios.sh", "tools/apple/build-ios-scanner.sh")),
        "key_tvos_app": fingerprint(root, files=("tools/build-tvos.sh",)),
        "key_android_qt": android_qt_key,
        "key_android_deps": android_deps,
        "key_webos_deps": webos_deps,
        "key_webos_build": fingerprint(root, mpv, files=("build-ipk.sh", "tools/lib/build-common.sh")),
        "key_windows_qt": fingerprint(root, qt["version"], qt["windowsKit"], qcoro, files=(
            f"{MANIFESTS}/qt-windows.json", "tools/windows/install-qt.ps1", "tools/windows/install-qcoro.ps1",
        )),
    }


def version_errors(root):
    manifests = root / MANIFESTS
    pins = load(manifests / "toolchain.json")
    qt, apple, android, nix = (pins[name] for name in ("qt", "apple", "android", "nix"))
    errors = []

    def require(condition, message):
        if not condition:
            errors.append(message)

    def check_modules(modules, label):
        valid = isinstance(modules, list) and bool(modules) and all(
            isinstance(module, str) and module in qt["sources"] for module in modules
        )
        require(valid, f"{label} must list modules by name from toolchain.json")
        if valid:
            require(len(set(modules)) == len(modules), f"{label} contains duplicate modules")

    semver = r"\d+\.\d+\.\d+"
    require(re.fullmatch(semver, qt["version"]) and qt["version"].rsplit(".", 1)[0] == qt["series"],
            "toolchain.json Qt version and series disagree")
    require(all(re.fullmatch(r"[a-f0-9]{64}", digest) for digest in qt["sources"].values()),
            "toolchain.json Qt sources must have SHA-256 digests")
    for platform in ("android", "webos"):
        name = f"qt-{platform}.json"
        data = load(manifests / name)
        require(not (data.keys() & {"qtVersion", "version", "series", "baseUrl", "android"}),
                f"{name} repeats toolchain.json pins")
        check_modules(data["modules"], name)
    check_modules(apple["qtModules"], "apple.qtModules")
    check_modules(apple["tvosQtSourceModules"], "apple.tvosQtSourceModules")
    for target in ("iosDeploymentTarget", "tvosDeploymentTarget"):
        require(re.fullmatch(r"\d+\.\d+", apple[target]), f"invalid apple.{target}")
    for name, value in (("apple.aqtinstall", apple["aqtinstall"]), ("android.buildTools", android["buildTools"]),
                        ("android.ndkVersion", android["ndkVersion"]), ("nix.version", nix["version"])):
        require(re.fullmatch(semver, value), f"invalid {name}")
    require(android["minimumSdk"].isdigit() and android["compileSdk"].isdigit()
            and int(android["minimumSdk"]) <= int(android["compileSdk"]), "invalid Android SDK bounds")
    macos_tools = (manifests / "macos-tools.txt").read_text(encoding="utf-8")
    for tool in ("cmake", "ninja", "meson"):
        require(re.search(rf"(?m)^{tool}=={semver}$", macos_tools),
                f"macos-tools.txt must pin the {tool} version")

    windows = load(manifests / "qt-windows.json")
    require(not (windows.keys() & {"version", "architecture"}), "qt-windows.json repeats toolchain.json pins")
    qt_digits = qt["version"].replace(".", "")
    require(windows["packageVersion"].startswith(qt["version"] + "-") and
            windows["repository"].endswith(f"/qt6_{qt_digits}/qt6_{qt_digits}_{qt['windowsKit']}") and
            all(archive["package"].startswith(f"qt.qt6.{qt_digits}.") and
                archive["package"].endswith(f".win64_{qt['windowsKit']}") for archive in windows["archives"]),
            "qt-windows.json official archive metadata disagrees with the Qt version or kit")
    require(qt["fauArchiveBaseUrl"] == "https://ftp.fau.de/qtproject/archive/qt",
            "cross-built Qt must use the FAU archive mirror")
    for platform in ("android", "webos"):
        name = f"{platform}-third-party.json"
        require("ffmpeg" not in load(manifests / name)["sources"],
                f"{name} still pins FFmpeg; toolchain.json owns it")

    installer = (root / NIX_INSTALL_ACTION).read_text(encoding="utf-8")
    action = NIX_INSTALLER_PIN.search(installer)
    require(action and action[2] == nix["quickInstallRelease"],
            "nix-quick-install action version comment must match toolchain.json")
    for field in ("version", "quickInstallRelease"):
        require(f"tools/toolchain-versions.sh nix.{field}" in installer,
                f"install-nix must read nix.{field} from toolchain.json")
    require(not re.search(r"(?m)^\s+RELEASE:\s*[\"']?v\d+", installer),
            "install-nix repeats the archive release pin")
    renovate = load(root / "renovate.json")
    require(any(NIX_INSTALLER_REPO in rule.get("matchPackageNames", []) and rule.get("enabled") is False
                for rule in renovate["packageRules"]),
            "Renovate must leave the Nix installer pins to atomic manual updates")

    literal_pin = re.compile(
        r"\b(?:APPLE_DEPLOYMENT_TARGET|QT_VERSION|FFMPEG_VERSION|NIX_VERSION)\s*[:=]\s*[\"']?\d+\.\d+"
        r"|aqtinstall==\d|build-tools/\d+\.\d+|brew list --versions"
        r"|\b(?:ios|tvos)DeploymentTarget\s*[:=]\s*[\"']?\d+\.\d+"
    )
    cache_key = re.compile(
        r"\s+(?:primary-)?key:\s*(?:android6-|android-qt-|apple-(?:host-qt|ios-kit|media)-"
        r"|ios-app-|tvos-(?:source1|qt|app)-|webos-(?:deps|build)-|windows-(?:qt|mpv)-"
        r"|linux-build-|macos-build-|nix-native-shell-)"
    )
    for path in sorted((root / ".github").rglob("*.y*ml")):
        text = path.read_text(encoding="utf-8")
        for number, line in enumerate(text.splitlines(), 1):
            require(not literal_pin.search(line), f"{path.relative_to(root)}:{number} repeats a toolchain pin")
            if cache_key.match(line):
                require("hashFiles(" not in line and ".outputs." in line,
                        f"{path.relative_to(root)}:{number} must use a toolchain fingerprint")
                require(not re.search(r"\b\d+\.\d+(?:\.\d+)?\b", line),
                        f"{path.relative_to(root)}:{number} repeats a version in a cache key")
        for step in re.split(r"(?m)^ +-(?: (?:name|uses|id):)", text):
            if re.search(r"(?m)^\s+key:\s*(?:android6-|android-qt-|tvos-source1-|tvos-qt-|windows-mpv-)", step):
                require("restore-keys:" not in step, f"{path.relative_to(root)} restores a partial toolchain cache")
    return errors


def nix_archive_errors(nix, asset_names):
    # Keep these in sync with the Linux, Apple Silicon and Intel macOS runners.
    platforms = ("x86_64-linux", "aarch64-darwin", "x86_64-darwin")
    required = {f"nix-{nix['version']}-{platform}.tar.zstd" for platform in platforms}
    missing = sorted(required.difference(asset_names))
    if missing:
        return [f"Nix installer {nix['quickInstallRelease']} is missing archives: {', '.join(missing)}"]
    return []


def check_nix_archives(root, nix):
    revision, release = NIX_INSTALLER_PIN.search((root / NIX_INSTALL_ACTION).read_text(encoding="utf-8")).groups()
    api = f"repos/{NIX_INSTALLER_REPO}"
    try:
        published_revision = subprocess.check_output(["gh", "api", f"{api}/commits/{release}", "--jq", ".sha"],
                                                     text=True).strip()
        asset_names = subprocess.check_output(["gh", "api", f"{api}/releases/tags/{release}",
                                               "--jq", ".assets[].name"], text=True).splitlines()
    except (OSError, subprocess.CalledProcessError) as error:
        return [f"Could not check Nix installer {release}: {error}"]
    if published_revision != revision:
        return [f"Nix installer {release} points to {published_revision}, expected {revision}"]
    return nix_archive_errors(nix, asset_names)


def main():
    if sys.argv[1:] in (["--check"], ["--check-nix-archives"]):
        pins = load(ROOT / MANIFESTS / "toolchain.json")
        errors = version_errors(ROOT)
        if not errors and sys.argv[1:] == ["--check-nix-archives"]:
            errors = check_nix_archives(ROOT, pins["nix"])
        if errors:
            print("\n".join(errors), file=sys.stderr)
            return 1
        if sys.argv[1:] == ["--check-nix-archives"]:
            print(f"Nix installer {pins['nix']['quickInstallRelease']} publishes all required Nix {pins['nix']['version']} archives")
        else:
            print(f"toolchain: Qt {pins['qt']['version']}, FFmpeg {pins['ffmpeg']['version']}")
        return 0
    mpv_revision = subprocess.check_output(["git", "rev-parse", "HEAD:mpv"], cwd=ROOT, text=True).strip()
    for key, value in outputs(ROOT, mpv_revision).items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
