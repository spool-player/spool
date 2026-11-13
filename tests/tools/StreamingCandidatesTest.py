#!/usr/bin/env python3
"""Exercise the streaming-candidate report at its spoolet CLI boundary."""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/find-streaming-candidates.py'
NOISE = 'provider boom https://cdn.invalid/stream.mpd?token=SECRET-TOKEN credential=abc123'

FAKE_SPOOLET = r'''#!/usr/bin/env python3
import json, os, sys

argv = sys.argv[1:]
if len(argv) != 6 or argv[0] != '--instance' or argv[2] != 'catalog':
    sys.exit(64)
library, offset, limit = argv[3], int(argv[4]), int(argv[5])
if limit <= 0:
    sys.exit(64)
with open(os.environ['SPOOL_FAKE_LOG'], 'a') as log:
    log.write('{} {}\n'.format(library, offset))
entry = json.loads(open(os.environ['SPOOL_FAKE_CATALOG']).read())['libraries'][library]
failure = entry.get('fail')
if failure == 'exit':
    sys.stderr.write(entry['noise'])
    sys.exit(3)
if failure == 'json':
    sys.stdout.write(entry['noise'])
    sys.exit(0)
if failure == 'ok':
    json.dump({'ok': False, 'error': {'message': entry['noise']}}, sys.stdout)
    sys.exit(0)
page = entry['pages'].get(str(offset), {'items': [], 'exhausted': True})
json.dump({'ok': True, 'result': {'items': page['items'], 'offset': offset, 'total': entry['total'],
                                  'exhausted': page['exhausted'], 'seekable': entry['seekable']}}, sys.stdout)
'''


def source(bitrate=None, size=None, width=None, height=None):
    entry = {}
    for key, value in (('bitrate', bitrate), ('sizeBytes', size), ('width', width), ('height', height)):
        if value is not None:
            entry[key] = value
    return entry


def item(item_id, sources, playable=True, **extra):
    entry = {'id': item_id, 'title': 'Title ' + item_id, 'playable': playable, 'sources': sources}
    entry.update(extra)
    return entry


def library(total, seekable, pages, **extra):
    entry = {'total': total, 'seekable': seekable, 'pages': pages, 'noise': NOISE}
    entry.update(extra)
    return entry


class StreamingCandidatesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fake = self.root / 'spoolet'
        self.fake.write_text(FAKE_SPOOLET)
        self.fake.chmod(self.fake.stat().st_mode | stat.S_IEXEC)
        self.log = self.root / 'calls.log'

    def run_tool(self, config, arguments):
        catalog = self.root / 'catalog.json'
        catalog.write_text(json.dumps(config))
        env = dict(os.environ, SPOOL_FAKE_CATALOG=str(catalog), SPOOL_FAKE_LOG=str(self.log))
        return subprocess.run([sys.executable, str(SCRIPT), '--spoolet', str(self.fake), *arguments],
                              capture_output=True, text=True, env=env)

    def report(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def calls(self):
        return self.log.read_text().split() if self.log.exists() else []

    def test_parallel_pages_rank_all_rows_without_fetching_past_total(self):
        rows = [item(str(i), [source(1000000 + i, i + 1, 1920, 1080)], url=NOISE)
                for i in range(350)]
        config = {'libraries': {'movies': library(350, True, {
            str(offset): {'items': rows[offset:offset + 100], 'exhausted': offset + 100 >= 350}
            for offset in range(0, 350, 100)})}}
        result = self.run_tool(config, ['--instance', 'acct-1', '--library', 'movies', '--top', '3'])
        report = self.report(result)
        self.assertEqual([r['id'] for r in report['candidates']], ['349', '348', '347'])
        self.assertEqual(report['libraries'][0], {'library': 'movies', 'scanned': 350,
                         'total': 350, 'unknownBitrate': 0, 'complete': True})
        calls = self.log.read_text().splitlines()
        self.assertEqual(set(calls), {'movies 0', 'movies 100', 'movies 200', 'movies 300'})
        self.assertEqual(len(calls), 4)
        self.assertNotIn(NOISE, result.stdout + result.stderr)
        self.assertEqual(set(report['candidates'][0]),
                         {'id', 'title', 'library', 'bitrate', 'sizeBytes', 'width', 'height'})

    def test_short_provider_pages_do_not_skip_rows(self):
        config = {'libraries': {'movies': library(3, True, {
            str(i): {'items': [item(str(i), [source(i + 1, 100)])], 'exhausted': i == 2}
            for i in range(3)})}}
        report = self.report(self.run_tool(config, ['--instance', 'acct', '--library', 'movies']))
        self.assertTrue(report['libraries'][0]['complete'])
        self.assertEqual(report['libraries'][0]['scanned'], 3)
        self.assertEqual([r['id'] for r in report['candidates']], ['2', '1', '0'])

    def test_cap_unknown_bitrate_and_missing_rows_prevent_complete_claims(self):
        cases = [
            (library(2, True, {'0': {'items': [item('a', [source(100)])], 'exhausted': False}}),
             ['--max-pages', '1']),
            (library(1, False, {'0': {'items': [item('a', [])], 'exhausted': True}}), []),
            (library(2, True, {'0': {'items': [item('a', [source(100)])], 'exhausted': True}}), []),
            (library(200, False, {'0': {'items': [item('a', [source(100)])], 'exhausted': False}}), []),
        ]
        for data, arguments in cases:
            with self.subTest(arguments=arguments, total=data['total']):
                report = self.report(self.run_tool({'libraries': {'movies': data}},
                    ['--instance', 'acct', '--library', 'movies', *arguments]))
                self.assertFalse(report['libraries'][0]['complete'])

    def test_libraries_deduplication_and_strongest_source(self):
        config = {'libraries': {
            'movies': library(1, True, {'0': {'items': [item('a', [source(1), source(10, 20)])],
                                            'exhausted': True}}),
            'shows': library(1, True, {'0': {'items': [item('b', [source(5)])], 'exhausted': True}})}}
        report = self.report(self.run_tool(config,
            ['--instance', 'acct', '--library', 'movies', '--library', 'shows']))
        self.assertEqual([r['id'] for r in report['candidates']], ['a', 'b'])
        self.assertEqual(report['candidates'][0]['bitrate'], 10)
        self.assertTrue(all(r['complete'] for r in report['libraries']))
        duplicate = library(2, True, {
            '0': {'items': [item('a', [source(10)])], 'exhausted': False},
            '1': {'items': [item('a', [source(20)])], 'exhausted': True}})
        report = self.report(self.run_tool({'libraries': {'movies': duplicate}},
            ['--instance', 'acct', '--library', 'movies']))
        self.assertFalse(report['libraries'][0]['complete'])
        self.assertEqual(len(report['candidates']), 1)

    def test_read_failure_exits_nonzero_without_provider_detail(self):
        for failure in ('exit', 'json', 'ok'):
            with self.subTest(failure=failure):
                config = {'libraries': {'movies': library(1, True, {}, fail=failure)}}
                result = self.run_tool(config, ['--instance', 'acct', '--library', 'movies'])
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')
                self.assertEqual(result.stderr.strip(), 'provider read failed')
                self.assertNotIn(NOISE, result.stderr)
        result = subprocess.run([sys.executable, str(SCRIPT), '--spoolet', str(self.root / 'missing'),
                                 '--instance', 'acct', '--library', 'movies'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stderr.strip(), 'provider read failed')


if __name__ == '__main__':
    unittest.main()
