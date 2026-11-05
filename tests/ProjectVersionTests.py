"""Exercise release tags in an isolated repository, never the user's tags."""
import pathlib
import shutil
import subprocess
import sys
import tempfile

sourceRoot = pathlib.Path(sys.argv[1])
cmake = sys.argv[2]
git = shutil.which('git')
if not git:
   raise RuntimeError('Git is required for the release-version regression test')
with tempfile.TemporaryDirectory(prefix='FlacCD-version-') as temporary:
   root = pathlib.Path(temporary)
   repository = root / 'Repository'
   repository.mkdir()
   checkScript = root / 'Check.cmake'
   checkScript.write_text('''cmake_minimum_required(VERSION 3.20)
include("${sourceRoot}/cmake/ProjectVersion.cmake")
resolveFlaccdVersion("${repository}" version)
file(WRITE "${resultFile}" "${version}")
''')

   def command(*arguments):
      subprocess.run([git, '-C', str(repository), *arguments], check=True, capture_output=True)

   def check(expected, folder=None, disableGit=False):
      resultFile = root / 'Version.txt'
      result = subprocess.run([cmake, '-DsourceRoot=' + str(sourceRoot),
                               '-Drepository=' + str(folder or repository),
                               '-DresultFile=' + str(resultFile),
                               '-DCMAKE_DISABLE_FIND_PACKAGE_Git=' + ('TRUE' if disableGit else 'FALSE'),
                               '-P', str(checkScript)], capture_output=True, text=True)
      if expected == 'ambiguous':
         assert result.returncode and 'Multiple release tags' in result.stderr, result.stderr
      else:
         assert result.returncode == 0, result.stderr
         assert resultFile.read_text() == expected, resultFile.read_text()

   command('init')
   command('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
           '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', 'fixture')
   check('0.1.1')
   command('tag', 'v0.2.3')
   check('0.2.3')
   check('0.1.1', disableGit=True)
   unpacked = repository / 'UnpackedSource'
   unpacked.mkdir()
   check('0.1.1', folder=unpacked)
   command('tag', 'v0.2.4')
   check('ambiguous')
   command('tag', '-d', 'v0.2.4')
   command('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
           '-c', 'commit.gpgsign=false', 'commit', '--allow-empty', '-m', 'after tag')
   check('0.1.1')
print('Exact release tags, ambiguity, untagged builds and source archives passed')
