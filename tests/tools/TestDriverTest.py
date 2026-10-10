#!/usr/bin/env python3
"""Exercise phase scheduling and native device receipt/report boundaries."""
import copy
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TestPhaseDriver(unittest.TestCase):
    def test_failure_never_short_circuits_or_overlaps_phases(self):
        with tempfile.TemporaryDirectory(prefix="spool-phase-contract-") as directory:
            root = Path(directory)
            source = root / "source"
            build = root / "build"
            source.mkdir()
            workload = source / "workload.py"
            workload.write_text('''import pathlib, sys, time
root = pathlib.Path(sys.argv[1])
case = sys.argv[2]
if case == "slow":
    (root / "slow-started").write_text("started")
    time.sleep(0.3)
    (root / "slow-finished").write_text("finished")
    sys.exit(0)
if case == "fail":
    (root / "failure-finished").write_text("failed")
    sys.exit(1)
assert (root / "slow-finished").is_file(), "e2e started before slow traditional selector finished"
assert (root / "failure-finished").is_file(), "e2e started before failed traditional selector finished"
(root / "e2e-finished").write_text("ran despite traditional failure")
sys.exit(2)
''', encoding="utf-8")
            # Use CMake's own quoting and real multi-config CTest scheduling,
            # not a replacement ctest or a copy of the driver's phase decision.
            cmake = ["cmake_minimum_required(VERSION 3.22)", "project(PhaseContract NONE)", "enable_testing()"]
            for name, phase in (("slow", "traditional"), ("fail", "traditional"), ("gui", "e2e")):
                cmake.extend([
                    f'add_test(NAME {name} COMMAND [[{sys.executable}]] [[{workload}]] [[{root}]] {name})',
                    f'set_tests_properties({name} PROPERTIES LABELS {phase})',
                ])
            (source / "CMakeLists.txt").write_text("\n".join(cmake) + "\n", encoding="utf-8")
            # Force configuration selection on every host, including Linux:
            # an omitted --config must not pass via a single-config generator.
            configured = subprocess.run(["cmake", "-G", "Ninja Multi-Config",
                                         "-S", str(source), "-B", str(build)],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout)
            # This is a scheduler fixture, not a graphical workload; device
            # suites likewise own their graphics instead of a host Weston session.
            (build / "spool-device-tests.json").write_text('{"format":1}\n', encoding="utf-8")
            result = subprocess.run([sys.executable, str(ROOT / "tools/run-tests.py"),
                                     "--build-dir", str(build), "--workers", "2", "--config", "Release"],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertTrue((root / "e2e-finished").is_file(), result.stdout)
            journal = json.loads((build / "test-phases.json").read_text(encoding="utf-8"))
            self.assertEqual([row["phase"] for row in journal["phases"]], ["traditional", "e2e"])
            self.assertTrue(all(row["exitCode"] != 0 for row in journal["phases"]))
            self.assertLessEqual(journal["phases"][0]["finished"], journal["phases"][1]["started"])
            self.assertTrue((root / "slow-finished").is_file(), result.stdout)
            self.assertTrue((root / "failure-finished").is_file(), result.stdout)


class TestSimulatorCrashReport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.adapter = runpy.run_path(str(ROOT / "tools/run-device-tests.py"))

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="spool-crash-contract-")
        self.addCleanup(directory.cleanup)
        self.report = Path(directory.name) / "spool-tests.ips"
        self.launch = {
            "pid": 1234, "bundle": "com.sachk.spool.tests",
            "device": "12345678-1234-1234-1234-123456789abc",
            "appPath": "/Users/runner/Library/Developer/CoreSimulator/Devices/"
                       "12345678-1234-1234-1234-123456789abc/data/Containers/Bundle/Application/"
                       "87654321-4321-4321-4321-cba987654321/spool-tests.app",
            "executable": "spool-tests",
            "binaryIdentities": [["abcdef01-2345-6789-abcd-ef0123456789", "arm64"]],
            "started": 1780000000.0, "finished": 1780000002.0,
        }
        self.metadata = {"bundleID": "com.sachk.spool.tests", "bug_type": "309"}
        self.payload = {
            "pid": 1234, "bundleInfo": {"CFBundleIdentifier": "com.sachk.spool.tests"},
            "procPath": "/Users/runner/*/spool-tests.app/spool-tests",
            "coalitionName": "com.apple.CoreSimulator.SimDevice.12345678-1234-1234-1234-123456789abc",
            "captureTime": "2026-05-28 20:26:41.000 +0000",
            "usedImages": [{"source": "P", "name": "spool-tests", "arch": "arm64",
                            "uuid": "abcdef01-2345-6789-abcd-ef0123456789"}],
        }

    def match(self, payload, metadata=None):
        self.report.write_text(json.dumps(self.metadata if metadata is None else metadata)
                               + "\n" + json.dumps(payload), encoding="utf-8")
        os.utime(self.report, (self.launch["finished"], self.launch["finished"]))
        return self.adapter["matching_simulator_report"](self.report, self.launch)

    def test_privacy_redacted_report_requires_installed_binary_and_simulator(self):
        self.assertIsNotNone(self.match(self.payload))
        changes = [
            {"pid": 1235},
            {"bundleInfo": {"CFBundleIdentifier": "com.sachk.spool.e2e-tests"}},
            {"coalitionName": "com.apple.CoreSimulator.SimDevice.other"},
            {"procPath": "/Users/runner/*/other.app/spool-tests"},
            {"procPath": "/Users/runner/other/spool-tests.app/spool-tests"},
            {"captureTime": "2026-05-28 20:26:43.000 +0000"},
            {"usedImages": []},
        ]
        for field, value in (("uuid", "abcdef01-2345-6789-abcd-ef0123456788"),
                             ("arch", "x86_64"), ("source", "S"), ("name", "other")):
            images = copy.deepcopy(self.payload["usedImages"])
            images[0][field] = value
            changes.append({"usedImages": images})
        for change in changes:
            with self.subTest(change=change):
                self.assertIsNone(self.match({**self.payload, **change}))
        self.assertIsNone(self.match(self.payload, {"bundleID": "other", "bug_type": "309"}))

    def test_unredacted_report_keeps_exact_installed_path_and_launch_window(self):
        payload = {**self.payload, "procPath": self.launch["appPath"] + "/spool-tests"}
        self.assertIsNotNone(self.match(payload))
        self.assertIsNone(self.match({**payload, "procPath": payload["procPath"].replace(
            self.launch["device"], "other")}))
        self.assertIsNone(self.match({**payload, "captureTime": "2026-05-28 20:26:39.000 +0000"}))


    def test_matched_report_export_keeps_offsets_but_not_private_report_strings(self):
        payload = copy.deepcopy(self.payload)
        private = "private-report-content-must-not-be-exported"
        payload.update(faultingThread=0, threads=[{"frames": [
            {"imageIndex": 0, "imageOffset": 42, "symbol": private}]}],
            lastExceptionBacktrace=[{"imageIndex": 0, "imageOffset": 43, "symbol": private}],
            asi={"messages": [private]})
        payload["usedImages"][0]["path"] = private
        matched = self.match(payload)
        self.assertIsNotNone(matched)
        exported = self.adapter["native_report_fields"](matched)
        self.assertEqual(exported["frames"], [{"imageIndex": 0, "imageOffset": 42}])
        self.assertEqual(exported["lastExceptionFrames"], [{"imageIndex": 0, "imageOffset": 43}])
        self.assertNotIn(private, json.dumps(exported))


if __name__ == "__main__":
    unittest.main()
