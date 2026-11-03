# Target-scoped MinGW runtime policy and PE deployment. No host PATH mutation.
function(configureWindowsDeployment)
   if(NOT WIN32)
      return()
   endif()

   set(endUserTargets flaccd-info flaccd-export flaccd-bin-cue flaccd flaccd-agent)
   option(FLACCD_MINGW_STATIC_RUNTIME "Statically link GCC and C++ runtimes into Windows applications" ON)
   option(FLACCD_MINGW_STATIC_WINPTHREAD "Prefer a link-tested static winpthreads archive" ON)
   set(FLACCD_WINDOWS_RUNTIME_DIRS "" CACHE STRING "Additional target DLL directories (semicolon-separated; never host PATH)")
   set(FLACCD_WINDOWS_LICENSE_DIRS "" CACHE STRING "Additional third-party license directories to include")
   set(runtimeDirs ${FLACCD_WINDOWS_RUNTIME_DIRS})
   get_filename_component(compilerDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)
   list(APPEND runtimeDirs "${compilerDirectory}")
   foreach(libraryDirectory IN LISTS FLAC_LIBRARY_DIRS FLAC_STATIC_LIBRARY_DIRS)
      get_filename_component(prefix "${libraryDirectory}" DIRECTORY)
      list(APPEND runtimeDirs "${libraryDirectory}" "${prefix}/bin")
   endforeach()

   set(FLACCD_WINDOWS_OBJDUMP "${CMAKE_OBJDUMP}" CACHE FILEPATH "GNU objdump capable of reading target PE files")
   if(NOT FLACCD_WINDOWS_OBJDUMP)
      find_program(peObjdump NAMES objdump HINTS "${compilerDirectory}" NO_CMAKE_FIND_ROOT_PATH)
      if(peObjdump)
         set(FLACCD_WINDOWS_OBJDUMP "${peObjdump}" CACHE FILEPATH "GNU PE objdump" FORCE)
      endif()
   endif()

   set(staticPthreadArchive "")
   if(MINGW)
      # Ask the selected target compiler, so a Linux cross build never scans
      # /usr/bin for host libraries or assumes an MSYS2 installation location.
      foreach(name libstdc++-6.dll libgcc_s_seh-1.dll libgcc_s_dw2-1.dll libgcc_s_sjlj-1.dll libwinpthread-1.dll)
         execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${name}"
                         OUTPUT_VARIABLE libraryPath OUTPUT_STRIP_TRAILING_WHITESPACE
                         RESULT_VARIABLE queryResult)
         if(queryResult EQUAL 0 AND IS_ABSOLUTE "${libraryPath}" AND EXISTS "${libraryPath}")
            get_filename_component(directory "${libraryPath}" DIRECTORY)
            list(APPEND runtimeDirs "${directory}")
         endif()
      endforeach()
      if(FLACCD_MINGW_STATIC_RUNTIME)
         foreach(target IN LISTS endUserTargets)
            target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
         endforeach()
      endif()
      if(FLACCD_MINGW_STATIC_WINPTHREAD AND FLACCD_MINGW_STATIC_RUNTIME)
         execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=libwinpthread.a
                         OUTPUT_VARIABLE archive OUTPUT_STRIP_TRAILING_WHITESPACE
                         RESULT_VARIABLE queryResult)
         if(queryResult EQUAL 0 AND IS_ABSOLUTE "${archive}" AND EXISTS "${archive}")
            # Keep the probe private. Whole-archive avoids order-dependent
            # omissions when static libstdc++ adds late pthread references.
            set(probeSource "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/FlacCDThreadProbe.cpp")
            set(probeBinary "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/FlacCDThreadProbe.exe")
            file(WRITE "${probeSource}" "#include <thread>\n#include <mutex>\nint main() { std::mutex m; std::thread t([&] { std::lock_guard<std::mutex> g(m); }); t.join(); }\n")
            set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
            unset(FLACCD_STATIC_WINPTHREAD_LINKS CACHE)
            try_compile(FLACCD_STATIC_WINPTHREAD_LINKS "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/FlacCDThreadProbe"
                        "${probeSource}"
                        CMAKE_FLAGS -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
                        LINK_OPTIONS -static-libgcc -static-libstdc++
                        LINK_LIBRARIES -Wl,--whole-archive "${archive}" -Wl,--no-whole-archive
                        COPY_FILE "${probeBinary}"
                        OUTPUT_VARIABLE probeLog)
            set(staticPthreadVerified FALSE)
            if(FLACCD_STATIC_WINPTHREAD_LINKS AND FLACCD_WINDOWS_OBJDUMP)
               execute_process(COMMAND "${FLACCD_WINDOWS_OBJDUMP}" -p "${probeBinary}"
                               RESULT_VARIABLE inspected OUTPUT_VARIABLE probeImports ERROR_VARIABLE probeError)
               string(TOLOWER "${probeImports}" probeImports)
               if(inspected EQUAL 0 AND probeImports MATCHES "file format pei-" AND
                  NOT probeImports MATCHES "dll name:[ \t]*(libwinpthread-1|libstdc\\+\\+-6|libgcc_s_[a-z0-9_-]+)\\.dll")
                  set(staticPthreadVerified TRUE)
               endif()
            endif()
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/FlacCDThreadProbe.log"
                       "${probeLog}\n${probeImports}\n${probeError}")
            if(staticPthreadVerified)
               set(staticPthreadArchive "${archive}")
               foreach(target IN LISTS endUserTargets)
                  target_link_libraries(${target} PRIVATE -Wl,--whole-archive "${archive}" -Wl,--no-whole-archive)
               endforeach()
            endif()
         endif()
         if(NOT staticPthreadArchive)
            message(STATUS "Static winpthreads unavailable/not verified by PE probe; deployment will bundle it if imported")
         endif()
      endif()
   endif()

   list(REMOVE_DUPLICATES runtimeDirs)
   set(licenseDirs ${FLACCD_WINDOWS_LICENSE_DIRS})
   foreach(directory IN LISTS runtimeDirs)
      get_filename_component(prefix "${directory}" DIRECTORY)
      foreach(package flac libogg gcc-libs cc-libs gcc libgcc libstdc++ libwinpthread winpthreads mingw-w64-winpthreads)
         if(IS_DIRECTORY "${prefix}/share/licenses/${package}")
            list(APPEND licenseDirs "${prefix}/share/licenses/${package}")
         endif()
      endforeach()
   endforeach()
   list(REMOVE_DUPLICATES licenseDirs)

   set(executablePaths "")
   foreach(target IN LISTS endUserTargets)
      list(APPEND executablePaths "$<TARGET_FILE:${target}>")
      list(APPEND runtimeDirs "$<TARGET_FILE_DIR:${target}>")
   endforeach()
   list(REMOVE_DUPLICATES runtimeDirs)
   set(expectStaticRuntime FALSE)
   set(expectStaticPthread FALSE)
   if(MINGW AND FLACCD_MINGW_STATIC_RUNTIME)
      set(expectStaticRuntime TRUE)
   endif()
   if(staticPthreadArchive)
      set(expectStaticPthread TRUE)
   endif()
   set(expectStaticFlac FALSE)
   if(flacLinkage STREQUAL "static")
      set(expectStaticFlac TRUE)
   endif()
   set(configuration "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Default>")
   set(deployBase "${CMAKE_BINARY_DIR}/deploy/${configuration}")
   math(EXPR architectureBits "${CMAKE_SIZEOF_VOID_P} * 8")
   set(archiveName "FlacCD-${PROJECT_VERSION}-windows-${architectureBits}bit.zip")
   configure_file("${PROJECT_SOURCE_DIR}/cmake/WindowsDeployConfig.cmake.in"
                  "${CMAKE_CURRENT_BINARY_DIR}/WindowsDeployConfig.cmake.in" @ONLY)
   file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WindowsDeploy-$<CONFIG>.cmake"
                 INPUT "${CMAKE_CURRENT_BINARY_DIR}/WindowsDeployConfig.cmake.in")
   set(configFile "${CMAKE_CURRENT_BINARY_DIR}/WindowsDeploy-$<CONFIG>.cmake")
   add_custom_target(flaccd-deploy
      COMMAND "${CMAKE_COMMAND}" "-DconfigFile=${configFile}" -P "${PROJECT_SOURCE_DIR}/cmake/DeployWindows.cmake"
      DEPENDS ${endUserTargets}
      COMMENT "Stage Windows applications and recursively audit/bundle PE runtime dependencies"
      VERBATIM)
   add_custom_target(flaccd-audit
      COMMAND "${CMAKE_COMMAND}" "-DconfigFile=${configFile}" -DdeployMode=verify
              -P "${PROJECT_SOURCE_DIR}/cmake/DeployWindows.cmake"
      COMMENT "Audit existing deployment using only its adjacent DLLs, never developer PATH"
      VERBATIM)
   add_custom_target(flaccd-smoke
      COMMAND "${CMAKE_COMMAND}" "-DconfigFile=${configFile}"
              -P "${PROJECT_SOURCE_DIR}/cmake/CheckWindowsDeployment.cmake"
      DEPENDS flaccd-deploy
      COMMENT "Verify packaged startup in normal CMD and PowerShell with a clean process PATH"
      VERBATIM)
   add_custom_target(flaccd-package
      COMMAND "${CMAKE_COMMAND}" "-DconfigFile=${configFile}" -DdeployMode=package
              -P "${PROJECT_SOURCE_DIR}/cmake/DeployWindows.cmake"
      DEPENDS flaccd-deploy
      COMMENT "Create audited Windows ZIP package"
      VERBATIM)
endfunction()
