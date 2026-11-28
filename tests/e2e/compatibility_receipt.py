# Sanitized receipts survive ordinary fixture setup and cleanup failures.
import argparse
from functools import wraps
import json
from pathlib import Path

_active = None


def set_phase(phase):
    if phase not in ("prerequisites", "synthetic-media", "server-startup", "selector", "cleanup"):
        raise ValueError("unknown compatibility phase")
    _active["phase"] = phase


def receipt_results():
    return _active["fixtures"]


def fixture_failure(results, name, error):
    previous = results.get(name, {})
    cleanup = "selectorExitCode" in previous
    results[name] = {**previous, "passed": False, "errorType": type(error).__name__,
                     "phase": "cleanup" if cleanup else _active["phase"]}
    if cleanup:
        results[name]["cleanupErrorType"] = type(error).__name__


def compatibility_runner(provider):
    def decorate(main):
        @wraps(main)
        def run():
            global _active
            parser = argparse.ArgumentParser(add_help=False)
            parser.add_argument("--report", type=Path)
            known, _ = parser.parse_known_args()
            if known.report is None:
                return main()
            path = known.report
            _active = {"format": 1, "provider": provider, "status": "failed",
                       "phase": "prerequisites", "fixtures": {}}
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(_active) + "\n", encoding="utf-8")
            code = 1
            try:
                code = main()
            except Exception as error:
                _active["errorType"] = type(error).__name__
                # Never export exception strings, endpoints or server output.
                print(provider + ": failed during " + _active["phase"] + " (" + type(error).__name__ + ")")
            finally:
                saved = json.loads(path.read_text(encoding="utf-8"))
                saved.update(_active)
                saved["status"] = "passed" if code == 0 else "failed"
                path.write_text(json.dumps(saved, indent=2) + "\n", encoding="utf-8")
                _active = None
            return code
        return run
    return decorate
