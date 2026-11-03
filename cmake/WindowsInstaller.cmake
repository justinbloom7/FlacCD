function(configureWindowsInstaller)
   if(NOT WIN32 OR NOT CMAKE_HOST_WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      return()
   endif()
   find_program(FLACCD_ISCC NAMES ISCC.exe ISCC
      HINTS "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
      DOC "Inno Setup 6.3+ compiler (ISCC.exe)")
   add_custom_target(flaccd-installer
      COMMAND "${CMAKE_COMMAND}"
         "-DconfigFile=${CMAKE_BINARY_DIR}/WindowsDeploy-$<CONFIG>.cmake"
         "-Discc=${FLACCD_ISCC}"
         "-DappVersion=${PROJECT_VERSION}"
         "-DinstallerOutput=${CMAKE_BINARY_DIR}/installer/$<CONFIG>"
         -P "${PROJECT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
      DEPENDS flaccd-smoke
      COMMENT "Build per-user Inno Setup installer from smoke-tested Windows deployment"
      VERBATIM)
   if(BUILD_TESTING AND FLACCD_ISCC)
      add_test(NAME windows-installer COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
         --config "$<CONFIG>" --target flaccd-installer)
      set_tests_properties(windows-installer PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
   endif()
endfunction()
