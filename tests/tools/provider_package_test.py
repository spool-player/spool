#!/usr/bin/env python3
import importlib.util
import json
import pathlib
import stat
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("provider_package", ROOT / "tools/provider-package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


def fixture():
    manifest = {"format": 1, "id": "test.provider", "version": "0.1.0", "api": "0.1",
                "entry": "logic/provider.mjs", "publisher": "test", "license": "MPL-2.0",
                "offers": {"future-feature": "0.1"}, "requires": {"http": "0.1"},
                "optional": {"unknown-optional": "0.1"}, "permissions": ["network:configured-origins"],
                "channels": ["desktop"], "stateSchema": 1, "ui": {"modules": [], "components": []}}
    return {"manifest.json": json.dumps(manifest).encode(), "logic/provider.mjs": b"export function createSource() {return {};}",
            "LICENSE": b"fixture license", "NOTICE": b"synthetic test data"}


class PackageTest(unittest.TestCase):
    def archive(self, files, metadata=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = pathlib.Path(directory.name) / "provider.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in files.items():
                entry = zipfile.ZipInfo(name)
                entry.external_attr = ((metadata or {}).get(name, stat.S_IFREG | 0o644)) << 16
                archive.writestr(entry, data)
        return path

    def test_optional_and_offered_extensions_do_not_reject(self):
        manifest, files = package.read_package(self.archive(fixture()))
        self.assertEqual(manifest["id"], "test.provider")
        self.assertIn("logic/provider.mjs", files)

    def test_required_extensions_permissions_and_api_fail_closed(self):
        for key, value in (("requires", {"unknown": "0.1"}), ("permissions", ["shell"]), ("api", "0.2"), ("format", 2)):
            with self.subTest(key=key):
                files = fixture()
                manifest = json.loads(files["manifest.json"])
                manifest[key] = value
                files["manifest.json"] = json.dumps(manifest).encode()
                with self.assertRaises(ValueError):
                    package.read_package(self.archive(files))

    def test_unsafe_and_case_colliding_paths(self):
        for name in ("../escape.qml", "/escape.qml", "logic/../escape.qml", "logic\\escape.qml", "C:/escape.qml", "logic/CON.qml", "logic/provider.mjs.", "LOGIC/PROVIDER.MJS"):
            with self.subTest(name=name):
                files = fixture()
                files[name] = b"bad"
                with self.assertRaises(ValueError):
                    package.read_package(self.archive(files))

    def test_symlink_and_executable_modes(self):
        for mode in (stat.S_IFLNK | 0o777, stat.S_IFREG | 0o755, stat.S_IFIFO | 0o644):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                package.read_package(self.archive(fixture(), {"logic/provider.mjs": mode}))

    def test_native_payload_disguised_as_source(self):
        files = fixture()
        files["logic/provider.mjs"] = b"\x7fELFfake"
        with self.assertRaises(ValueError):
            package.read_package(self.archive(files))

    def test_import_escape_and_undeclared_qml(self):
        files = fixture()
        files["logic/provider.mjs"] = b'import "../../private.mjs";'
        with self.assertRaises(ValueError):
            package.read_package(self.archive(files))
        files = fixture()
        files["ui/Hidden.qml"] = b"import QtQuick\nItem {}"
        with self.assertRaises(ValueError):
            package.read_package(self.archive(files))

    def test_expanded_size_limit_before_read(self):
        files = fixture()
        files["resources/bomb.txt"] = b"0" * (package.MAX_FILE + 1)
        with self.assertRaises(ValueError):
            package.read_package(self.archive(files))

    def test_build_is_reproducible_and_round_trips(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            source = root / "source"
            for name, data in fixture().items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            a, b = root / "a.zip", root / "b.zip"
            package.build(source, a)
            package.build(source, b)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(package.read_package(a)[1], fixture())


if __name__ == "__main__":
    unittest.main()
