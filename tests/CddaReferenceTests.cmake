set(inputPath "${fixtureDirectory}/fixture-4096.flac")
set(referencePath "${fixtureDirectory}/reference-flac.raw")
set(exportPath "${fixtureDirectory}/reference-export.raw")
file(REMOVE "${referencePath}" "${exportPath}")
execute_process(COMMAND "${flacTool}" --decode --silent --force-raw-format --endian=little --sign=signed
   "--output-name=${referencePath}" "${inputPath}"
   RESULT_VARIABLE decodeStatus ERROR_VARIABLE decodeError)
if(NOT decodeStatus EQUAL 0)
   message(FATAL_ERROR "Reference flac decoder failed: ${decodeError}")
endif()
execute_process(COMMAND "${exportTool}" "${inputPath}" native -2 240 "${exportPath}"
   RESULT_VARIABLE exportStatus ERROR_VARIABLE exportError OUTPUT_QUIET)
if(NOT exportStatus EQUAL 0)
   message(FATAL_ERROR "Reference comparison export failed: ${exportError}")
endif()
foreach(candidate IN ITEMS "${fixtureDirectory}/original.raw" "${exportPath}")
   execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${referencePath}" "${candidate}"
      RESULT_VARIABLE compareStatus)
   if(NOT compareStatus EQUAL 0)
      message(FATAL_ERROR "Reference decode differs from ${candidate}")
   endif()
endforeach()
message(STATUS "Export and original PCM both match the flac reference decoder byte-for-byte")
