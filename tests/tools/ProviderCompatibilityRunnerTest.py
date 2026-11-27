#!/usr/bin/env python3
# Offline checks: never launch an app, a container or a provider endpoint.
from contextlib import contextmanager
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1] / "e2e"))
receipt = importlib.import_module("compatibility_receipt")
modules = [importlib.import_module(name + "_provider_compatibility")
           for name in ("emby", "plex", "stremio", "open_movies")]


class Receipts(unittest.TestCase):
    def run_main(self, module, directory):
        path = Path(directory) / "receipt.json"
        with patch.object(sys, "argv", ["runner", "--report", str(path), "--spool-tests", "unused"]):
            code = module.main()
        result = json.loads(path.read_text())
        self.assertNotIn("SECRET", json.dumps(result))
        return code, result

    def test_missing_and_timed_out_ffmpeg_always_leave_receipts(self):
        for module in modules:
            for error in (FileNotFoundError("SECRET"), subprocess.TimeoutExpired("SECRET", 1)):
                with self.subTest(module=module.__name__, error=type(error).__name__), tempfile.TemporaryDirectory() as directory:
                    with patch.object(module.subprocess, "run", side_effect=error):
                        code, result = self.run_main(module, directory)
                    self.assertEqual(code, 1)
                    self.assertEqual(result["phase"], "synthetic-media")
                    self.assertEqual(result["errorType"], type(error).__name__)
                    self.assertEqual(result["fixtures"], {})

    def test_openssl_failure_leaves_receipt(self):
        module = modules[-1]
        def run(command, **kwargs):
            if command[0] == "openssl":
                raise subprocess.CalledProcessError(1, "SECRET")
            return subprocess.CompletedProcess(command, 0, stderr="")
        with tempfile.TemporaryDirectory() as directory, patch.object(module.subprocess, "run", side_effect=run):
            code, result = self.run_main(module, directory)
        self.assertEqual(code, 1)
        self.assertEqual(result["errorType"], "CalledProcessError")

    def test_server_selector_and_cleanup_results_are_distinct(self):
        for module in modules[:2]:
            for failure in ("server-startup", "selector", "cleanup", None):
                with self.subTest(module=module.__name__, failure=failure), tempfile.TemporaryDirectory() as directory:
                    @contextmanager
                    def fixture(*args):
                        if failure == "server-startup":
                            raise RuntimeError("SECRET")
                        yield "http://127.0.0.1:1", None
                        if failure == "cleanup":
                            raise RuntimeError("SECRET")
                    def run(command, **kwargs):
                        code = 7 if command[0] != "ffmpeg" and failure == "selector" else 0
                        return subprocess.CompletedProcess(command, code, stderr="")
                    with patch.object(module, "fake_server", fixture), patch.object(module, "upstream_server", fixture), patch.object(module.subprocess, "run", side_effect=run):
                        code, result = self.run_main(module, directory)
                    self.assertEqual(code, 0 if failure is None else 1)
                    for row in result["fixtures"].values():
                        self.assertEqual(row["passed"], failure is None)
                        if failure == "selector":
                            self.assertEqual(row["selectorExitCode"], 7)
                        if failure == "cleanup":
                            self.assertEqual(row["selectorExitCode"], 0)
                            self.assertEqual(row["cleanupErrorType"], "RuntimeError")
                        if failure in ("server-startup", "cleanup"):
                            self.assertEqual(row["phase"], failure)


if __name__ == "__main__":
    unittest.main()
