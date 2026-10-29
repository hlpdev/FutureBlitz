# fb_source_commit(folder out): the short commit of the git checkout whose top folder is `folder`,
# in `out`, or "unknown" without git, or where `folder` isn't a checkout's top, as a source
# archive unpacked inside another checkout isn't.

find_package(Git QUIET)

function(fb_source_commit folder out)
  set(${out} unknown PARENT_SCOPE)
  if(NOT GIT_FOUND)
    return()
  endif()

  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-toplevel --short=7 HEAD
    WORKING_DIRECTORY "${folder}"
    OUTPUT_VARIABLE lines OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    return()
  endif()

  string(REPLACE "\n" ";" lines "${lines}")
  list(LENGTH lines count)
  if(NOT count EQUAL 2)
    return()
  endif()

  list(GET lines 0 top)
  list(GET lines 1 commit)
  file(REAL_PATH "${top}" top)
  file(REAL_PATH "${folder}" folder)
  if(top STREQUAL folder AND commit MATCHES "^[0-9a-f]+$")
    set(${out} "${commit}" PARENT_SCOPE)
  endif()
endfunction()
