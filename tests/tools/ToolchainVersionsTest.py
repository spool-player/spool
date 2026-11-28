#!/usr/bin/env python3
"""Keep toolchain pin validation and platform cache boundaries under test."""

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("toolchain_versions", ROOT / "tools/toolchain-versions.py")
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)


class ToolchainVersionsTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="spool-toolchain-test-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        for name in ("tools", ".github"):
            shutil.copytree(ROOT / name, self.root / name)
        for name in ("flake.nix", "flake.lock", "renovate.json"):
            shutil.copy2(ROOT / name, self.root / name)

    def test_repository_versions(self):
        self.assertEqual(toolchain.version_errors(self.root), [])
        pins = toolchain.load(self.root / "tools/manifests/toolchain.json")
        outputs = toolchain.outputs(self.root, "a" * 40)
        self.assertEqual(outputs["apple_qt_modules"].split(), pins["apple"]["qtModules"])
        self.assertEqual(outputs["android_build_tools"], pins["android"]["buildTools"])
        self.assertEqual(outputs["qt_build_tag"], pins["qt"]["series"].replace(".", ""))

    def test_nix_archives_must_exist_for_each_runner(self):
        nix = {"version": "2.99.0", "quickInstallRelease": "v99"}
        archives = ["nix-2.99.0-x86_64-linux.tar.zstd", "nix-2.99.0-aarch64-darwin.tar.zstd",
                    "nix-2.99.0-x86_64-darwin.tar.zstd"]
        self.assertEqual(toolchain.nix_archive_errors(nix, archives), [])
        self.assertEqual(toolchain.nix_archive_errors(nix, archives[:-1] + ["nix-2.99.1-x86_64-darwin.tar.zstd"]),
                         ["Nix installer v99 is missing archives: nix-2.99.0-x86_64-darwin.tar.zstd"])

    def test_windows_and_mpv_edits_leave_source_qt_caches_warm(self):
        before = toolchain.outputs(self.root, "a" * 40)
        windows = self.root / "tools/windows/install-qt.ps1"
        with windows.open("a", encoding="utf-8") as handle:
            handle.write("\n# edits to Windows package setup\n")
        lock_file = self.root / "flake.lock"
        lock = toolchain.load(lock_file)
        lock["nodes"]["mpv-src"]["locked"]["rev"] = "b" * 40
        lock_file.write_text(json.dumps(lock), encoding="utf-8")
        after = toolchain.outputs(self.root, "b" * 40)
        for key in ("key_windows_qt", "key_mpv", "key_native_shell", "key_linux_build", "key_webos_build"):
            self.assertNotEqual(before[key], after[key], key)
        for key in ("key_apple_host", "key_ios_kit", "key_tvos_qt", "key_apple_deps",
                    "key_android_qt", "key_webos_deps"):
            self.assertEqual(before[key], after[key], key)

    def test_qt_digest_change_only_affects_builds_that_use_it(self):
        before = toolchain.outputs(self.root, "a" * 40)
        path = self.root / "tools/manifests/toolchain.json"
        pins = toolchain.load(path)
        pins["qt"]["sources"]["qtwayland"] = "0" * 64
        path.write_text(json.dumps(pins), encoding="utf-8")
        after = toolchain.outputs(self.root, "a" * 40)
        self.assertNotEqual(before["key_webos_deps"], after["key_webos_deps"])
        self.assertEqual(before["key_tvos_qt"], after["key_tvos_qt"])
        self.assertEqual(before["key_android_qt"], after["key_android_qt"])
        self.assertEqual(before["key_windows_qt"], after["key_windows_qt"])

    def test_qcoro_edit_does_not_invalidate_android_media_or_qt(self):
        before = toolchain.outputs(self.root, "a" * 40)
        path = self.root / "tools/manifests/android-third-party.json"
        pins = toolchain.load(path)
        pins["sources"]["qcoro"]["sha256"] = "0" * 64
        path.write_text(json.dumps(pins), encoding="utf-8")
        after = toolchain.outputs(self.root, "a" * 40)
        for key in ("key_android_deps", "key_android_qt"):
            self.assertEqual(before[key], after[key], key)
        self.assertNotEqual(before["key_apple_deps"], after["key_apple_deps"])

    def test_android_dependency_recipe_edit_invalidates_android_qt(self):
        before = toolchain.outputs(self.root, "a" * 40)
        recipe = self.root / "tools/android/build-dependencies.sh"
        recipe.write_text(recipe.read_text(encoding="utf-8").replace("--with-brotli=no ", "--with-brotli=yes ", 1),
                          encoding="utf-8")
        after = toolchain.outputs(self.root, "a" * 40)
        for key in ("key_android_qt", "key_android_deps"):
            self.assertNotEqual(before[key], after[key], key)
        self.assertEqual(before["key_tvos_qt"], after["key_tvos_qt"])

    def test_android_min_sdk_pin_updates_all_apk_deployment_settings(self):
        for name in ("cmake/platform", "app/android-common", "tests/platform/android"):
            shutil.copytree(ROOT / name, self.root / name)
        shutil.copy2(ROOT / "tests/platform/MobileTestBundles.cmake", self.root / "tests/platform")
        shutil.copy2(Path(__file__).with_name("android-packaging") / "CMakeLists.txt", self.root)

        build = self.root / "build"
        manifest = self.root / "tools/manifests/toolchain.json"
        pins = toolchain.load(manifest)

        def check_deployment_settings():
            for target in ("spool", "spool-tests", "spool-e2e-tests"):
                settings = toolchain.load(build / f"android-{target}-deployment-settings.json")
                self.assertEqual(settings["android-min-sdk-version"], pins["android"]["minimumSdk"], target)

        subprocess.run(["cmake", "-S", str(self.root), "-B", str(build)], check=True, stdout=subprocess.PIPE)
        check_deployment_settings()

        minimum_sdk = int(pins["android"]["minimumSdk"])
        compile_sdk = int(pins["android"]["compileSdk"])
        pins["android"]["minimumSdk"] = str(minimum_sdk + 1 if minimum_sdk < compile_sdk else minimum_sdk - 1)
        manifest.write_text(json.dumps(pins), encoding="utf-8")
        subprocess.run(["cmake", "--build", str(build)], check=True, stdout=subprocess.PIPE)
        check_deployment_settings()

    def test_guard_rejects_duplicates_literal_versions_and_partial_caches(self):
        android = self.root / "tools/manifests/qt-android.json"
        data = toolchain.load(android)
        data["qtVersion"] = "99.0.0"
        android.write_text(json.dumps(data), encoding="utf-8")

        workflow = self.root / ".github/workflows/tvos.yml"
        lines = workflow.read_text(encoding="utf-8").splitlines()
        line = next(index for index, text in enumerate(lines) if "key: tvos-qt-" in text)
        lines.insert(line + 1, "          restore-keys: tvos-qt-")
        lines.append("      APPLE_DEPLOYMENT_TARGET: '99.0'")
        workflow.write_text("\n".join(lines) + "\n", encoding="utf-8")

        errors = "\n".join(toolchain.version_errors(self.root))
        self.assertIn("qt-android.json repeats toolchain.json pins", errors)
        self.assertIn("repeats a toolchain pin", errors)
        self.assertIn("restores a partial toolchain cache", errors)


if __name__ == "__main__":
    unittest.main()
