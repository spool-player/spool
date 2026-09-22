#!/usr/bin/env python3
"""sdk/spool-provider.py: builds, validates and describes .tar.zst provider packages."""
import hashlib
import importlib.util
import io
import json
import pathlib
import shutil
import tarfile
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("spool_provider", ROOT / "sdk/spool-provider.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

try:
    from compression import zstd as _  # noqa: F401  Python 3.14+
    HAVE_ZSTD = True
except ImportError:
    HAVE_ZSTD = shutil.which("zstd") is not None


def manifest(**changes):
    value = {"format": 2, "api": "0.2", "id": "test.provider", "name": "Test", "version": "0.1.0",
             "summary": "A test provider", "entry": "logic/provider.mjs", "icon": "assets/icon.svg",
             "capabilities": ["search"], "ui": {"login": "ui/Login.qml"}, "origins": ["https://api.example.org"]}
    value.update(changes)
    return value


def files(**changes):
    value = {"manifest.json": json.dumps(manifest()).encode(),
             "logic/provider.mjs": b"export function createSource() { return {describe() { return {}; }}; }",
             "ui/Login.qml": b"import QtQuick\nimport QtQuick.Layouts\nimport Spool\nItem {}\n",
             "assets/icon.svg": b"<svg/>", "LICENSE": b"license", "NOTICE": b"notice"}
    value.update(changes)
    return value


class ValidateTest(unittest.TestCase):
    def rejects(self, package, fragment):
        with self.assertRaises(ValueError) as caught:
            tool.validate(package)
        self.assertIn(fragment, str(caught.exception))

    def test_valid_package(self):
        self.assertEqual(tool.validate(files())["id"], "test.provider")

    def test_unsafe_and_colliding_paths(self):
        for name in ("../escape.mjs", "/escape.mjs", "logic/../escape.mjs", "logic\\escape.mjs", "C:/escape.mjs",
                     "logic/.hidden.mjs", "logic/provider.exe", "logic /spaced.mjs"):
            with self.subTest(name=name):
                self.rejects(files(**{name: b"x"}), "forbidden path")
        self.rejects(files(**{"LOGIC/PROVIDER.MJS": b"x"}), "case-colliding")

    def test_native_payloads(self):
        for magic in (b"\x7fELF", b"MZ", b"\0asm", b"\xcf\xfa\xed\xfe"):
            with self.subTest(magic=magic):
                self.rejects(files(**{"logic/provider.mjs": magic + b"rest"}), "native")

    def test_manifest_contract(self):
        for changes, fragment in (({"format": 1}, "format 2"), ({"api": "0.1"}, "format 2"),
                                  ({"id": "NoDots"}, "publisher.name"), ({"version": "1.0"}, "semantic"),
                                  ({"name": "x" * 65}, "64 characters"), ({"entry": "logic/main.js"}, ".mjs"),
                                  ({"icon": "assets/missing.svg"}, "icon"), ({"ui": {"wizard": "ui/Login.qml"}}, "roles"),
                                  ({"ui": {"settings": "ui/Settings.qml"}}, "missing QML"),
                                  ({"capabilities": ["teleport"]}, "unknown capabilities"),
                                  ({"origins": ["https://example.org/path"]}, "origins")):
            with self.subTest(changes=changes):
                self.rejects(files(**{"manifest.json": json.dumps(manifest(**changes)).encode()}), fragment)
        self.rejects({k: v for k, v in files().items() if k != "manifest.json"}, "manifest.json")

    def test_qml_imports_are_limited_to_what_spool_provides(self):
        for module in ("QtWebEngine", "Spool.Ui", "QtQuick.LocalStorage"):
            with self.subTest(module=module):
                self.rejects(files(**{"ui/Login.qml": f"import {module}\nItem {{}}\n".encode()}), "imports")


@unittest.skipUnless(HAVE_ZSTD, "needs Python 3.14 or the zstd command")
class PackageTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = pathlib.Path(directory.name)
        self.source = self.root / "source"
        for name, data in files(**{"README.md": b"not packaged", "tests/contract.mjs": b"not packaged"}).items():
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def test_build_is_reproducible_and_round_trips(self):
        first = tool.build(self.source, self.root / "a.tar.zst")
        second = tool.build(self.source, self.root / "b.tar.zst")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        manifest_value, contents = tool.read(first)
        self.assertEqual(manifest_value["version"], "0.1.0")
        self.assertEqual(contents, files(), "only manifest, licences, logic/, ui/ and assets/ are packaged")

    def test_default_output_name(self):
        self.assertEqual(tool.build(self.source, None).name, "test.provider-0.1.0.tar.zst")

    def test_symlinks_are_refused(self):
        (self.source / "logic/link.mjs").symlink_to(self.source / "logic/provider.mjs")
        with self.assertRaises(ValueError):
            tool.build(self.source, self.root / "out.tar.zst")

    def test_links_inside_archives_are_refused(self):
        tar = io.BytesIO()
        with tarfile.open(fileobj=tar, mode="w:", format=tarfile.USTAR_FORMAT) as archive:
            for name, data in files().items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
            link = tarfile.TarInfo("logic/escape.mjs")
            link.type, link.linkname = tarfile.SYMTYPE, "/etc/passwd"
            archive.addfile(link)
        path = self.root / "linked.tar.zst"
        path.write_bytes(tool.compress(tar.getvalue()))
        with self.assertRaises(ValueError):
            tool.read(path)

    def test_feed_entry_describes_the_exact_archive(self):
        package = tool.build(self.source, self.root / "p.tar.zst")
        entry = tool.feed(package, "https://example.org/p.tar.zst")
        self.assertEqual(entry["id"], "test.provider")
        self.assertEqual(entry["api"], "0.2")
        self.assertEqual(entry["url"], "https://example.org/p.tar.zst")
        self.assertEqual(entry["size"], package.stat().st_size)
        self.assertEqual(entry["sha256"], hashlib.sha256(package.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
