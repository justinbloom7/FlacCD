"""Check wrapper arguments and setup ownership without network or Wine."""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

sourceRoot = pathlib.Path(sys.argv[1])
testDirectory = pathlib.Path(sys.argv[2])
testDirectory.mkdir(parents=True, exist_ok=True)
temporary = tempfile.TemporaryDirectory(prefix='Run-', dir=testDirectory)
root = pathlib.Path(temporary.name)
cmake = sys.argv[3]
root.mkdir(parents=True, exist_ok=True)

def run(arguments, expected=None, **options):
   result = subprocess.run(arguments, capture_output=True, text=True, **options)
   if expected is None:
      if result.returncode:
         raise RuntimeError(result.stdout + result.stderr)
   elif result.returncode == 0 or expected not in result.stdout + result.stderr:
      raise RuntimeError('Expected ' + expected + ': ' + result.stdout + result.stderr)
   return result

for name in ('SetupWindowsBuild.sh', 'BuildWindows.sh'):
   script = str(sourceRoot / 'scripts' / name)
   run(['bash', '-n', script])
   run(['bash', script, '--help'])
   run(['bash', script, '--unknown'], 'Unknown option')
   run(['bash', script, '--tools-dir'], 'Missing value')
run(['bash', str(sourceRoot / 'scripts/BuildWindows.sh'), '--tools-dir', str(root / 'Missing')],
    'Build settings missing')

fakeBin = root / 'Fake bin'
fakeBin.mkdir(exist_ok=True)
record = root / 'Arguments.json'
fake = fakeBin / 'cmake'
fake.write_text('#!' + sys.executable + '\nimport json, os, sys\n'
                'open(os.environ["FLACCD_TEST_ARGUMENTS"], "w").write(json.dumps(sys.argv[1:]))\n')
fake.chmod(0o700)
xvfb = fakeBin / 'xvfb-run'
xvfb.write_text('#!/bin/sh\n[ "$1" = -a ] || exit 2\nshift\nexec "$@"\n')
xvfb.chmod(0o700)
tools = root / "Prepared 音楽 & ' tools"
tools.mkdir(exist_ok=True)
(tools / 'WindowsBuildSettings.cmake').write_text('# FlacCD Windows build settings v1\n')
environment = dict(os.environ, PATH=str(fakeBin) + os.pathsep + os.environ['PATH'],
                   FLACCD_TEST_ARGUMENTS=str(record), DISPLAY='')
run(['bash', str(sourceRoot / 'scripts/BuildWindows.sh'), '--tools-dir', str(tools),
     '--release-dir', str(root / 'Release with spaces'), '--', '-DFLACCD_FLAC_SOURCE_ARCHIVE=/cache/album & source.tar'],
    env=environment, cwd='/tmp')
arguments = json.loads(record.read_text())
assert '-DFLACCD_BUILD_SETTINGS=' + str(tools / 'WindowsBuildSettings.cmake') in arguments
assert '-DFLACCD_RELEASE_ROOT=' + str(root / 'Release with spaces') in arguments
assert '-DFLACCD_FLAC_SOURCE_ARCHIVE=/cache/album & source.tar' in arguments
assert arguments[-1] == str(sourceRoot / 'cmake/BuildWindowsRelease.cmake')
run(['bash', str(sourceRoot / 'scripts/SetupWindowsBuild.sh'), '--tools-dir', str(tools),
     '--archive-dir', str(root / 'Archive cache'), '--headless'], env=environment, cwd='/tmp')
arguments = json.loads(record.read_text())
assert '-DFLACCD_TOOLS_ROOT=' + str(tools) in arguments
assert '-DFLACCD_ARCHIVE_DIRECTORY=' + str(root / 'Archive cache') in arguments

inputRoot = root / 'Input'
(inputRoot / 'Fixture/bin').mkdir(parents=True, exist_ok=True)
(inputRoot / 'Fixture/bin/compiler').write_text('fixture')
cache = root / 'Cache'
cache.mkdir(exist_ok=True)
archive = cache / 'Fixture.tar'
run([cmake, '-E', 'tar', 'cf', str(archive), '--format=gnutar', 'Fixture'], cwd=inputRoot)
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
originalArchive = root / 'Original.tar'
originalArchive.write_bytes(archive.read_bytes())
check = root / 'Prepare.cmake'
check.write_text('''cmake_minimum_required(VERSION 3.20)
include("${sourceRoot}/cmake/WindowsBuildSetup.cmake")
file(MAKE_DIRECTORY "${FLACCD_TOOLS_ROOT}" "${FLACCD_ARCHIVE_DIRECTORY}")
prepareWindowsBuildTool(Fixture Fixture.tar "${archiveUrl}" "${hash}" destination)
''')

def prepare(folder, expected=None):
   return run([cmake, '-DsourceRoot=' + str(sourceRoot), '-DFLACCD_TOOLS_ROOT=' + str(folder),
               '-DFLACCD_ARCHIVE_DIRECTORY=' + str(cache), '-Dhash=' + digest,
               '-DarchiveUrl=' + originalArchive.as_uri(), '-P', str(check)], expected)

managed = root / 'Managed'
prepare(managed)
archive.unlink()
prepare(managed)  # Reuse the owned extraction without downloads.
prepare(root / 'Downloaded')
assert hashlib.sha256(archive.read_bytes()).hexdigest() == digest
unowned = root / 'Unowned'
(unowned / 'Fixture').mkdir(parents=True, exist_ok=True)
(unowned / 'Fixture/keep.txt').write_text('keep')
prepare(unowned, 'not managed by setup')
assert (unowned / 'Fixture/keep.txt').read_text() == 'keep'
archive.write_bytes(b'corrupt archive')
prepare(root / 'Corrupt', 'checksum mismatch')
assert archive.read_bytes() == b'corrupt archive'
invalid = root / 'Invalid.cmake'
invalid.write_text('# Unrelated settings\nmessage(FATAL_ERROR "must not execute")\n')
run([cmake, '-DFLACCD_BUILD_SETTINGS=' + str(invalid), '-P', str(sourceRoot / 'cmake/BuildWindowsRelease.cmake')],
    'Invalid prepared build settings')
print('Build wrapper quoting, headless forwarding, setup cache and ownership checks passed')
