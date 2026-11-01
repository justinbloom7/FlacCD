# All generated output belongs to the CTest fixture directory.
set(inputPath "${fixtureDirectory}/fixture-4096.flac")
set(originalPath "${fixtureDirectory}/original.raw")
set(nativePath "${fixtureDirectory}/export-native.raw")
set(textPath "${fixtureDirectory}/export-text.raw")
set(rangePath "${fixtureDirectory}/export-range.raw")
set(invalidPath "${fixtureDirectory}/export-invalid.raw")
file(REMOVE "${nativePath}" "${textPath}" "${rangePath}" "${invalidPath}")

foreach(source IN ITEMS native text)
   execute_process(COMMAND "${exportTool}" "${inputPath}" "${source}" -2 240 "${fixtureDirectory}/export-${source}.raw"
      RESULT_VARIABLE exportStatus OUTPUT_VARIABLE exportOutput ERROR_VARIABLE exportError)
   if(NOT exportStatus EQUAL 0)
      message(FATAL_ERROR "${source} export failed: ${exportError}")
   endif()
   execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${originalPath}" "${fixtureDirectory}/export-${source}.raw"
      RESULT_VARIABLE compareStatus)
   if(NOT compareStatus EQUAL 0)
      message(FATAL_ERROR "${source} full export differs from original signed little-endian PCM")
   endif()
endforeach()

# Three-sector ranges straddle both INDEX 00 and INDEX 01 of track 2.
execute_process(COMMAND "${exportTool}" "${inputPath}" native 22 7 "${rangePath}"
   RESULT_VARIABLE exportStatus ERROR_VARIABLE exportError OUTPUT_QUIET)
if(NOT exportStatus EQUAL 0)
   message(FATAL_ERROR "Range export failed: ${exportError}")
endif()
math(EXPR referenceOffset "24 * 2352")
math(EXPR rangeLength "7 * 2352")
file(READ "${originalPath}" expectedRange OFFSET ${referenceOffset} LIMIT ${rangeLength} HEX)
file(READ "${rangePath}" actualRange HEX)
if(NOT expectedRange STREQUAL actualRange)
   message(FATAL_ERROR "Range export differs from original PCM at INDEX 00/01")
endif()

# Existing output and input audio must never be truncated or overwritten.
file(SHA256 "${inputPath}" inputHash)
file(SHA256 "${nativePath}" outputHash)
foreach(destination IN ITEMS "${inputPath}" "${nativePath}")
   execute_process(COMMAND "${exportTool}" "${inputPath}" native 0 1 "${destination}"
      RESULT_VARIABLE overwriteStatus OUTPUT_QUIET ERROR_QUIET)
   if(overwriteStatus EQUAL 0)
      message(FATAL_ERROR "Export overwrote an existing file")
   endif()
endforeach()
file(SHA256 "${inputPath}" inputHashAfter)
file(SHA256 "${nativePath}" outputHashAfter)
if(NOT inputHash STREQUAL inputHashAfter OR NOT outputHash STREQUAL outputHashAfter)
   message(FATAL_ERROR "Input or existing output changed")
endif()

foreach(start IN ITEMS -3 238 9223372036854775807)
   execute_process(COMMAND "${exportTool}" "${inputPath}" native "${start}" 1 "${invalidPath}"
      RESULT_VARIABLE invalidStatus OUTPUT_QUIET ERROR_QUIET)
   if(invalidStatus EQUAL 0 OR EXISTS "${invalidPath}")
      message(FATAL_ERROR "Invalid LBA was accepted or created output")
   endif()
endforeach()
execute_process(COMMAND "${exportTool}" "${inputPath}" native 237 2 "${invalidPath}"
   RESULT_VARIABLE invalidStatus OUTPUT_QUIET ERROR_QUIET)
if(invalidStatus EQUAL 0 OR EXISTS "${invalidPath}")
   message(FATAL_ERROR "Range past lead-out was accepted or created output")
endif()
message(STATUS "Full/range exports match original PCM; overwrite and invalid-range checks passed")
