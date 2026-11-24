import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = "tools/manifests/toolchain.json"
SOURCES = [
    ("tools/apple/build-ios-scanner.sh", "qtdeclarative"),
    ("tools/apple/build-qt-tvos.sh", "qtbase"),
]


def executable(path: Path, script: str) -> None:
    path.write_text("#!/usr/bin/env bash\n" + script + "\n", encoding="utf-8")
    path.chmod(0o755)


with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory)
    commands = fixture / "bin"
    commands.mkdir()
    for filename in (MANIFEST, "tools/lib/manifest-sources.sh", *(script for script, _ in SOURCES)):
        destination = fixture / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / filename, destination)

    manifest = json.loads((fixture / MANIFEST).read_text(encoding="utf-8"))
    # A different pin makes hard-coded source paths fail even when today's pin
    # has the same value. The stub stops before any network or build activity.
    manifest["qt"]["version"] = "6.17.3"
    (fixture / MANIFEST).write_text(json.dumps(manifest), encoding="utf-8")
    executable(commands / "uname", "echo Darwin")
    executable(commands / "qtpaths", "echo 6.17.3")
    for name in ("xcrun", "cmake", "ninja", "gpatch"):
        executable(commands / name, "exit 0")
    executable(commands / "curl", 'printf "%s\\n" "${@: -1}"; exit 42')

    env = dict(os.environ, PATH=f"{commands}{os.pathsep}{os.environ['PATH']}",
               QT_HOST_PATH=str(fixture), JOBS="1")
    for name, module in SOURCES:
        result = subprocess.run(["bash", str(fixture / name)], env=env, check=False,
                                text=True, capture_output=True)
        base = manifest["qt"]["sourceArchive"]
        expected = f"{base}/6.17/6.17.3/submodules/{module}-everywhere-src-6.17.3.tar.xz"
        assert result.returncode == 42, (name, result.returncode, result.stderr)
        assert result.stdout.strip() == expected, (name, result.stdout, result.stderr)

print("Apple build scripts resolved the Qt source archive from the toolchain pin.")
