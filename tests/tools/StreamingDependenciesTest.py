#!/usr/bin/env python3
"""Exercise transport source preparation and CMake policy at their CLI boundary."""
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('openssl', 'nghttp2', 'nghttp3', 'ngtcp2', 'curl')


class StreamingDependenciesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for relative in ('tools/lib/manifest-sources.sh', 'tools/prepare-streaming-sources.sh',
                         'tools/streaming-fingerprint.py', 'tools/cmake/StreamingDependencies.cmake',
                         'tools/cmake/QuicBatchingProbe/CMakeLists.txt'):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        archive = self.root / 'fixture.tar.gz'
        with tarfile.open(archive, 'w:gz') as output:
            info = tarfile.TarInfo('fixture/content.txt')
            info.size = len(b'original\n')
            output.addfile(info, io.BytesIO(b'original\n'))
        self.manifest = self.root / 'tools/manifests/streaming-third-party.json'
        self.manifest.parent.mkdir(parents=True)
        self.data = {'schemaVersion': 1, 'sources': {name: {
            'version': '1', 'url': archive.as_uri(),
            'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()} for name in NAMES}}
        self.write_manifest()
        self.output = self.root / 'prepared'

    def write_manifest(self):
        self.manifest.write_text(json.dumps(self.data))

    def prepare(self, *extra):
        return subprocess.run(['bash', str(self.root / 'tools/prepare-streaming-sources.sh'),
                               '--root', str(self.root), '--output', str(self.output), *extra],
                              capture_output=True, text=True)

    def test_digest_rejection_precedes_source_use(self):
        self.data['sources']['openssl']['sha256'] = '0' * 64
        self.write_manifest()
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checksum mismatch', result.stderr)
        self.assertFalse(self.output.exists())

    def test_patch_update_reextracts_and_reuses_verified_archives(self):
        patch = self.root / 'change.patch'
        self.data['sources']['curl']['patches'] = ['change.patch']
        for replacement in ['first', 'second']:
            patch.write_text('--- a/content.txt\n+++ b/content.txt\n@@ -1 +1 @@\n-original\n+' + replacement + '\n')
            self.write_manifest()
            result = self.prepare()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((self.output / 'curl/content.txt').read_text(), replacement + '\n')
        # Verify cache identity does not depend on an available upstream URL.
        archive = self.root / 'fixture.tar.gz'
        archive.unlink()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_help_and_invalid_arguments_have_no_side_effects(self):
        for arguments, code in [(['--help'], 0), (['--root', 'relative', '--output', str(self.output)], 2),
                                (['--output'], 2), (['--invalid'], 2)]:
            result = subprocess.run(['bash', str(ROOT / 'tools/prepare-streaming-sources.sh'), *arguments],
                                    cwd=self.root, capture_output=True)
            self.assertEqual(result.returncode, code)
        self.assertFalse((self.root / 'build').exists())
        self.assertFalse(self.output.exists())

    def test_target_and_wrapper_content_invalidate_configuration(self):
        wrapper = self.root / 'wrapper.sh'
        wrapper.write_text('original')
        def identity(target):
            return subprocess.check_output(['python3', str(self.root / 'tools/streaming-fingerprint.py'),
                '--root', str(self.root), '--target', target, '--wrapper', str(wrapper)], text=True)
        initial = identity('arm:28:static')
        self.assertEqual(initial, identity('arm:28:static'))
        self.assertNotEqual(initial, identity('arm:29:static'))
        wrapper.write_text('changed')
        self.assertNotEqual(initial, identity('arm:28:static'))

    @unittest.skipUnless(shutil.which('ninja'), 'Ninja is required by the production dependency builder')
    def test_dependency_builder_stops_on_first_configure_failure_even_inside_conditional(self):
        sources = self.root / 'sources'
        for name in NAMES[1:]:
            source = sources / name
            source.mkdir(parents=True)
            source.joinpath('CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\n'
                'project(FailureFixture NONE)\n'
                + ('message(FATAL_ERROR "fixture configuration rejected")\n' if name == 'nghttp2'
                   else 'file(WRITE "${CMAKE_BINARY_DIR}/entered" "configured")\n'))
        builds = self.root / 'builds'
        # Conditional callers suppress bash errexit inside functions. The owner
        # must propagate the failing configure command itself, before siblings.
        result = subprocess.run(['bash', '-c',
            'set -e; ROOT="$1"; source "$ROOT/tools/lib/streaming-build.sh"; '
            'if build_streaming_http "$2" "$3" "$4" OFF; then exit 99; else exit "$?"; fi',
            'bash', str(ROOT), str(sources), str(builds), str(self.root / 'prefix')],
            capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotEqual(result.returncode, 99)
        self.assertIn('fixture configuration rejected', result.stdout + result.stderr)
        self.assertFalse((builds / 'nghttp3').exists())

    def test_batching_metadata_tracks_actual_target_compile_capability(self):
        source = self.root / 'curl'
        (source / 'lib').mkdir(parents=True)
        (source / 'include').mkdir()
        build = self.root / 'curl-build'
        (build / 'lib').mkdir(parents=True)
        metadata = self.root / 'libcurl.pc'
        metadata.write_text('Name: test-curl\nVersion: 1\n')
        setup = source / 'lib/curl_setup.h'
        for available in (True, False):
            setup.write_text('#include <sys/socket.h>\n#define HAVE_NETINET_UDP_H 1\n'
                             '#define USE_NGTCP2 1\n#define USE_NGHTTP3 1\n#define HAVE_SENDMSG 1\n'
                             + ('#define HAVE_SENDMMSG 1\n' if available else ''))
            configured = subprocess.run(['cmake', '-S', str(ROOT / 'tools/cmake/QuicBatchingProbe'),
                '-B', str(self.root / 'probe'), '-DSTREAMING_CURL_SOURCE=' + str(source),
                '-DSTREAMING_CURL_BUILD=' + str(build), '-DSTREAMING_INSTALLED_PC=' + str(metadata)],
                capture_output=True, text=True)
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            expected = '1' if available and sys.platform.startswith('linux') else '0'
            self.assertIn('spool_quic_batching=' + expected, metadata.read_text())
            self.assertEqual(metadata.read_text().count('spool_quic_batching='), 1)

    def test_cmake_policy_builds_selected_library_and_requires_protocols(self):
        source = self.root / 'project'
        source.mkdir()
        (source / 'sample.c').write_text('int sample(void) { return 42; }\n')
        # This project consumes the initializer just like each real dependency;
        # a missing protocol/adapter option fails configure at that boundary.
        (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(PolicyFixture C)
if(STREAMING_DEPENDENCY STREQUAL "curl")
  if(NOT USE_NGHTTP2 OR NOT USE_NGHTTP3 OR NOT USE_NGTCP2 OR CURL_DISABLE_ALTSVC)
    message(FATAL_ERROR "required transport capabilities disabled")
  endif()
elseif(STREAMING_DEPENDENCY STREQUAL "ngtcp2")
  if(NOT ENABLE_OPENSSL OR ENABLE_GNUTLS OR ENABLE_QUICTLS)
    message(FATAL_ERROR "incorrect TLS adapter")
  endif()
endif()
add_library(sample sample.c)
''')
        for dependency in NAMES[1:]:
            for shared in ('ON', 'OFF'):
                with self.subTest(dependency=dependency, shared=shared):
                    build = self.root / f'build-{dependency}-{shared}'
                    result = subprocess.run(['cmake', '-S', str(source), '-B', str(build),
                        '-DSTREAMING_DEPENDENCY=' + dependency, '-DSTREAMING_SHARED=' + shared,
                        '-C', str(ROOT / 'tools/cmake/StreamingDependencies.cmake')], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    subprocess.run(['cmake', '--build', str(build)], check=True, capture_output=True)
                    extension = ('.dylib' if sys.platform == 'darwin' else '.so') if shared == 'ON' else '.a'
                    self.assertTrue((build / ('libsample' + extension)).is_file())
        result = subprocess.run(['cmake', '-DSTREAMING_DEPENDENCY=bad', '-DSTREAMING_SHARED=ON',
            '-P', str(ROOT / 'tools/cmake/StreamingDependencies.cmake')], capture_output=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
