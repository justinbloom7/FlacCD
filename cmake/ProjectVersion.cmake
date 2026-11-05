# Source archives and untagged development builds need a numeric base version.
function(resolveFlaccdVersion sourceDirectory result)
   set(version 0.1.1)
   find_package(Git QUIET)
   if(Git_FOUND)
      execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${sourceDirectory}" rev-parse --show-prefix
                      RESULT_VARIABLE found OUTPUT_VARIABLE prefix OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      # Do not adopt a containing repository's tag for an unpacked source tree.
      # An empty prefix identifies the repository root without comparing MSYS
      # and native Windows spellings of a drive path.
      if(found EQUAL 0 AND prefix STREQUAL "")
         execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${sourceDirectory}" tag --points-at HEAD --list "v[0-9]*"
                         RESULT_VARIABLE found OUTPUT_VARIABLE tags OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
         if(found EQUAL 0)
            string(REPLACE "\n" ";" tags "${tags}")
            set(versions "")
            foreach(tag IN LISTS tags)
               if(tag MATCHES "^v([0-9]+\\.[0-9]+\\.[0-9]+)$")
                  list(APPEND versions "${CMAKE_MATCH_1}")
               endif()
            endforeach()
            list(LENGTH versions count)
            if(count GREATER 1)
               message(FATAL_ERROR "Multiple release tags on HEAD: ${versions}; keep one unambiguous version tag")
            elseif(count EQUAL 1)
               list(GET versions 0 version)
            endif()
         endif()
      endif()
   endif()
   set(${result} "${version}" PARENT_SCOPE)
endfunction()
