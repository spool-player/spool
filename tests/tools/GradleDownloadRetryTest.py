import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RETRY = ROOT / "tools/android/retry-gradle-download.sh"
RATE_LIMIT = (
    "Could not GET 'https://repo.maven.apache.org/maven2/dependency.pom'. "
    "Received status code 429 from server: Too Many Requests"
)

with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory)
    sleep = fixture / "sleep"
    sleep.write_text('#!/usr/bin/env bash\nprintf "%s\\n" "$1" >> sleeps\n', encoding="utf-8")
    sleep.chmod(0o755)
    command = fixture / "build.sh"
    command.write_text(
        """printf 'attempt\\n' >> attempts
if [[ "$TEST_CASE" == recover && $(wc -l < attempts) -eq 2 ]]; then
    exit 0
fi
printf '%s\\n' "$TEST_MESSAGE" >&2
exit 7
""",
        encoding="utf-8",
    )

    for case, message, returncode, delays in (
        ("recover", RATE_LIMIT, 0, ["30"]),
        ("exhaust", RATE_LIMIT, 7, ["30", "90"]),
        ("build-error", "compiler: missing header", 7, []),
        ("other-host", RATE_LIMIT.replace("repo.maven.apache.org", "dl.google.com"), 7, []),
    ):
        for name in ("attempts", "sleeps"):
            (fixture / name).unlink(missing_ok=True)
        env = dict(os.environ, PATH=f"{fixture}{os.pathsep}{os.environ['PATH']}",
                   TEST_CASE=case, TEST_MESSAGE=message)
        result = subprocess.run(
            ["bash", str(RETRY), "bash", str(command)],
            cwd=fixture, env=env, check=False, text=True, capture_output=True,
        )
        assert result.returncode == returncode, (case, result.returncode, result.stderr)
        sleeps = (fixture / "sleeps").read_text().splitlines() if (fixture / "sleeps").exists() else []
        assert sleeps == delays, (case, sleeps)
        assert (fixture / "attempts").read_text().splitlines() == ["attempt"] * (len(delays) + 1)

print("Gradle build retries only the observed Maven Central rate limit.")
