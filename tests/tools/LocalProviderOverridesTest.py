#!/usr/bin/env python3
"""Local provider discovery boundaries and manifest validation."""
import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("local_provider_overrides", ROOT / "tools/local-provider-overrides.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)


class DiscoveryTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.workspace = pathlib.Path(directory.name)
        self.root = self.workspace / "arbitrary app checkout"
        self.root.mkdir()

    def provider(self, folder, identifier="test.provider", **changes):
        checkout = self.workspace / folder
        (checkout / "logic").mkdir(parents=True)
        (checkout / "logic/main.mjs").write_text("export function createSource() {}\n")
        manifest = {"format": 2, "api": tool.SDK.API, "id": identifier, "name": "Test", "version": "0.1.0",
                    "entry": "logic/main.mjs"}
        manifest.update(changes)
        (checkout / "manifest.json").write_text(json.dumps(manifest))
        return checkout

    def test_arbitrary_names_are_sorted_by_manifest_id_and_nested_checkouts_ignored(self):
        first = self.provider("zeta checkout", "test.alpha")
        second = self.provider("alpha checkout", "test.zeta")
        self.provider("unrelated/nested", "test.nested")
        self.assertEqual(tool.discover(self.root), f"test.alpha={first};test.zeta={second}")

    def test_duplicate_ids_fail_with_both_checkouts(self):
        first = self.provider("one")
        second = self.provider("two")
        with self.assertRaisesRegex(ValueError, "duplicate provider ID test.provider") as caught:
            tool.discover(self.root)
        self.assertIn(str(first), str(caught.exception))
        self.assertIn(str(second), str(caught.exception))

    def test_directory_limit_precedes_manifest_reads(self):
        candidate = self.provider("candidate")
        for index in range(198):
            (self.workspace / f"unrelated-{index}").mkdir()
        self.assertEqual(tool.discover(self.root), f"test.provider={candidate}")
        (candidate / "manifest.json").write_text("invalid JSON")
        (self.workspace / "directory-201").mkdir()
        with self.assertRaisesRegex(ValueError, "more than 200 immediate directories"):
            tool.discover(self.root)

    def test_malformed_candidate_is_not_silently_ignored(self):
        candidate = self.provider("broken")
        (candidate / "manifest.json").write_text("{")
        with self.assertRaisesRegex(ValueError, "invalid provider candidate") as caught:
            tool.discover(self.root)
        self.assertIn(str(candidate), str(caught.exception))

    def test_sdk_contract_and_required_packaged_files(self):
        candidate = self.provider("candidate")
        manifest_path = candidate / "manifest.json"
        original = json.loads(manifest_path.read_text())
        for changes in ({"api": "999"}, {"format": 1}, {"id": "invalid"},
                        {"entry": "logic/missing.mjs"}, {"entry": "../escape.mjs"},
                        {"icon": "assets/missing.svg"}, {"ui": {"login": "ui/Missing.qml"}},
                        {"entry": "unpackaged/main.mjs"}):
            with self.subTest(changes=changes):
                manifest_path.write_text(json.dumps(original | changes))
                with self.assertRaisesRegex(ValueError, "invalid provider candidate"):
                    tool.discover(self.root)

    def test_symlinked_required_file_is_rejected(self):
        candidate = self.provider("candidate")
        entry = candidate / "logic/main.mjs"
        entry.unlink()
        target = self.workspace / "external.mjs"
        target.write_text("export function createSource() {}\n")
        entry.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "symlinks are not allowed"):
            tool.discover(self.root)

    def test_semicolon_path_is_rejected(self):
        self.provider("ambiguous;checkout")
        with self.assertRaisesRegex(ValueError, "paths cannot contain semicolons"):
            tool.discover(self.root)

    def test_no_candidates_yields_empty_override_list(self):
        (self.workspace / "unrelated").mkdir()
        self.assertEqual(tool.discover(self.root), "")


if __name__ == "__main__":
    unittest.main()
