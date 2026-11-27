#!/usr/bin/env python3
import re
from pathlib import Path
import unittest


class ArtifactAdmission(unittest.TestCase):
    def test_checked_in_toolchain_job_predicate(self):
        workflow = (Path(__file__).parents[2] / ".github/workflows/build-artifacts.yml").read_text()
        job = workflow.split("  toolchain:", 1)[1].split("    outputs:", 1)[0]
        expression = re.search(r"if: \$\{\{ (.*?) \}\}", job).group(1)
        for old, new in (("inputs.release", "release"), ("github.event_name", "event"),
                         ("github.ref_name", "ref"), ("true", "True"), ("||", " or "), ("&&", " and ")):
            expression = expression.replace(old, new)
        cases = [("push", "v0.10.2", True, True), ("pull_request", "topic", False, True),
                 ("push", "master", False, True), ("push", "release/0.10.2", False, True),
                 ("push", "topic", False, False), ("push", "v0.10.2", False, False),
                 ("workflow_dispatch", "topic", False, True), ("workflow_dispatch", "topic", True, True)]
        for event, ref, release, expected in cases:
            with self.subTest(event=event, ref=ref, release=release):
                actual = eval(expression, {"__builtins__": {}},
                              {"event": event, "ref": ref, "release": release,
                               "startsWith": lambda value, prefix: value.startswith(prefix)})
                self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main()
