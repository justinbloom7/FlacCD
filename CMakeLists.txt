cmake_minimum_required(VERSION 3.20)
project(FlacCD VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
   message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build -G Ninja")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac>=1.3.3)

add_library(flaccd-core src/FlacMetadataInspector.cpp src/CueTextMetadata.cpp src/TrackLayout.cpp src/CddaSectorReader.cpp src/BinCueExporter.cpp src/CueSheetWriter.cpp src/AudioExporter.cpp src/ExportOutput.cpp)
target_include_directories(flaccd-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(flaccd-core PUBLIC cxx_std_20)
set_target_properties(flaccd-core PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(flaccd-core PRIVATE PkgConfig::FLAC)

add_executable(flaccd-info apps/FlaccdInfo.cpp)
target_link_libraries(flaccd-info PRIVATE flaccd-core)
set_target_properties(flaccd-info PROPERTIES CXX_EXTENSIONS OFF)

add_executable(flaccd-export apps/FlaccdExport.cpp)
target_link_libraries(flaccd-export PRIVATE flaccd-core)
set_target_properties(flaccd-export PROPERTIES CXX_EXTENSIONS OFF)

add_executable(flaccd-bin-cue apps/FlaccdBinCue.cpp)
target_link_libraries(flaccd-bin-cue PRIVATE flaccd-core)
set_target_properties(flaccd-bin-cue PROPERTIES CXX_EXTENSIONS OFF)

include(CTest)

# Mount coordination is platform-neutral for mock tests, separate from core.
# The real process/device adapter and CLI are built only on Windows.
if(WIN32 OR BUILD_TESTING)
   add_library(flaccd-mount src/mount/ImgDriveBackend.cpp
                            src/mount/MountStateStore.cpp
                            src/mount/MountFileSystem.cpp
                            src/mount/ProcessRunner.cpp)
   target_link_libraries(flaccd-mount PUBLIC flaccd-core)
   set_target_properties(flaccd-mount PROPERTIES CXX_EXTENSIONS OFF)
endif()

if(WIN32)
   target_compile_definitions(flaccd-mount PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
   add_library(flaccd-imgdrive src/mount/WindowsImgDriveHost.cpp)
   target_link_libraries(flaccd-imgdrive PUBLIC flaccd-mount PRIVATE advapi32 shell32 ole32 user32 uuid)
   target_compile_definitions(flaccd-imgdrive PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
   set_target_properties(flaccd-imgdrive PROPERTIES CXX_EXTENSIONS OFF)
   add_executable(flaccd apps/Flaccd.cpp)
   target_link_libraries(flaccd PRIVATE flaccd-imgdrive)
   target_compile_definitions(flaccd PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
   set_target_properties(flaccd PROPERTIES CXX_EXTENSIONS OFF)
   if(MINGW)
      target_link_options(flaccd PRIVATE -municode)
   endif()
   if(MSVC)
      target_compile_options(flaccd-core PRIVATE /utf-8)
      target_compile_options(flaccd-mount PRIVATE /utf-8)
      target_compile_options(flaccd-imgdrive PRIVATE /utf-8)
      target_compile_options(flaccd PRIVATE /utf-8)
   endif()
endif()

if(BUILD_TESTING)
   add_executable(flaccd-process-diagnostic-tests tests/ProcessArgumentDiagnosticsTests.cpp)
   target_compile_features(flaccd-process-diagnostic-tests PRIVATE cxx_std_20)
   set_target_properties(flaccd-process-diagnostic-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME process-argument-diagnostics COMMAND flaccd-process-diagnostic-tests)
   if(WIN32)
      add_executable(flaccd-process-probe tests/WindowsProcessProbe.cpp)
      target_link_libraries(flaccd-process-probe PRIVATE flaccd-core)
      add_executable(flaccd-windows-process-tests tests/WindowsProcessTests.cpp)
      target_link_libraries(flaccd-windows-process-tests PRIVATE flaccd-imgdrive)
      foreach(target IN ITEMS flaccd-process-probe flaccd-windows-process-tests)
         target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
         set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
         if(MINGW)
            target_link_options(${target} PRIVATE -municode)
         endif()
         if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8)
         endif()
      endforeach()
      add_test(NAME windows-process COMMAND flaccd-windows-process-tests "$<TARGET_FILE:flaccd-process-probe>")
      add_test(NAME mount-cli-help COMMAND flaccd --help)
   endif()
   find_package(Threads REQUIRED)
   add_executable(flaccd-imgdrive-tests tests/ImgDriveBackendTests.cpp)
   target_link_libraries(flaccd-imgdrive-tests PRIVATE flaccd-mount Threads::Threads)
   set_target_properties(flaccd-imgdrive-tests PROPERTIES CXX_EXTENSIONS OFF)
   if(MSVC)
      target_compile_options(flaccd-imgdrive-tests PRIVATE /utf-8)
   endif()
   add_test(NAME imgdrive-backend COMMAND flaccd-imgdrive-tests "${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures")
   set_tests_properties(imgdrive-backend PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   add_executable(flaccd-audio-export-tests tests/AudioExporterTests.cpp)
   target_link_libraries(flaccd-audio-export-tests PRIVATE flaccd-core)
   set_target_properties(flaccd-audio-export-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME audio-export COMMAND flaccd-audio-export-tests "${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures")
   set_tests_properties(audio-export PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   add_executable(flaccd-bin-cue-tests tests/BinCueExporterTests.cpp)
   target_link_libraries(flaccd-bin-cue-tests PRIVATE flaccd-core)
   set_target_properties(flaccd-bin-cue-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME bin-cue COMMAND flaccd-bin-cue-tests "${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures")
   set_tests_properties(bin-cue PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   add_test(NAME bin-cue-help COMMAND flaccd-bin-cue --help)
   add_test(NAME bin-cue-cli COMMAND "${CMAKE_COMMAND}"
      "-DbinCueTool=$<TARGET_FILE:flaccd-bin-cue>"
      "-DfixtureDirectory=${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BinCueCliTests.cmake")
   set_tests_properties(bin-cue-cli PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   add_executable(flaccd-cdda-tests tests/CddaSectorReaderTests.cpp)
   target_link_libraries(flaccd-cdda-tests PRIVATE flaccd-core PkgConfig::FLAC)
   set_target_properties(flaccd-cdda-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME cdda-reader COMMAND flaccd-cdda-tests "${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures")
   set_tests_properties(cdda-reader PROPERTIES FIXTURES_SETUP cddaFixtures)
   add_test(NAME cdda-export COMMAND "${CMAKE_COMMAND}"
      "-DexportTool=$<TARGET_FILE:flaccd-export>"
      "-DfixtureDirectory=${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/CddaExportTests.cmake")
   set_tests_properties(cdda-export PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   find_program(FLAC_REFERENCE_TOOL NAMES flac)
   if(FLAC_REFERENCE_TOOL)
      add_test(NAME cdda-reference COMMAND "${CMAKE_COMMAND}"
         "-DexportTool=$<TARGET_FILE:flaccd-export>"
         "-DflacTool=${FLAC_REFERENCE_TOOL}"
         "-DfixtureDirectory=${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures"
         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/CddaReferenceTests.cmake")
      set_tests_properties(cdda-reference PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   else()
      message(STATUS "flac command not found; reference decoder comparison test is unavailable")
   endif()
   add_test(NAME modern-export COMMAND "${CMAKE_COMMAND}"
      "-DexportTool=$<TARGET_FILE:flaccd-export>"
      "-DflacTool=${FLAC_REFERENCE_TOOL}"
      "-DfixtureDirectory=${CMAKE_CURRENT_BINARY_DIR}/cdda-fixtures"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ModernExportTests.cmake")
   set_tests_properties(modern-export PROPERTIES FIXTURES_REQUIRED cddaFixtures)
   add_test(NAME export-help COMMAND flaccd-export --help)
   add_executable(flaccd-layout-tests tests/TrackLayoutTests.cpp)
   target_link_libraries(flaccd-layout-tests PRIVATE flaccd-core)
   set_target_properties(flaccd-layout-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME track-layout COMMAND flaccd-layout-tests)
   add_executable(flaccd-metadata-tests tests/FlacMetadataTests.cpp)
   target_link_libraries(flaccd-metadata-tests PRIVATE flaccd-core)
   set_target_properties(flaccd-metadata-tests PROPERTIES CXX_EXTENSIONS OFF)
   add_test(NAME metadata COMMAND flaccd-metadata-tests "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
   set_tests_properties(metadata PROPERTIES FIXTURES_SETUP metadataFixtures)
   add_test(NAME cli-help COMMAND flaccd-info --help)
   add_test(NAME cli-metadata COMMAND flaccd-info "${CMAKE_CURRENT_BINARY_DIR}/fixtures/cue.flac")
   set_tests_properties(cli-metadata PROPERTIES FIXTURES_REQUIRED metadataFixtures
      PASS_REGULAR_EXPRESSION "Track 1: offset 0 samples")
   add_test(NAME cli-descriptive COMMAND flaccd-info "${CMAKE_CURRENT_BINARY_DIR}/fixtures/cue.flac")
   set_tests_properties(cli-descriptive PROPERTIES FIXTURES_REQUIRED metadataFixtures
      PASS_REGULAR_EXPRESSION "ARTIST: Fixture Artist")
   add_test(NAME cli-layout COMMAND flaccd-info "${CMAKE_CURRENT_BINARY_DIR}/fixtures/cue.flac")
   set_tests_properties(cli-layout PROPERTIES FIXTURES_REQUIRED metadataFixtures
      PASS_REGULAR_EXPRESSION "Lead-out LBA: 74")
   add_test(NAME cli-layout-diagnostic COMMAND flaccd-info "${CMAKE_CURRENT_BINARY_DIR}/fixtures/cue.flac")
   set_tests_properties(cli-layout-diagnostic PROPERTIES FIXTURES_REQUIRED metadataFixtures
      PASS_REGULAR_EXPRESSION "REJECTED layout: missing FILE directive")
   add_test(NAME cli-invalid COMMAND flaccd-info "${CMAKE_CURRENT_BINARY_DIR}/fixtures/invalid.flac")
   set_tests_properties(cli-invalid PROPERTIES FIXTURES_REQUIRED metadataFixtures WILL_FAIL TRUE)
   add_test(NAME cli-no-arguments COMMAND flaccd-info)
   set_tests_properties(cli-no-arguments PROPERTIES WILL_FAIL TRUE)
endif()
