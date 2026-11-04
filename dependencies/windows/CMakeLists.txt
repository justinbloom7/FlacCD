cmake_minimum_required(VERSION 3.20)
project(FlacCDWindowsDependencies LANGUAGES NONE)
if(POLICY CMP0135)
   cmake_policy(SET CMP0135 NEW)
endif()
if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
   message(FATAL_ERROR "Configure with the LLVM-MinGW UCRT toolchain")
endif()
if(NOT FLACCD_WINDOWS_DEPENDENCY_PREFIX)
   message(FATAL_ERROR "Set FLACCD_WINDOWS_DEPENDENCY_PREFIX to an isolated absolute prefix")
endif()
include(ExternalProject)
file(SHA256 "${FLACCD_LLVM_MINGW_ROOT}/bin/clang" compilerHash)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/FlacCDToolchain.txt" "${FLACCD_LLVM_MINGW_ROOT}\n${compilerHash}\n")
set(FLACCD_FLAC_SOURCE_ARCHIVE "" CACHE FILEPATH "Optional offline flac-1.4.3.tar.xz (hash verified)")
set(flacUrl "https://downloads.xiph.org/releases/flac/flac-1.4.3.tar.xz")
if(FLACCD_FLAC_SOURCE_ARCHIVE)
   set(flacUrl "${FLACCD_FLAC_SOURCE_ARCHIVE}")
endif()
ExternalProject_Add(WindowsFlac
   URL "${flacUrl}"
   URL_HASH SHA256=6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70
   DOWNLOAD_NO_PROGRESS TRUE
   TLS_VERIFY TRUE
   BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target FLAC
   CMAKE_ARGS
      "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DFLACCD_LLVM_MINGW_ROOT=${FLACCD_LLVM_MINGW_ROOT}"
      "-DFLACCD_WINDOWS_DEPENDENCY_PREFIX=${FLACCD_WINDOWS_DEPENDENCY_PREFIX}"
      "-DFLACCD_WINDOWS_OBJDUMP=${FLACCD_WINDOWS_OBJDUMP}"
      "-DCMAKE_INSTALL_PREFIX=${FLACCD_WINDOWS_DEPENDENCY_PREFIX}"
      -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF
      -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF
      -DINSTALL_MANPAGES=OFF -DWITH_OGG=OFF
      -DWITH_FORTIFY_SOURCE=OFF -DWITH_STACK_PROTECTOR=OFF)
# FLAC 1.4.3's MinGW stack-protector check assumes GCC's libssp. LLVM's
# compiler-rt runtime is different; do not import GCC archives to satisfy it.
ExternalProject_Add_Step(WindowsFlac licenses
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLACCD_WINDOWS_DEPENDENCY_PREFIX}/share/licenses/flac"
   COMMAND "${CMAKE_COMMAND}" -E copy <SOURCE_DIR>/COPYING.Xiph
      "${FLACCD_WINDOWS_DEPENDENCY_PREFIX}/share/licenses/flac/COPYING.Xiph"
   COMMAND "${CMAKE_COMMAND}" -E make_directory "${FLACCD_WINDOWS_DEPENDENCY_PREFIX}/share/licenses/llvm-mingw"
   COMMAND "${CMAKE_COMMAND}" -E copy "${FLACCD_LLVM_MINGW_ROOT}/LICENSE.TXT"
      "${FLACCD_WINDOWS_DEPENDENCY_PREFIX}/share/licenses/llvm-mingw/LICENSE.TXT"
   COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/FlacCDToolchain.txt"
      "${FLACCD_WINDOWS_DEPENDENCY_PREFIX}/FlacCDToolchain.txt"
   DEPENDEES install)
