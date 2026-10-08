#!/usr/bin/env python3
"""Exercise the actual phase driver against real CTest subprocess workloads."""
import json
from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
