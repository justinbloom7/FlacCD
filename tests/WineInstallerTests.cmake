cmake_minimum_required(VERSION 3.20)
set(root "${testDirectory}/Wine package 音楽 & spaces")
file(MAKE_DIRECTORY "${root}/prefix" "${root}/input")
file(WRITE "${root}/prefix/system.reg" "#arch=win64\n")
file(WRITE "${root}/fixture.s" ".text\n.global entry\nentry:\n ret\n")
function(run)
   execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
   if(NOT result EQUAL 0)
      message(FATAL_ERROR "Fixture failed: ${output}${error}")
   endif()
endfunction()
run("${assembler}" --64 -o "${root}/fixture.o" "${root}/fixture.s")
run("${objcopy}" -O pe-x86-64 "${root}/fixture.o" "${root}/fixture.pe.o")
run("${linker}" -mi386pep --entry entry -o "${root}/fixture.exe" "${root}/fixture.pe.o")
set(executables "")
foreach(name flaccd flaccd-agent flaccd-info flaccd-export flaccd-bin-cue)
   configure_file("${root}/fixture.exe" "${root}/input/${name}.exe" COPYONLY)
   list(APPEND executables "${root}/input/${name}.exe")
endforeach()
set(configFile "${root}/Deploy.cmake")
file(WRITE "${configFile}" "set(peObjdump [==[${peObjdump}]==])\nset(deployExecutables [==[${executables}]==])\nset(deployBase [==[${root}/deploy]==])\nset(sourceRoot [==[${sourceRoot}]==])\n")
run("${CMAKE_COMMAND}" "-DconfigFile=${configFile}" -P "${sourceRoot}/cmake/DeployWindows.cmake")
file(WRITE "${root}/winepath" [=[#!/bin/sh
if [ "$MOCK_MODE" = path-failure ]; then exit 7; fi
printf 'Z:%s\n' "$2"
]=])
file(WRITE "${root}/wine" [=[#!/bin/sh
compiler=${1#Z:}
shift
exec "$compiler" "$@"
]=])
file(WRITE "${root}/ISCC mock.exe" [=[#!/bin/sh
printf '%s\n' "$@" > "$MOCK_ARGUMENTS"
printf '%s\n' "$WINEPREFIX" > "$MOCK_PREFIX"
printf 'Mock Inno compiler log\n'
if [ "$MOCK_MODE" = failure ]; then exit 9; fi
for argument do
   case "$argument" in
      /DOUTPUT_DIR=*) output=${argument#/DOUTPUT_DIR=Z:} ;;
      /DAPP_VERSION=*) version=${argument#/DAPP_VERSION=} ;;
      /DMANIFEST_FILE=*) manifest=${argument#/DMANIFEST_FILE=Z:} ;;
   esac
done
cp "$manifest" "$MOCK_MANIFEST"
if [ "$MOCK_MODE" = missing ]; then exit 0; fi
if [ "$MOCK_MODE" = invalid ]; then
   printf 'MZbroken' > "$output/FlacCD-$version-windows-x64-setup.exe"
else
   cp "$MOCK_PE" "$output/FlacCD-$version-windows-x64-setup.exe"
fi
]=])
foreach(name wine winepath "ISCC mock.exe")
   file(CHMOD "${root}/${name}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endforeach()
function(compile mode expected)
   execute_process(COMMAND "${CMAKE_COMMAND}" -E env
      "MOCK_MODE=${mode}" "MOCK_ARGUMENTS=${root}/Arguments.txt" "MOCK_PREFIX=${root}/Prefix.txt"
      "MOCK_PE=${root}/fixture.exe" "MOCK_MANIFEST=${root}/CapturedManifest.txt"
      "${CMAKE_COMMAND}" "-DconfigFile=${configFile}" "-Discc=${root}/ISCC mock.exe"
      "-DwineExecutable=${root}/wine" "-DwinepathExecutable=${root}/winepath"
      "-DwinePrefix=${root}/prefix" -DappVersion=0.1.0 "-DinstallerOutput=${root}/output"
      -P "${sourceRoot}/cmake/BuildWindowsInstaller.cmake"
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
   if(expected STREQUAL "success")
      if(NOT result EQUAL 0)
         message(FATAL_ERROR "Wine packaging rejected valid fixture: ${output}${error}")
      endif()
   elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected}")
      message(FATAL_ERROR "Expected ${expected}, got ${result}: ${output}${error}")
   endif()
endfunction()
compile(success success)
set(installer "${root}/output/FlacCD-0.1.0-windows-x64-setup.exe")
file(SHA256 "${installer}" originalHash)
file(READ "${root}/Arguments.txt" arguments)
foreach(expected "/DAPP_PUBLISHER=Evenstar Systems" "/DPRODUCT_NAME=FlacCD"
                 "/DPAYLOAD_DIR=Z:${root}/deploy/FlacCD" "Z:${sourceRoot}/installer/FlacCD.iss")
   string(FIND "${arguments}" "${expected}\n" found)
   if(found LESS 0)
      message(FATAL_ERROR "ISCC argument was split, unconverted or lost: ${expected}")
   endif()
endforeach()
file(READ "${root}/Prefix.txt" prefix)
if(NOT prefix STREQUAL "${root}/prefix\n")
   message(FATAL_ERROR "Compiler escaped the isolated Wine prefix")
endif()
file(READ "${root}/CapturedManifest.txt" manifest)
if(NOT manifest MATCHES "flaccd-agent.exe\\|[0-9a-f]+")
   message(FATAL_ERROR "Installer did not receive the existing ownership manifest")
endif()
foreach(mode failure missing invalid path-failure)
   compile("${mode}" "compilation failed|PE inspection failed|winepath failed")
   file(SHA256 "${installer}" remainingHash)
   if(NOT remainingHash STREQUAL originalHash)
      message(FATAL_ERROR "Failed ${mode} packaging replaced the prior complete installer")
   endif()
endforeach()
file(READ "${installer}.sha256" hashFile)
if(NOT hashFile MATCHES "${originalHash}")
   message(FATAL_ERROR "Installer checksum missing or inconsistent")
endif()
# Rerun after failures: no stale temporary result can count as success.
compile(success success)
file(WRITE "${root}/prefix/system.reg" "#arch=win32\n")
compile(success "win64 Wine prefix")
message(STATUS "Wine path/argument isolation, manifests, output validation and failure publication passed")
