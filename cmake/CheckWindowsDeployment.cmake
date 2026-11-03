cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_HOST_WIN32)
   message(FATAL_ERROR "Windows CMD/PowerShell deployment smoke tests require a Windows host")
endif()
if(NOT configFile)
   # CTest wrapper preserves the chosen multi-config configuration.
   set(command "${CMAKE_COMMAND}" --build "${buildRoot}" --target flaccd-smoke)
   if(configuration)
      list(APPEND command --config "${configuration}")
   endif()
   execute_process(COMMAND ${command} RESULT_VARIABLE result)
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "Windows deployment smoke target failed: ${result}")
   endif()
   return()
endif()
include("${configFile}")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DconfigFile=${configFile}" -DdeployMode=verify
                        -P "${CMAKE_CURRENT_LIST_DIR}/DeployWindows.cmake"
                RESULT_VARIABLE audited)
if(NOT audited EQUAL 0)
   message(FATAL_ERROR "Isolated PE closure audit failed before runtime smoke tests")
endif()
set(smokeDirectory "${deployBase}/Smoke")
file(MAKE_DIRECTORY "${smokeDirectory}")
# Literal relative names plus an explicit working directory avoid interpolating
# user filenames/paths into shell code. setlocal never changes global PATH.
file(WRITE "${smokeDirectory}/Smoke.cmd" [=[@echo off
setlocal
set "PATH=%SystemRoot%\System32;%SystemRoot%"
"..\FlacCD\flaccd-info.exe" --help
if not "%ERRORLEVEL%"=="0" exit /b 1
"..\FlacCD\flaccd-export.exe" --help
if not "%ERRORLEVEL%"=="0" exit /b 1
"..\FlacCD\flaccd-bin-cue.exe" --help
if not "%ERRORLEVEL%"=="0" exit /b 1
"..\FlacCD\flaccd.exe" --help
if not "%ERRORLEVEL%"=="0" exit /b 1
rem Invalid startup arguments exercise the agent loader without starting it.
"..\FlacCD\flaccd-agent.exe" --invalid-smoke-option
if not "%ERRORLEVEL%"=="1" exit /b 1
exit /b 0
]=])
set(powerShellCode [=[
$ErrorActionPreference = 'Stop'
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
foreach ($name in @('flaccd-info.exe', 'flaccd-export.exe', 'flaccd-bin-cue.exe', 'flaccd.exe')) {
   $program = Join-Path (Get-Location) "..\FlacCD\$name"
   & $program --help
   if ($LASTEXITCODE -ne 0) { throw "Startup failed: $name ($LASTEXITCODE)" }
}
$agent = Join-Path (Get-Location) '..\FlacCD\flaccd-agent.exe'
& $agent --invalid-smoke-option
if ($LASTEXITCODE -ne 1) { throw "Agent loader/startup check failed ($LASTEXITCODE)" }
exit 0
]=])
set(systemRoot "$ENV{SystemRoot}")
if(NOT systemRoot)
   message(FATAL_ERROR "Windows SystemRoot is not available")
endif()
execute_process(
   COMMAND "${smokeDirectory}/Smoke.cmd"
   WORKING_DIRECTORY "${smokeDirectory}"
   TIMEOUT 30
   RESULT_VARIABLE cmdResult
   OUTPUT_VARIABLE cmdOutput
   ERROR_VARIABLE cmdError
)
#execute_process(COMMAND "${systemRoot}/System32/cmd.exe" /D /C Smoke.cmd
#WORKING_DIRECTORY "${smokeDirectory}" TIMEOUT 30
#RESULT_VARIABLE cmdResult OUTPUT_VARIABLE cmdOutput ERROR_VARIABLE cmdError)
file(WRITE "${smokeDirectory}/Cmd.txt" "${cmdOutput}\n${cmdError}")
if(NOT cmdResult EQUAL 0)
   message(FATAL_ERROR "Clean-PATH CMD startup failed (${cmdResult}); see ${smokeDirectory}/Cmd.txt")
endif()
execute_process(COMMAND "${systemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
                        -NoLogo -NoProfile -NonInteractive -Command "${powerShellCode}"
                WORKING_DIRECTORY "${smokeDirectory}" TIMEOUT 30
                RESULT_VARIABLE psResult OUTPUT_VARIABLE psOutput ERROR_VARIABLE psError)
file(WRITE "${smokeDirectory}/PowerShell.txt" "${psOutput}\n${psError}")
if(NOT psResult EQUAL 0)
   message(FATAL_ERROR "Clean-PATH PowerShell startup failed (${psResult}); see ${smokeDirectory}/PowerShell.txt")
endif()
message(STATUS "All five deployed Windows executables started under CMD and PowerShell without MSYS2 in PATH")
