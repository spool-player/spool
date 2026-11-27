#!/usr/bin/env python3
"""Required CI must fail with a receipt when its real-server tools are absent."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("latency", Path(__file__).parents[1] / "e2e/jellyfin_latency_e2e.py")
latency = importlib.util.module_from_spec(spec)
spec.loader.exec_module(latency)


class Prerequisites(unittest.TestCase):
    def test_missing_tools_produce_optional_skip_and_required_failure(self):
        for required, expected in ((False, 77), (True, 1)):
            with tempfile.TemporaryDirectory() as directory:
                report = Path(directory) / "receipt.json"
                arguments = ["latency", "--libmpv", "unused", "--transport-helper", "unused",
                             "--shaper-script", "unused", "--output", str(report)]
                if required:
                    arguments.append("--required")
                with patch.object(sys, "argv", arguments), patch.object(latency.shutil, "which", return_value=None):
                    self.assertEqual(latency.main(), expected)
                result = json.loads(report.read_text())
                self.assertEqual(result["status"], "failed" if required else "skipped")
                self.assertIn("jellyfin", result["missing"])
                self.assertEqual(result["scenarios"], {})

    def test_required_https_and_provider_prerequisites(self):
        with tempfile.TemporaryDirectory() as directory:
            native = Path(directory) / "spool-tests"
            native.touch()
            with patch.object(latency.shutil, "which", side_effect=lambda name: None if name == "nginx" else name):
                self.assertEqual(latency.prerequisites("jellyfin", "ffmpeg", required=True,
                                                     spool_tests=native), ["nginx"])
            with patch.object(latency.shutil, "which", return_value="tool"):
                self.assertEqual(latency.prerequisites("jellyfin", "ffmpeg", required=True,
                                                     spool_tests=native), [])


if __name__ == "__main__":
    unittest.main()
