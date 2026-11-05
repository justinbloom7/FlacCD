function(configureWindowsInstaller)
   if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
      return()
   endif()
   if(NOT CMAKE_HOST_WIN32 AND NOT (CMAKE_CROSSCOMPILING AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"))
      return()
   endif()
   find_program(FLACCD_ISCC NAMES ISCC.exe ISCC
      HINTS "$ENV{ProgramFiles}/Inno Setup 7" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 7"
            "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
            "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
      DOC "Inno Setup 6.3+ compiler (ISCC.exe)")
   set(installerDependency flaccd-smoke)
   if(NOT CMAKE_HOST_WIN32)
      find_program(FLACCD_WINE_EXECUTABLE NAMES wine wine64 DOC "Linux Wine loader")
      find_program(FLACCD_WINEPATH_EXECUTABLE NAMES winepath DOC "Wine path translator")
      set(FLACCD_WINE_PREFIX "" CACHE PATH "Initialized win64 Wine prefix containing Inno Setup")
      set(installerDependency flaccd-deploy)
   endif()
   add_custom_target(flaccd-installer
      COMMAND "${CMAKE_COMMAND}"
         "-DconfigFile=${CMAKE_BINARY_DIR}/WindowsDeploy-$<CONFIG>.cmake"
         "-Discc=${FLACCD_ISCC}"
         "-DwineExecutable=${FLACCD_WINE_EXECUTABLE}"
         "-DwinepathExecutable=${FLACCD_WINEPATH_EXECUTABLE}"
         "-DwinePrefix=${FLACCD_WINE_PREFIX}"
         "-DappVersion=${PROJECT_VERSION}"
         "-DinstallerOutput=${CMAKE_BINARY_DIR}/installer/$<CONFIG>"
         -P "${PROJECT_SOURCE_DIR}/cmake/BuildWindowsInstaller.cmake"
      DEPENDS ${installerDependency}
      COMMENT "Build per-user Inno Setup installer from audited Windows deployment"
      VERBATIM)
   if(BUILD_TESTING AND FLACCD_ISCC AND CMAKE_HOST_WIN32)
      add_test(NAME windows-installer COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
         --config "$<CONFIG>" --target flaccd-installer)
      set_tests_properties(windows-installer PROPERTIES TIMEOUT 720 RUN_SERIAL TRUE)
   endif()
endfunction()
