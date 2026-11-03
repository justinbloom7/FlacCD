cmake_minimum_required(VERSION 3.20)

function(run)
   execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE problem)
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "Static FLAC fixture failed: ${output} ${problem}")
   endif()
endfunction()

set(root "${testDirectory}/static-flac")
set(prefix "${root}/prefix")
file(MAKE_DIRECTORY "${prefix}/include/FLAC" "${prefix}/lib" "${root}/archives" "${root}/consumer")
file(WRITE "${prefix}/include/FLAC/Fixture.h" [=[
#pragma once
#ifndef FLAC__NO_DLL
#error Static FLAC must disable DLL import declarations
#endif
extern "C" {
struct FLAC__StreamDecoder;
struct FLAC__StreamEncoder;
struct FLAC__StreamMetadata;
enum FLAC__MetadataType { FLAC__METADATA_TYPE_VORBIS_COMMENT = 4 };
int FLAC__stream_decoder_process_single();
int FLAC__stream_decoder_init_file();
int FLAC__stream_encoder_process_interleaved();
int FLAC__metadata_chain_read_with_callbacks();
FLAC__StreamDecoder* FLAC__stream_decoder_new();
FLAC__StreamEncoder* FLAC__stream_encoder_new();
FLAC__StreamMetadata* FLAC__metadata_object_new(FLAC__MetadataType type);
void FLAC__stream_decoder_delete(FLAC__StreamDecoder* decoder);
void FLAC__stream_encoder_delete(FLAC__StreamEncoder* encoder);
void FLAC__metadata_object_delete(FLAC__StreamMetadata* metadata);
}
]=])
foreach(header stream_decoder stream_encoder metadata)
   file(WRITE "${prefix}/include/FLAC/${header}.h" "#include \"Fixture.h\"\n")
endforeach()
file(WRITE "${root}/archives/FakeOgg.cpp" "extern \"C\" int fixtureOgg() { return 0; }\n")
file(WRITE "${root}/archives/FakeFlac.cpp" [=[
#include <FLAC/Fixture.h>
extern "C" int fixtureOgg();
extern "C" {
int FLAC__stream_decoder_process_single() { return fixtureOgg(); }
int FLAC__stream_decoder_init_file() { return 0; }
int FLAC__stream_encoder_process_interleaved() { return 0; }
int FLAC__metadata_chain_read_with_callbacks() { return 0; }
FLAC__StreamDecoder* FLAC__stream_decoder_new() { fixtureOgg(); return nullptr; }
FLAC__StreamEncoder* FLAC__stream_encoder_new() { return nullptr; }
FLAC__StreamMetadata* FLAC__metadata_object_new(FLAC__MetadataType) { return nullptr; }
void FLAC__stream_decoder_delete(FLAC__StreamDecoder*) {}
void FLAC__stream_encoder_delete(FLAC__StreamEncoder*) {}
void FLAC__metadata_object_delete(FLAC__StreamMetadata*) {}
}
]=])
set(archiveProject [=[
cmake_minimum_required(VERSION 3.20)
project(StaticFlacArchiveFixture LANGUAGES CXX)
add_library(FakeOgg STATIC FakeOgg.cpp)
add_library(FakeFlac STATIC FakeFlac.cpp)
target_compile_definitions(FakeFlac PRIVATE FLAC__NO_DLL)
target_include_directories(FakeFlac PRIVATE [==[@prefix@/include]==])
set_target_properties(FakeOgg PROPERTIES OUTPUT_NAME ogg ARCHIVE_OUTPUT_DIRECTORY [==[@prefix@/lib]==])
set_target_properties(FakeFlac PROPERTIES OUTPUT_NAME FLAC ARCHIVE_OUTPUT_DIRECTORY [==[@prefix@/lib]==])
]=])
string(CONFIGURE "${archiveProject}" archiveProject @ONLY)
file(WRITE "${root}/archives/CMakeLists.txt" "${archiveProject}")
run("${CMAKE_COMMAND}" -S "${root}/archives" -B "${root}/archive-build" -G "${fixtureGenerator}"
    "-DCMAKE_CXX_COMPILER=${compilerPath}" -DCMAKE_BUILD_TYPE=Release)
run("${CMAKE_COMMAND}" --build "${root}/archive-build" --config Release)
# Multi-config generators add configuration subdirectories unless overridden.
file(GLOB_RECURSE builtFlac "${prefix}/lib/libFLAC.a")
file(GLOB_RECURSE builtOgg "${prefix}/lib/libogg.a")
list(GET builtFlac 0 flacArchive)
list(GET builtOgg 0 oggArchive)
get_filename_component(libraryDirectory "${flacArchive}" DIRECTORY)
set(consumerProject [=[
cmake_minimum_required(VERSION 3.20)
project(StaticFlacConsumerFixture LANGUAGES CXX)
# Exercise the MinGW selection policy with native archives on Linux too; only
# real Windows targets require the PE-specific probe in addition to linking.
set(MINGW TRUE)
set(FLAC_INCLUDE_DIRS [==[@prefix@/include]==])
set(FLAC_LIBRARY_DIRS [==[@libraryDirectory@]==])
set(FLAC_STATIC_LIBRARY_DIRS [==[@libraryDirectory@]==])
set(FLAC_STATIC_LIBRARIES FLAC ogg)
add_library(PkgConfig::FLAC INTERFACE IMPORTED)
include([==[@sourceRoot@/cmake/FlacDependency.cmake]==])
configureFlacDependency()
file(WRITE "${CMAKE_BINARY_DIR}/Selection.txt" "${flacLinkage}\n${staticFlacArchive}")
if(flacLinkage STREQUAL "static")
   add_executable(Consumer "${CMAKE_BINARY_DIR}/CMakeFiles/FlacCDFlacProbe.cpp")
   target_link_libraries(Consumer PRIVATE flaccd::flac)
endif()
]=])
string(CONFIGURE "${consumerProject}" consumerProject @ONLY)
file(WRITE "${root}/consumer/CMakeLists.txt" "${consumerProject}")

function(configureCase name expected)
   execute_process(COMMAND "${CMAKE_COMMAND}" -S "${root}/consumer" -B "${root}/${name}" -G "${fixtureGenerator}"
                           "-DCMAKE_CXX_COMPILER=${compilerPath}" -DCMAKE_BUILD_TYPE=Release ${ARGN}
                   RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE problem)
   if(expected STREQUAL "reject")
      if(result EQUAL 0 OR NOT "${output}${problem}" MATCHES "not an import library")
         message(FATAL_ERROR "DLL import archive accepted as a static implementation: ${output} ${problem}")
      endif()
      return()
   endif()
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "Static selection configure failed: ${output} ${problem}")
   endif()
   file(READ "${root}/${name}/Selection.txt" selection)
   if(NOT selection MATCHES "^${expected}")
      message(FATAL_ERROR "Expected ${expected}, got ${selection}: ${output} ${problem}")
   endif()
endfunction()

configureCase(valid static)
run("${CMAKE_COMMAND}" --build "${root}/valid" --config Release)
configureCase(disabled shared -DFLACCD_PREFER_STATIC_FLAC=OFF)
file(RENAME "${flacArchive}" "${flacArchive}.saved")
configureCase(unavailable shared)
file(RENAME "${flacArchive}.saved" "${flacArchive}")
file(RENAME "${oggArchive}" "${oggArchive}.saved")
configureCase(missing-private shared)
file(RENAME "${oggArchive}.saved" "${oggArchive}")
file(WRITE "${libraryDirectory}/libFLAC.dll.a" "DLL import library must not be selected")
configureCase(import-override reject "-DFLACCD_STATIC_FLAC_ARCHIVE=${libraryDirectory}/libFLAC.dll.a")
# A file named .a still has to link; renaming a bogus/import file proves nothing.
file(RENAME "${flacArchive}" "${flacArchive}.saved")
file(WRITE "${flacArchive}" "invalid archive")
configureCase(incompatible shared)
file(REMOVE "${flacArchive}")
file(RENAME "${flacArchive}.saved" "${flacArchive}")
message(STATUS "Static FLAC selection/private dependency/NO_DLL and shared fallback tests passed")
