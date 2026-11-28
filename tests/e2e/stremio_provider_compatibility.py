#!/usr/bin/env python3
"""Run the shipped Stremio provider through fake and upstream-SDK add-ons."""

from compatibility_receipt import compatibility_runner, fixture_failure, receipt_results, set_phase

import argparse
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import tempfile


def check_fixture(server, media, mode, spool_tests, result):
    with subprocess.Popen(['node', str(server), mode, str(media)],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) as addon:
        try:
            with selectors.DefaultSelector() as poll:
                poll.register(addon.stdout, selectors.EVENT_READ)
                origin = addon.stdout.readline().strip() if poll.select(timeout=15) else ''
            if not re.fullmatch(r'http://127\.0\.0\.1:[0-9]+', origin):
                raise RuntimeError('add-on did not publish a loopback origin')
            set_phase("selector")
            process = subprocess.run([str(spool_tests), '--child', 'stremio-addon-provider'], timeout=75,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                    env=os.environ | {'SPOOL_TEST_STREMIO_ORIGIN': origin,
                                                      'NO_PROXY': '127.0.0.1', 'no_proxy': '127.0.0.1'})
            for line in process.stderr.splitlines():
                if line.startswith('spool-test-require: '):
                    print(f'{mode}: {line}')
            result.update(passed=process.returncode == 0, selectorExitCode=process.returncode)
            return process.returncode
        finally:
            if "selectorExitCode" in result:
                set_phase("cleanup")
            if addon.poll() is None:
                addon.terminate()
                try:
                    addon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    addon.kill()
                    addon.wait(timeout=5)


@compatibility_runner('stremio')
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--spool-tests', required=True, type=Path)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args()
    server = Path(__file__).resolve().parent / 'fixtures' / 'stremio-addon' / 'server.mjs'
    results = receipt_results()
    set_phase("synthetic-media")
    with tempfile.TemporaryDirectory(prefix='spool-stremio-media-') as directory:
        media = Path(directory) / 'fixture.mp4'
        subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                        '-i', 'testsrc2=size=160x90:rate=12:duration=2', '-c:v', 'libx264',
                        '-pix_fmt', 'yuv420p', '-movflags', 'faststart', str(media)], check=True, timeout=30)
        for mode in ('fake', 'sdk'):
            try:
                set_phase("server-startup")
                results[mode] = {}
                code = check_fixture(server, media, mode, args.spool_tests.resolve(), results[mode])
                results[mode] = {'passed': code == 0, 'selectorExitCode': code}
            except (RuntimeError, OSError, subprocess.SubprocessError) as error:
                fixture_failure(results, mode, error)
            print(f"Stremio {mode} fixture: {'passed' if results[mode]['passed'] else 'failed'}")
            set_phase("cleanup")
    set_phase("cleanup")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({'provider': 'stremio', 'sdkVersion': '1.1.4', 'fixtures': results}, indent=2) + '\n',
                           encoding='utf-8')
    return 0 if all(result['passed'] for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
