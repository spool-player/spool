#!/usr/bin/env python3
"""Exercise webOS orchestration at its CLI/transport boundaries; no TV needed."""
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
STUB = r'''
import json, os, pathlib, subprocess, sys, time
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as out:
    out.write(json.dumps([name, args]) + '\n')
scenario = os.environ.get('SCENARIO', '')
if name == 'ssh':
    command = args[-1]
    if scenario == 'capture-local':
        raise SystemExit(subprocess.run(command, shell=True, stdin=sys.stdin).returncode)
    if scenario == 'hang':
        time.sleep(60)
    if 'listLaunchPoints' in command:
        app = 'com.sachk.spool.other' if scenario == 'wrong-app' else 'com.sachk.spool'
        print(json.dumps(dict(returnValue=True, launchPoints=[dict(appId=app)])))
    elif 'getAppInfo' in command:
        print('{"returnValue":true,"appInfo":{"id":"com.sachk.spool"}}')
    elif 'executeOneShot' in command:
        print('{"returnValue":false,"errorText":"capture unavailable"}' if scenario == 'capture-fail' else '{"returnValue":true}')
    elif 'power/getPowerState' in command:
        print(json.dumps(dict(returnValue=True, state='Standby' if scenario == 'asleep' else 'Active')))
    elif 'hbchannel.service/install' in command:
        print('{"returnValue":true,"finished":true}', flush=True)
        time.sleep(60)
    elif 'luna://' in command:
        print('{"returnValue":true}')
elif name == 'scp':
    target = pathlib.Path(args[-1])
    if target.suffix == '.json':
        target.write_text(json.dumps({'samples': [dict(routeTo='home', wallMs=5, guiCpuMs=1,
            instanceMs=1, actualSwaps=1, maxGapMs=0, frameBudgetMs=16.67)]}))
    else:
        target.write_bytes(b'nonempty screenshot')
elif name == 'ip':
    print('127.0.0.1 dev lo src 127.0.0.1')
elif name == 'getent':
    print('127.0.0.1 STREAM stub.invalid')
elif name == 'nix':
    if scenario == 'profile-timeout':
        print(os.environ['FAKE_HEAPTRACK'])
        raise SystemExit(0)
    raise SystemExit('nix must not be entered for help or invalid arguments')
'''


class WebosToolingTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.work = pathlib.Path(temp.name)
        self.bin = self.work / 'bin'
        self.bin.mkdir()
        self.calls_path = self.work / 'calls.jsonl'
        self.env = os.environ | {'PATH': str(self.bin) + os.pathsep + os.environ['PATH'],
                                 'CALLS': str(self.calls_path), 'TV_HOST': 'root@stub.invalid',
                                 'WEBOS_COMMAND_TIMEOUT': '2'}
        for name in ('ssh', 'scp', 'ares-launch', 'ip', 'curl', 'getent', 'nix'):
            self.executable(self.bin / name, STUB, python=True)

    def executable(self, path, body, python=False):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text((f'#!{sys.executable}\n' if python else '#!/usr/bin/env bash\n') + textwrap.dedent(body))
        path.chmod(0o755)

    def run_script(self, relative, *args, root=ROOT, env=None):
        return subprocess.run(['bash', str(root / relative), *map(str, args)], cwd=self.work,
                              env=self.env | (env or {}), text=True, capture_output=True, timeout=15)

    def calls(self):
        return [json.loads(line) for line in self.calls_path.read_text().splitlines()] if self.calls_path.exists() else []

    def verify(self, *args, env=None, root=ROOT):
        return self.run_script('tools/webos/verify-device.sh', '--out', self.work / 'verify', *args, env=env, root=root)

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_help_and_invalid_arguments_have_no_external_side_effects(self):
        scripts = ('build-ipk.sh', 'tools/webos-native/build-webos.sh',
                   'tools/webos-native/build-third-party.sh', 'tools/webos-native/build-curl.sh',
                   'tools/webos-native/build-qcoro.sh', 'tools/webos-native/build-ffmpeg.sh',
                   'tools/webos/verify-device.sh', 'tools/webos/capture-memory.sh',
                   'tools/webos/profile-memory.sh', 'tools/webos/bench-device.sh',
                   'tools/webos/package-heaptrack-profile.sh')
        for script in scripts:
            with self.subTest(script=script):
                self.assert_success(self.run_script(script, '--help'))
                self.assertEqual(self.run_script(script, '--invalid').returncode, 2)
        for script, option in (('verify-device.sh', '--host'), ('profile-memory.sh', '--duration'),
                               ('capture-memory.sh', '--out'), ('bench-device.sh', '--library')):
            self.assertEqual(self.run_script('tools/webos/' + script, option).returncode, 2)
        for script, arguments in (
                ('verify-device.sh', ('--app-id', "invalid';id")),
                ('verify-device.sh', ('--install-timeout', '0')),
                ('capture-memory.sh', ('--duration', '0')),
                ('capture-memory.sh', ('--interval', '1;id')),
                ('profile-memory.sh', ('--duration', '-1'))):
            self.assertEqual(self.run_script('tools/webos/' + script, *arguments).returncode, 2)
        self.assertEqual(self.calls(), [])

    def test_read_only_verification_never_launches_or_captures(self):
        self.assert_success(self.verify('--no-install'))
        calls = self.calls()
        self.assertEqual([name for name, _ in calls], ['ssh', 'ssh'])
        self.assertIn('listLaunchPoints', calls[0][1][-1])
        self.assertIn('getAppInfo', calls[1][1][-1])
        self.assertTrue(all('BatchMode=yes' in args and 'ConnectTimeout=10' in args for _, args in calls))

    def test_registration_requires_exact_app_identity(self):
        result = self.verify('--no-install', env={'SCENARIO': 'wrong-app'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('not registered', result.stderr)
        self.assertEqual(len(self.calls()), 1)

    def test_explicit_launch_captures_and_cleans_only_its_screenshot(self):
        self.assert_success(self.verify('--no-install', '--launch', '--launch-wait', '0'))
        calls = self.calls()
        self.assertEqual(sum(name == 'ares-launch' for name, _ in calls), 1)
        self.assertTrue((self.work / 'verify/screenshot.png').is_file())
        removes = [args[-1] for name, args in calls if name == 'ssh' and 'rm -f' in args[-1]]
        self.assertEqual(len(removes), 1)
        self.assertIn('/tmp/com.sachk.spool-verify-', removes[0])

    def test_capture_service_rejection_is_not_a_successful_verification(self):
        result = self.verify('--no-install', '--launch', '--launch-wait', '0', env={'SCENARIO': 'capture-fail'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('screenshot capture failed', result.stderr)
        self.assertFalse(any(name == 'scp' for name, _ in self.calls()))

    def test_unresponsive_transport_is_bounded(self):
        result = self.verify('--no-install', env={'SCENARIO': 'hang', 'WEBOS_COMMAND_TIMEOUT': '1'})
        self.assertEqual(result.returncode, 124)
        self.assertFalse(any(name == 'ares-launch' for name, _ in self.calls()))

    def test_profile_wait_failure_disarms_without_launching_or_closing_app(self):
        heaptrack = self.work / 'heaptrack'
        for relative in ('lib/heaptrack/libexec/heaptrack_interpret', 'bin/heaptrack_print'):
            self.executable(heaptrack / relative, 'exit 0\n')
        result = self.run_script('tools/webos/profile-memory.sh', '--wait-timeout', '1',
            '--out', self.work / 'profile', env={'SCENARIO': 'profile-timeout', 'FAKE_HEAPTRACK': str(heaptrack)})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Timed out waiting', result.stderr)
        commands = [args[-1] for name, args in self.calls() if name == 'ssh']
        self.assertTrue(any(": > '/tmp/spool-heaptrack.on'" in command for command in commands))
        self.assertEqual(commands[-1], "rm -f '/tmp/spool-heaptrack.on'")
        self.assertFalse(any('applicationManager/' in command for command in commands))

    def copy_script(self, root, relative):
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / relative, destination)

    def test_default_install_chooses_mtime_not_lexical_version_and_never_launches(self):
        root = self.work / 'checkout'
        for relative in ('tools/webos/verify-device.sh', 'tools/webos/device-common.sh', 'tools/webos/luna-response.py'):
            self.copy_script(root, relative)
        build = root / 'build'
        build.mkdir()
        older = build / 'com.sachk.spool_99.0.0_arm.ipk'
        newer = build / 'com.sachk.spool_1.0.0_arm.ipk'
        older.write_bytes(b'old'); newer.write_bytes(b'new')
        os.utime(older, (10, 10)); os.utime(newer, (20, 20))
        with socket.socket() as available:
            available.bind(('127.0.0.1', 0))
            port = available.getsockname()[1]
        self.assert_success(self.verify('--http-port', port, root=root))
        calls = self.calls()
        install = next(args[-1] for name, args in calls if name == 'ssh' and 'hbchannel.service/install' in args[-1])
        self.assertIn(newer.name, install)
        self.assertNotIn(older.name, install)
        self.assertFalse(any(name == 'ares-launch' for name, _ in calls))
        route = next(args for name, args in calls if name == 'ip')
        self.assertEqual(route, ['route', 'get', '127.0.0.1'])
        self.assertTrue(all('root@stub.invalid' in args for name, args in calls if name == 'ssh'))
        self.calls_path.unlink()
        self.assert_success(self.verify('--http-port', port, '--host', 'root@127.0.0.1', root=root))
        self.assertTrue(all('root@127.0.0.1' in args for name, args in self.calls() if name == 'ssh'))

    def test_capture_clamps_large_interval_and_preserves_completed_csv(self):
        output = self.work / 'capture.csv'
        started = time.monotonic()
        result = self.run_script('tools/webos/capture-memory.sh', '--duration', '1',
            '--interval', '120', '--wait-timeout', '1', '--no-plot', '--out', output,
            env={'SCENARIO': 'capture-local'})
        self.assert_success(result)
        self.assertLess(time.monotonic() - started, 5)
        self.assertGreater(len(output.read_text().splitlines()), 1)

    def test_benchmark_never_patches_installed_tree_or_wakes_panel(self):
        result = self.run_script('tools/webos/bench-device.sh', '--out', self.work / 'bench')
        self.assert_success(result)
        commands = [args[-1] for name, args in self.calls() if name == 'ssh']
        for command in commands:
            self.assertNotIn('chown', command)
            self.assertNotIn('mv spool', command)
            self.assertNotIn('turnOnScreen', command)
            self.assertNotIn('com.webos.app.hdmi', command)
        self.assertTrue(any("printf '%s\\n'" in command and '/tmp/spool-benchmark.on' in command for command in commands))
        self.assertTrue(any('applicationManager/launch' in command for command in commands))

    def test_sleeping_panel_is_rejected_before_benchmark_mutation(self):
        result = self.run_script('tools/webos/bench-device.sh', env={'SCENARIO': 'asleep'})
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(len(self.calls()), 1)

    def test_packaged_shim_forwards_data_without_evaluating_shell(self):
        app = self.work / 'app'
        (app / 'bin').mkdir(parents=True)
        self.copy_script(app, 'tools/webos/heaptrack-launch-shim.sh')
        shutil.copy2(app / 'tools/webos/heaptrack-launch-shim.sh', app / 'bin/spool')
        self.executable(app / 'bin/spool.real', 'import json,os; print(json.dumps({key:value for key,value in os.environ.items() if key.startswith("SPOOL_BENCH")}))\n', python=True)
        marker = self.work / 'benchmark.marker'
        sentinel = self.work / 'must-not-execute'
        library = f"Movies'; touch '{sentinel}'; '"
        marker.write_text('\n'.join(('spool-benchmark-v1', 'routes', '2', library, '1', '0', '3', '/tmp/report.json')) + '\n')
        result = self.run_script('bin/spool', root=app, env={'SPOOL_BENCH_MARKER': str(marker),
            'SPOOL_HEAPTRACK_MARKER': str(self.work / 'absent'), 'SPOOL_BIGALLOC_MARKER': str(self.work / 'absent')})
        self.assert_success(result)
        forwarded = json.loads(result.stdout)
        self.assertEqual(forwarded['SPOOL_BENCH_LIBRARY'], library)
        self.assertEqual(forwarded['SPOOL_BENCH_OUT'], '/tmp/report.json')
        self.assertFalse(sentinel.exists())
        marker.write_text('spool-benchmark-v1\nroutes\n')
        self.assert_success(result := self.run_script('bin/spool', root=app, env={'SPOOL_BENCH_MARKER': str(marker)}))
        self.assertNotIn('SPOOL_BENCH', json.loads(result.stdout))


if __name__ == '__main__':
    unittest.main()
