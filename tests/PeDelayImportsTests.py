"""Synthetic PE parsing tests; no target code is executed."""
import pathlib
import struct
import subprocess
import sys

sourceRoot = pathlib.Path(sys.argv[1])
testRoot = pathlib.Path(sys.argv[2])
cmake = sys.argv[3]
testRoot.mkdir(parents=True, exist_ok=True)
checkScript = testRoot / 'Check.cmake'
checkScript.write_text('''cmake_minimum_required(VERSION 3.20)
include("${sourceRoot}/cmake/PeDependencies.cmake")
include("${sourceRoot}/cmake/PeDelayImports.cmake")
readPeDelayImports("${filePath}" imports)
if(NOT imports STREQUAL "kernel32.dll")
   message(FATAL_ERROR "Unexpected imports: ${imports}")
endif()
''')

def fixture(bits=64):
   data = bytearray(0x400)
   data[:2] = b'MZ'
   struct.pack_into('<I', data, 60, 0x80)
   data[0x80:0x84] = b'PE\0\0'
   struct.pack_into('<H', data, 0x86, 1)
   optional = 0x98
   size = 240 if bits == 64 else 224
   struct.pack_into('<H', data, 0x94, size)
   struct.pack_into('<H', data, optional, 523 if bits == 64 else 267)
   struct.pack_into('<I', data, optional + 60, 0x200)
   directory = optional + (112 if bits == 64 else 96)
   struct.pack_into('<I', data, directory - 4, 16)
   struct.pack_into('<II', data, directory + 13 * 8, 0x1000, 64)
   section = optional + size
   struct.pack_into('<IIII', data, section + 8, 0x200, 0x1000, 0x200, 0x200)
   struct.pack_into('<II', data, 0x200, 1, 0x1040)
   data[0x240:0x24d] = b'KERNEL32.dll\0'
   return data

def check(name, data, expected=None):
   path = testRoot / (name + '.exe')
   path.write_bytes(data)
   result = subprocess.run([cmake, '-DsourceRoot=' + str(sourceRoot),
                            '-DfilePath=' + str(path), '-P', str(checkScript)],
                           capture_output=True, text=True)
   if expected is None:
      if result.returncode:
         raise RuntimeError(name + ': ' + result.stdout + result.stderr)
   elif result.returncode == 0 or expected not in result.stdout + result.stderr:
      raise RuntimeError(name + ': expected ' + expected + ': ' + result.stdout + result.stderr)

check('Pe64', fixture())
check('Pe32', fixture(32))
data = fixture(32)
struct.pack_into('<I', data, 0x98 + 28, 0x400000)
struct.pack_into('<II', data, 0x200, 0, 0x401040)
check('LegacyVa', data)
check('Truncated', fixture()[:100], 'Truncated PE')
data = fixture()
struct.pack_into('<I', data, 0x200, 2)
check('Attributes', data, 'Unsupported PE delay-import attributes')
data = fixture()
struct.pack_into('<I', data, 0x204, 0x5000)
check('NameRva', data, 'Unmapped PE delay-import RVA')
data = fixture()
data[0x220:0x240] = data[0x200:0x220]
check('UnterminatedTable', data, 'Unterminated PE delay-import descriptor table')
data = fixture()
data[0x240:0x340] = b'a' * 256
check('UnterminatedName', data, 'unterminated PE delay-import DLL name')
data = fixture()
data[0x240] = 255
check('InvalidName', data, 'Invalid PE delay-import DLL name byte')
data = fixture()
struct.pack_into('<II', data, 0x98 + 112 + 13 * 8, 0x11f0, 64)
check('SectionBounds', data, 'crosses section bounds')
print('PE32/PE64 delayed imports, legacy VAs and malformed bounds passed')
