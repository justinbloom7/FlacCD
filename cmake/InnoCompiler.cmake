# Run the same compiler directly on Windows or through an explicit Wine prefix.
function(compileInnoInstaller manifest payload output script resultVariable logVariable)
   set(compiler "${iscc}")
   if(NOT CMAKE_HOST_WIN32)
      foreach(tool wineExecutable winepathExecutable)
         if(NOT EXISTS "${${tool}}")
            message(FATAL_ERROR "Set ${tool} to the installed Wine tool; see docs/LinuxWindowsRelease.md")
         endif()
      endforeach()
      if(NOT IS_ABSOLUTE "${winePrefix}" OR NOT EXISTS "${winePrefix}/system.reg")
         message(FATAL_ERROR "Set an initialized absolute winePrefix (WINEARCH=win64)")
      endif()
      file(STRINGS "${winePrefix}/system.reg" architecture REGEX "^#arch=win64$" LIMIT_COUNT 1)
      if(NOT architecture)
         message(FATAL_ERROR "The installer compiler requires a win64 Wine prefix")
      endif()
      file(LOCK "${winePrefix}/FlacCD-Iscc.lock" GUARD FUNCTION TIMEOUT 60)
      foreach(variable compiler manifest payload output script)
         execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=WINESERVER
                                 "WINEPREFIX=${winePrefix}" "WINELOADER=${wineExecutable}" WINEARCH=win64 WINEDEBUG=-all
                                 "${winepathExecutable}" -w "${${variable}}"
                         RESULT_VARIABLE converted OUTPUT_VARIABLE windowsPath ERROR_VARIABLE problem
                         OUTPUT_STRIP_TRAILING_WHITESPACE TIMEOUT 60)
         if(NOT converted STREQUAL "0" OR NOT windowsPath MATCHES "^([A-Za-z]:[/\\\\]|\\\\\\\\)" OR
            windowsPath MATCHES "[\r\n]")
            set(${resultVariable} "winepath failed" PARENT_SCOPE)
            set(${logVariable} "winepath failed for ${variable}: ${problem} ${windowsPath}" PARENT_SCOPE)
            return()
         endif()
         set(${variable} "${windowsPath}")
      endforeach()
      set(command "${CMAKE_COMMAND}" -E env --unset=WINESERVER
                  "WINEPREFIX=${winePrefix}" "WINELOADER=${wineExecutable}" WINEARCH=win64 WINEDEBUG=-all
                  "${wineExecutable}" "${compiler}")
   else()
      set(command "${compiler}")
   endif()
   execute_process(COMMAND ${command} "/DAPP_VERSION=${appVersion}"
                           "/DAPP_PUBLISHER=${FLACCD_PUBLISHER}" "/DPRODUCT_NAME=${FLACCD_PRODUCT_NAME}"
                           "/DMANIFEST_FILE=${manifest}" "/DPAYLOAD_DIR=${payload}" "/DOUTPUT_DIR=${output}"
                           "${script}"
                   RESULT_VARIABLE compiled OUTPUT_VARIABLE compilerOutput ERROR_VARIABLE compilerError TIMEOUT 600)
   set(${resultVariable} "${compiled}" PARENT_SCOPE)
   set(${logVariable} "${compilerOutput}\n${compilerError}" PARENT_SCOPE)
endfunction()
